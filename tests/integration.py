"""Native-player and loopback HTTP regression fixtures; no receiver required."""

import argparse
import base64
import copy
import gzip
import http.server
import json
import math
import os
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
import zlib

import h264_avcc_mp4


def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}):\n{result.stderr.decode()}")
    return result


def read_fifo(fd):
    try:
        return os.read(fd, 65536)
    except BlockingIOError:
        return b""


def pgs_cues(path, cues):
    """A PGS (.sup) track: a 32x8 picture in two colours at each cue start, cleared at its end."""
    def segment(seconds, kind, payload):
        return b"PG" + struct.pack(">IIBH", int(seconds * 90000), 0, kind, len(payload)) + payload
    window = struct.pack(">BBHHHH", 1, 0, 100, 400, 32, 8)
    palette = struct.pack(">BB", 0, 0) + bytes([1, 235, 128, 128, 255, 2, 81, 240, 90, 192])
    rle = b"\x00\x90\x01\x00\x90\x02\x00\x00" * 8  # 16 pixels colour 1, 16 colour 2, end of line
    picture = (struct.pack(">HBB", 0, 0, 0xC0) + (len(rle) + 4).to_bytes(3, "big")
               + struct.pack(">HH", 32, 8) + rle)
    data, number = b"", 0
    for start, end in cues:
        data += (segment(start, 0x16, struct.pack(">HHBHBBBBHBBHH", 720, 576, 0x10, number, 0x80,
                                                  0, 0, 1, 0, 0, 0, 100, 400))
                 + segment(start, 0x17, window) + segment(start, 0x14, palette)
                 + segment(start, 0x15, picture) + segment(start, 0x80, b""))
        data += (segment(end, 0x16, struct.pack(">HHBHBBBB", 720, 576, 0x10, number + 1, 0, 0, 0, 0))
                 + segment(end, 0x17, window) + segment(end, 0x80, b""))
        number += 2
    path.write_bytes(data)


class Player:
    def __init__(self, executable, *args, env=None):
        self.events = queue.Queue()
        self.messages = []
        self.raw_stderr = []
        self.diagnostic_error = False
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [executable, *args], stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.PIPE, text=True, env=env)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stderr:
            self.raw_stderr.append(line)
            try:
                message = json.loads(line)
            except json.JSONDecodeError as error:
                self.diagnostic_error = True
                self.events.put(error)
                continue
            self.messages.append(message)
            self.events.put(message)

    def wait(self, name):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                event = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if isinstance(event, Exception):
                raise event
            if name in event:
                return event[name]
        raise AssertionError(f"Missing event {name}: {self.messages}")

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def stop(self):
        output = ""
        try:
            if self.process.poll() is None and not self.diagnostic_error:
                try:
                    self.process.stdin.write("q\n")
                    self.process.stdin.flush()
                except BrokenPipeError:
                    pass
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise
        finally:
            self.reader.join(timeout=2)
            self.process.stdin.close()
            self.process.stderr.close()
            self.log.seek(0)
            output = self.log.read().decode(errors="replace")
            self.log.close()
        assert self.process.returncode == 0, (
            self.process.returncode, self.messages, output[-6000:], "".join(self.raw_stderr)[-6000:])


drm_last_log = ""
drm_last_rc = (None, False)
drm_last_end = None  # seconds from the start until the player ended by itself


def video_ms(messages):
    """the largest video position the player reported (j)"""
    return max((m["J"]["ms"] for m in messages if "J" in m), default=0)


def drm_play(args, player_args, answers=None, seconds=8, extra_pairs=(), before=()):
    """Plays with -K and the front end answering DRM_KEY_REQ from answers
    (kid -> "key" or "-"; a missing kid is not answered). extra_pairs go
    into the dk line in front of the answer, the commands of before are
    sent ahead of it. Returns the messages and the left channel of the
    exported PCM."""
    fifo = Path(args.pcm_fifo)
    os.mkfifo(fifo, 0o600)
    reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    pcm = bytearray()
    player = Player(args.player, "-K", *player_args)
    answered = 0
    asked_j = 0
    started = time.monotonic()
    global drm_last_end
    drm_last_end = None
    try:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and len(pcm) < 300000:
            pcm.extend(read_fifo(reader))
            if time.monotonic() - asked_j > 0.5 and player.process.poll() is None:
                try:
                    player.command("j")
                except BrokenPipeError:
                    pass
                asked_j = time.monotonic()
            while answered < len(player.messages):
                request = player.messages[answered].get("DRM_KEY_REQ")
                answered += 1
                if request and answers is not None and request["kid"] in answers:
                    for text in before:
                        player.command(text)
                    player.command(" ".join(["dk", *extra_pairs, f"{request['kid']}:{answers[request['kid']]}"]))
            if player.process.poll() is not None:
                drm_last_end = time.monotonic() - started
                pcm.extend(read_fifo(reader))
                break
            time.sleep(0.005)
    finally:
        global drm_last_log, drm_last_rc
        player.log.seek(0)
        drm_last_log = player.log.read().decode(errors="replace")[-4000:]
        stopped_by_itself = player.process.poll() is not None
        try:
            player.stop()
        except AssertionError:
            pass  # checked by the caller with drm_last_rc
        drm_last_rc = (player.process.returncode, stopped_by_itself)
        os.close(reader)
        fifo.unlink()
    lefts, offset = [], 0
    while offset + 16 <= len(pcm):
        _, _, channels, _, size = struct.unpack_from("<IIHHI", pcm, offset)
        frame = pcm[offset + 16:offset + 16 + size]
        count = len(frame) // 4 * 2
        lefts.extend(struct.unpack(f"<{count}h", frame[:count * 2])[0::2])
        offset += 16 + size
    return player.messages, lefts


def sine_played(lefts):
    """the 500 Hz sine of the test audio (peak 8000) and not noise: about
    two zero crossings every 96 samples"""
    if len(lefts) < 8192:
        return False
    part = lefts[len(lefts) // 2 - 2400:][:4800]
    crossings = sum(1 for a, b in zip(part, part[1:]) if (a < 0) != (b < 0))
    return 4000 < max(abs(value) for value in part) < 12000 and 80 <= crossings <= 120


def check_drm_keys(args, root, audio, video, resources, base, seen):
    """--key, --key-request (DRM_KEY_REQ / dk), with a front end simulator,
    and the OE-A options after .mpd in the url. seen: the (path, start) of
    the HTTP requests."""
    caps = json.loads(subprocess.run(
        [args.player], capture_output=True, timeout=5).stderr.splitlines()[0])["EPLAYER3_EXTENDED"]
    assert caps.get("drmkey") in (1, 3), caps
    kid_a, key_a = "00112233445566778899aabbccddeeff", "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
    kid_v, key_v = "ffeeddccbbaa99887766554433221100", "a0b1c2d3e4f5061728394a5b6c7d8e9f"
    widevine = "edef8ba979d64acea3c827dcd51d21ed"

    clear = root / "drm-clear.m4a"
    run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(audio),
        "-t", "8", "-c:a", "aac", "-b:a", "128k", str(clear))
    single = root / "drm.m4a"
    run(args.ffmpeg, "-y", "-i", str(clear), "-c", "copy", "-encryption_scheme", "cenc-aes-ctr",
        "-encryption_key", key_a, "-encryption_kid", kid_a, str(single))

    # DASH: video and audio, each with its own KID (the dash muxer encrypts
    # all representations with one key: one MPD each, joined)
    ns = {"d": "urn:mpeg:dash:schema:mpd:2011", "cenc": "urn:mpeg:cenc:2013"}
    ET.register_namespace("", ns["d"])
    ET.register_namespace("cenc", ns["cenc"])
    # the same layout in the clear (/drm-clear.mpd): whether the DASH
    # demuxer of this FFmpeg plays it at all
    box = b"\x00\x00\x00\x00" + bytes.fromhex(widevine) + (8).to_bytes(4, "big") + b"ep3 test"
    box = (len(box) + 8).to_bytes(4, "big") + b"pssh" + box
    kid_b, key_b = "1234567890abcdef1234567890abcdef", "00ff11ee22dd33cc44bb55aa66997788"
    sets = {}
    for variant in ("", "-clear"):
        joined = None
        for name, source, codec, kid, key in (
                ("v", ["-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
                       "-framerate", "25", "-i", str(video)], ["-c:v", "mpeg2video", "-g", "25"], kid_v, key_v),
                ("a", ["-i", str(clear)], ["-c", "copy"], kid_a, key_a),
                ("b", ["-i", str(clear)], ["-c", "copy"], kid_b, key_b),
                ("c", ["-i", str(clear)], ["-c", "copy"], kid_a, key_a)):
            if variant and name in ("b", "c"):
                continue
            prefix = f"drm{variant}-{name}"
            directory = root / prefix
            directory.mkdir()
            encryption = [] if variant else [
                "-format_options", f"encryption_scheme=cenc-aes-ctr:encryption_key={key}:encryption_kid={kid}"]
            run(args.ffmpeg, "-y", *source, *codec, "-t", "8", "-f", "dash", "-seg_duration", "1",
                "-use_template", "1", "-use_timeline", "1",
                "-init_seg_name", "init.m4s", "-media_seg_name", "seg-$Number%05d$.m4s",
                *encryption, str(directory / "stream.mpd"))
            for file in directory.iterdir():
                resources[f"/{prefix}/{file.name}"] = file.read_bytes()
            mpd = ET.parse(directory / "stream.mpd").getroot()
            adaptation = mpd.find(".//d:AdaptationSet", ns)
            for template in adaptation.iter(f"{{{ns['d']}}}SegmentTemplate"):
                template.set("initialization", f"{prefix}/" + template.get("initialization"))
                template.set("media", f"{prefix}/" + template.get("media"))
            for representation in adaptation.findall("d:Representation", ns):
                representation.set("id", name)
            if joined is None:
                joined = mpd
                period = mpd.find(".//d:Period", ns)
            elif name in ("b", "c"):
                sets[name] = adaptation
            else:
                period.append(adaptation)
        if not variant:
            protection = ET.Element(f"{{{ns['d']}}}ContentProtection", schemeIdUri=f"urn:uuid:{widevine}")
            ET.SubElement(protection, f"{{{ns['cenc']}}}pssh").text = base64.b64encode(box).decode()
            period.find("d:AdaptationSet", ns).insert(0, protection)
        resources[f"/drm{variant}.mpd"] = ET.tostring(joined)
        if not variant:
            # a second audio track with its own KID: drm2.mpd; with the KID
            # of the first: drm4.mpd
            shared = copy.deepcopy(joined)
            period.append(sets["b"])
            resources["/drm2.mpd"] = ET.tostring(joined)
            shared.find(".//d:Period", ns).append(sets["c"])
            resources["/drm4.mpd"] = ET.tostring(shared)

    # the video (KID kid_v) encrypted with the key of the audio: drm-one.mpd
    # has one key for every KID (the OE-A &5=<key> after .mpd in the url)
    directory = root / "drm-o"
    directory.mkdir()
    run(args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
        "-framerate", "25", "-i", str(video), "-c:v", "mpeg2video", "-g", "25", "-t", "8",
        "-f", "dash", "-seg_duration", "1", "-use_template", "1", "-use_timeline", "1",
        "-init_seg_name", "init.m4s", "-media_seg_name", "seg-$Number%05d$.m4s",
        "-format_options", f"encryption_scheme=cenc-aes-ctr:encryption_key={key_a}:encryption_kid={kid_v}",
        str(directory / "stream.mpd"))
    for file in directory.iterdir():
        resources[f"/drm-o/{file.name}"] = file.read_bytes()
    resources["/drm-one.mpd"] = resources["/drm.mpd"].replace(b"drm-v/", b"drm-o/")

    # MP4, video and audio with their own KID: encrypted with one key, the
    # KID of the audio track (the second tenc) changed afterwards
    kid_m = "abcdef0123456789abcdef0123456789"
    both = root / "drm-both.mp4"
    run(args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
        "-framerate", "25", "-i", str(video), "-i", str(clear), "-map", "0:v", "-map", "1:a",
        "-c:v", "mpeg2video", "-g", "25", "-c:a", "copy", "-t", "8",
        "-encryption_scheme", "cenc-aes-ctr", "-encryption_key", key_v, "-encryption_kid", kid_v, str(both))
    data = bytearray(both.read_bytes())
    tenc = [i for i in range(len(data) - 4) if data[i:i + 4] == b"tenc"]
    assert len(tenc) == 2 and data[tenc[1] + 12:tenc[1] + 28] == bytes.fromhex(kid_v), tenc
    data[tenc[1] + 12:tenc[1] + 28] = bytes.fromhex(kid_m)
    both.write_bytes(bytes(data))

    def requests(messages):
        return [m["DRM_KEY_REQ"] for m in messages if "DRM_KEY_REQ" in m]

    def results(messages):
        return {m["DRM_KEY"]["kid"]: m["DRM_KEY"]["sts"] for m in messages if "DRM_KEY" in m}

    def drops(messages):
        return [(m["DRM_KEY_DROP"]["kid"], m["DRM_KEY_DROP"]["type"], m["DRM_KEY_DROP"]["switch"])
                for m in messages if "DRM_KEY_DROP" in m]

    # Whether this FFmpeg itself reads the encrypted DASH stream with the
    # keys (ffmpeg -cenc_decryption_keys, packets against the clear copy):
    # release/6.1-patched reads almost nothing of it (1 of 576 packets in
    # CI), so the player cannot either and the DASH cases are skipped
    # there. With an FFmpeg that can request keys (9.0 series) it must read
    # all of them.
    def packets(*options):
        result = subprocess.run([args.ffmpeg, "-hide_banner", "-v", "error", *options,
                                 "-map", "0", "-c", "copy", "-f", "framecrc", "-"],
                                capture_output=True, timeout=60)
        return sum(1 for line in result.stdout.splitlines() if line and not line.startswith(b"#"))
    clear_packets = packets("-i", base + "/drm-clear.mpd")
    keyed_packets = packets("-cenc_decryption_keys", f"{kid_a}={key_a}:{kid_v}={key_v}",
                            "-i", base + "/drm.mpd")
    dash = clear_packets > 100 and keyed_packets >= clear_packets - 4
    assert dash or not caps["drmkey"] & 2, ("ffmpeg reads encrypted DASH", clear_packets, keyed_packets)
    if not dash:
        print(f"SKIP CENC keys, DASH cases: ffmpeg of this FFmpeg reads {keyed_packets} of "
              f"{clear_packets} packets of the encrypted DASH stream with cenc_decryption_keys")

    # preloaded keys: no request, on every FFmpeg (decryption_keys,
    # cenc_decryption_keys)
    for target, keys in ((str(single), [f"{kid_a}:{key_a}"]),
                         (base + "/drm.mpd", [f"{kid_a.upper()}:{key_a}", f"{kid_v}:{key_v.upper()}"])):
        if "drm.mpd" in target and not dash:
            continue
        options = [option for key in keys for option in ("--key", key)]
        messages, lefts = drm_play(args, [*options, target])
        assert not requests(messages), messages
        assert sine_played(lefts), (target, len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    for wrong in ("1234:5678", f"{kid_a}{key_a}", f"{kid_a}:{key_a[:-1]}g"):
        invalid = subprocess.run([args.player, "--key", wrong, str(single)], capture_output=True, timeout=5)
        assert invalid.returncode != 0 and b"--key: " in invalid.stdout, (wrong, invalid.stdout)
    print("PASS CENC keys --key: MP4" + (" and DASH (a KID per track)" if dash else "")
          + " play with the preloaded keys, no request; malformed --key refused")

    # OE-A options after .mpd in the url ("mpdopt"): each &<0|1|5|6|7|8>=
    # is taken off, the rest of the url (a query string around them) is
    # opened as it was; &50= is not one of them
    assert caps.get("mpdopt") == 1, caps
    wrong = "0123456789abcdef0123456789abcdef"
    for suffix, expected in ((f"&5={key_a}", ""),
                             (f"?a=b&50=x&c=d&0=0&1=0&5={key_a}&8=u&e=f", "?a=b&50=x&c=d&e=f"),
                             ("?a=b&c=d", "?a=b&c=d")):
        seen.clear()
        messages, lefts = drm_play(args, [base + "/drm-clear.mpd" + suffix], seconds=5)
        manifests = {path for path, _ in seen if path.startswith("/drm-clear.mpd")}
        assert manifests == {"/drm-clear.mpd" + expected}, (suffix, seen[:6], drm_last_log)
        assert clear_packets <= 100 or sine_played(lefts), (suffix, len(lefts), messages[-12:], drm_last_log)
    print("PASS OE-A url options: &0= &1= &5= &8= after .mpd taken off the url, "
          "query strings around them kept, &50= and a plain query untouched")

    if dash:
        # &5=: a key for every KID; explicit keys (--key) win over the
        # defaults of the url (a wrong default key would give noise)
        for options, url in (([], f"/drm-one.mpd&5={key_a}"),
                             ([], f"/drm-one.mpd?a=b&5={key_a}&c=d"),
                             ([], f"/drm.mpd&6={key_v}&7={key_a}"),
                             (["--key", f"{kid_v}:{key_v}"], f"/drm.mpd&5={key_a}"),
                             (["--key", f"{kid_a}:{key_a}", "--key", f"{kid_v}:{key_v}"],
                              f"/drm.mpd&5={wrong}&7={wrong}")):
            messages, lefts = drm_play(args, [*options, base + url])
            assert not requests(messages), (url, messages)
            assert not any("FF_ERROR" in m or "DRM_KEY_DROP" in m for m in messages), (url, messages[-12:])
            assert sine_played(lefts), (url, len(lefts), max(map(abs, lefts), default=0), messages[-12:],
                                        drm_last_log)
        print("PASS OE-A url options: DASH plays decrypted with &5= (one key for every KID), "
              "&6= / &7= (video / audio); --key wins over them")

    if dash and caps["drmkey"] & 2:
        # --key-request: a KID a default key of the url applies to is not
        # asked for, the others are
        messages, lefts = drm_play(args, ["--key-request", base + f"/drm-one.mpd&5={key_a}"], {})
        assert not requests(messages), messages
        assert sine_played(lefts), (len(lefts), messages[-12:], drm_last_log)
        messages, lefts = drm_play(args, ["--key-request", base + f"/drm.mpd&7={key_a}"], {kid_v: key_v})
        assert [(r["kid"], r["type"]) for r in requests(messages)] == [(kid_v, "video")], messages
        assert sine_played(lefts), (len(lefts), messages[-12:], drm_last_log)
        messages, lefts = drm_play(args, ["--key-request", base + f"/drm.mpd&6={key_v}"], {kid_a: key_a})
        assert [(r["kid"], r["type"]) for r in requests(messages)] == [(kid_a, "audio")], messages
        assert sine_played(lefts), (len(lefts), messages[-12:], drm_last_log)
        print("PASS OE-A url options with --key-request: only KIDs without a default key "
              "of the url are asked for")

    if not caps["drmkey"] & 2:
        print("SKIP CENC key requests: this FFmpeg has no decryption_key_url")
        return

    # the front end is asked once per KID and answers
    messages, lefts = drm_play(args, ["--key-request", str(single)], {kid_a: key_a})
    assert [(r["kid"], r["type"]) for r in requests(messages)] == [(kid_a, "audio")], messages
    assert results(messages) == {kid_a: 0}, messages
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    messages, lefts = drm_play(args, ["--key-request", base + "/drm.mpd"],
                               {kid_a: key_a.upper(), kid_v: key_v})
    asked = requests(messages)
    assert sorted((r["kid"], r["type"]) for r in asked) == sorted([(kid_a, "audio"), (kid_v, "video")]), messages
    for request in asked:
        assert request["pssh"] == ([{"system": "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                                     "data": base64.b64encode(box).decode()}]
                                   if request["kid"] == kid_v else []), request
    assert results(messages) == {kid_a: 0, kid_v: 0}, messages
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    # one key preloaded, the other asked for
    messages, lefts = drm_play(args, ["--key-request", "--key", f"{kid_v}:{key_v}", base + "/drm.mpd"],
                               {kid_a: key_a})
    assert [r["kid"] for r in requests(messages)] == [kid_a], messages
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    print("PASS CENC key requests: DRM_KEY_REQ per KID with type and MPD pssh, dk answers, "
          "MP4 and DASH play; preloaded KIDs are not asked for")

    # no key: "dk kid:-", no answer (timeout), no --key-request
    messages, lefts = drm_play(args, ["--key-request", str(single)], {kid_a: "-"}, seconds=4)
    assert [r["kid"] for r in requests(messages)] == [kid_a] and results(messages) == {kid_a: -1}, messages
    assert not sine_played(lefts)
    assert drops(messages) == [(kid_a, "audio", -1)], messages
    start = time.monotonic()
    messages, lefts = drm_play(args, ["--key-request", "--key-timeout", "1", str(single)], {}, seconds=4)
    assert [r["kid"] for r in requests(messages)] == [kid_a] and results(messages) == {kid_a: -2}, messages
    assert not sine_played(lefts)
    messages, lefts = drm_play(args, [str(single)], {kid_a: key_a}, seconds=3)
    assert not requests(messages) and not results(messages), messages
    assert not sine_played(lefts)
    print("PASS CENC key requests: dk kid:- and no answer (--key-timeout) give no key; "
          "without --key-request nothing is asked")

    # A track without a key is dropped: audio -> the playback goes on with
    # video (or another audio track that has a key); video -> FF_ERROR and
    # the player stops the playback itself (PLAYBACK_STOP, exit code 0).
    for target, answers, timeout in ((str(both), {kid_v: key_v, kid_m: "-"}, []),
                                     (base + "/drm.mpd", {kid_v: key_v, kid_a: "-"}, []),
                                     (base + "/drm.mpd", {},
                                      ["--key-timeout", "1", "--key", f"{kid_v}:{key_v}"])):
        dropped = kid_m if target == str(both) else kid_a
        messages, lefts = drm_play(args, ["--key-request", *timeout, target], answers, seconds=6)
        assert drops(messages) == [(dropped, "audio", -1)], (target, messages[-12:], drm_last_log)
        assert results(messages)[dropped] == (-2 if timeout else -1), messages
        assert not any("FF_ERROR" in m for m in messages), messages
        assert video_ms(messages) > 2500, (target, video_ms(messages), messages[-12:], drm_last_log)
        assert drm_last_rc == (0, False), (target, drm_last_rc)
    print("PASS CENC key requests: an audio track without a key (dk kid:-, no answer) is dropped "
          "with DRM_KEY_DROP, MP4 and DASH play on with video")

    # two audio tracks, the first without a key: switch to the second
    messages, lefts = drm_play(args, ["--key-request", base + "/drm2.mpd"],
                               {kid_v: key_v, kid_a: "-", kid_b: key_b}, seconds=8)
    dropped = [m["DRM_KEY_DROP"] for m in messages if "DRM_KEY_DROP" in m]
    assert len(dropped) == 1 and dropped[0]["kid"] == kid_a and dropped[0]["switch"] >= 0, messages
    after = messages[messages.index({"DRM_KEY_DROP": dropped[0]}):]
    assert any(m.get("a_c", {}).get("id") == dropped[0]["switch"] for m in after), after
    assert [r["kid"] for r in requests(messages)].count(kid_b) == 1, messages
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    print("PASS CENC key requests: the audio track without a key is dropped, the player switches "
          "to the other audio track and plays it")

    # a dk line with 8 pairs (530 characters), the asked one last: read as
    # one line, not cut at 254 characters into a broken pair and a rest that
    # is taken as a command (all-f keys: "f..." would be PLAYBACK_FASTFORWARD)
    others = [f"{index:032x}:{'f' * 32}" for index in range(1, 8)]
    messages, lefts = drm_play(args, ["--key-request", str(single)], {kid_a: key_a}, extra_pairs=others)
    assert [r["kid"] for r in requests(messages)] == [kid_a] and results(messages) == {kid_a: 0}, messages
    assert not any("PLAYBACK_FASTFORWARD" in m for m in messages), messages
    assert "is not <kid>:<key>" not in drm_last_log, drm_last_log
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    print("PASS CENC key requests: a dk line with 8 pairs is taken whole")

    # the command loop waits for the container lock (al with -C lists the
    # tracks again under it) while the reading thread waits for dk under
    # that lock: dk must get through all the same, not after --key-timeout
    messages, lefts = drm_play(args, ["-C", "--key-request", "--key-timeout", "5", base + "/drm2.mpd"],
                               {kid_v: key_v, kid_a: "-", kid_b: key_b}, seconds=10, before=("al",))
    assert results(messages) == {kid_v: 0, kid_a: -1, kid_b: 0}, messages
    assert [r["kid"] for r in requests(messages)].count(kid_b) == 1, messages
    assert sine_played(lefts), (len(lefts), max(map(abs, lefts), default=0), messages[-12:], drm_last_log)
    print("PASS CENC key requests: dk answers while the command loop waits for the container lock")

    # the video without a key: FF_ERROR naming the KID, PLAYBACK_STOP, exit 0
    for target, answers in ((str(both), {kid_v: "-", kid_m: key_v}),
                            (base + "/drm.mpd", {kid_v: "-", kid_a: key_a})):
        messages, lefts = drm_play(args, ["--key-request", target], answers, seconds=8)
        errors = [m["FF_ERROR"] for m in messages if "FF_ERROR" in m]
        assert errors == [{"msg": f"No key for KID {kid_v} (video)", "code": -13}], (target, messages)
        assert any("PLAYBACK_STOP" in m for m in messages[messages.index({"FF_ERROR": errors[0]}):]), messages
        assert drm_last_rc == (0, True), (target, drm_last_rc, drm_last_log)
    print("PASS CENC key requests: a video without a key ends with FF_ERROR (KID, video) "
          "and PLAYBACK_STOP, exit code 0")

    # no answer at all: one request; after its timeout no other KID is
    # waited for, the video has no key: FF_ERROR, PLAYBACK_STOP soon after
    for target in (str(both), base + "/drm.mpd"):
        messages, lefts = drm_play(args, ["--key-request", "--key-timeout", "2", target], {}, seconds=12)
        assert len(requests(messages)) == 1, (target, messages)
        errors = [m["FF_ERROR"] for m in messages if "FF_ERROR" in m]
        assert errors == [{"msg": f"No key for KID {kid_v} (video)", "code": -13}], (target, messages)
        assert any("PLAYBACK_STOP" in m for m in messages), messages
        assert drm_last_rc == (0, True) and drm_last_end < 2 + 3.5, (target, drm_last_rc, drm_last_end)
    print("PASS CENC key requests: no answer at all: one request, after its timeout FF_ERROR "
          "and PLAYBACK_STOP, no second wait")

    # two audio tracks with the same KID and no key: both dropped, no switch
    # to a dropped track, the video position goes on without a jump
    messages, lefts = drm_play(args, ["--key-request", base + "/drm4.mpd"],
                               {kid_v: key_v, kid_a: "-"}, seconds=7)
    dropped = [m["DRM_KEY_DROP"] for m in messages if "DRM_KEY_DROP" in m]
    assert sorted(d["kid"] for d in dropped) == [kid_a, kid_a], messages
    assert len({d["id"] for d in dropped}) == 2, dropped
    for index, message in enumerate(messages):
        if "DRM_KEY_DROP" in message:
            gone = message["DRM_KEY_DROP"]["id"]
            for later in messages[index + 1:]:
                for name in ("a_s", "a_c"):
                    assert later.get(name, {}).get("id") != gone, (gone, messages)
    positions = [m["J"]["ms"] for m in messages if "J" in m]
    steps = [b - a for a, b in zip(positions, positions[1:])]
    assert positions and positions[-1] > 2500 and all(-100 <= step <= 1500 for step in steps), positions
    assert drm_last_rc == (0, False), drm_last_rc
    print("PASS CENC key requests: two audio tracks with one KID without a key: both dropped, "
          "no switch to a dropped track, the video position without a jump")


# HEVC HDR: a raw HEVC stream written bit by bit. The parameter sets are valid;
# the slices are IDR headers with filler that no decoder is asked to show.
class HevcBits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        self.bits += [(value >> i) & 1 for i in range(count - 1, -1, -1)]

    def ue(self, value):
        code = value + 1
        self.put(0, code.bit_length() - 1)
        self.put(code, code.bit_length())

    def nal(self, nal_type):
        """Annex-B NAL unit with trailing bits and emulation prevention."""
        self.put(1, 1)
        self.put(0, -len(self.bits) % 8)
        out, zeros = bytearray(b"\0\0\0\1" + bytes([nal_type << 1, 1])), 0
        for i in range(0, len(self.bits), 8):
            byte = int("".join(map(str, self.bits[i:i + 8])), 2)
            if zeros >= 2 and byte <= 3:
                out.append(3)
                zeros = 0
            out.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        return bytes(out)


def hevc_profile_tier_level(bits):
    bits.put(1, 8)  # profile space 0, Main tier, Main profile
    bits.put(0x60000000, 32)
    bits.put(9, 4)  # progressive, frame only
    bits.put(0, 44)
    bits.put(60, 8)  # level 2


def hevc_stream(transfer, alternative=None, frames=60):
    vps = HevcBits()
    vps.put(0, 4)
    vps.put(3, 2)
    vps.put(0, 6)  # one layer
    vps.put(0, 3)  # one sub-layer
    vps.put(1, 1)
    vps.put(0xFFFF, 16)
    hevc_profile_tier_level(vps)
    vps.put(1, 1)
    for _ in range(3):
        vps.ue(0)
    vps.put(0, 6)
    vps.ue(0)
    vps.put(0, 2)  # no timing info, no extension
    sps = HevcBits()
    sps.put(0, 4)
    sps.put(0, 3)
    sps.put(1, 1)
    hevc_profile_tier_level(sps)
    for value in (0, 1, 64, 64):  # SPS id, 4:2:0, 64x64
        sps.ue(value)
    sps.put(0, 1)  # no conformance window
    for value in (0, 0, 4):  # 8 bit, 8 bit POC LSB
        sps.ue(value)
    sps.put(1, 1)
    for value in (0, 0, 0, 0, 1, 0, 2, 0, 0):  # DPB; 8x8 CU, 16x16 CTB; 4x4 to 16x16 TU
        sps.ue(value)
    sps.put(0, 4)  # scaling list, AMP, SAO, PCM
    sps.ue(0)  # no short term reference picture sets
    sps.put(0, 3)  # long term, temporal MVP, strong intra smoothing
    sps.put(1, 1)  # VUI
    sps.put(0, 2)
    sps.put(1, 1)  # video signal type
    sps.put(5, 3)
    sps.put(0, 1)
    sps.put(1, 1)  # colour description: BT.2020 primaries and matrix
    sps.put(9, 8)
    sps.put(transfer, 8)
    sps.put(9, 8)
    sps.put(0, 7)
    sps.put(0, 1)
    pps = HevcBits()
    pps.ue(0)
    pps.ue(0)
    pps.put(0, 7)
    for _ in range(3):
        pps.ue(0)
    pps.put(0, 3)
    pps.ue(0)
    pps.ue(0)
    pps.put(0, 10)
    pps.ue(0)
    pps.put(0, 2)
    stream = vps.nal(32) + sps.nal(33) + pps.nal(34)
    if alternative is not None:
        sei = HevcBits()
        sei.put(147, 8)  # alternative transfer characteristics
        sei.put(1, 8)
        sei.put(alternative, 8)
        stream += sei.nal(39)
    idr = HevcBits()
    idr.put(1, 1)  # first slice segment in the picture
    idr.put(0, 1)
    idr.ue(0)  # PPS 0
    idr.ue(2)  # I slice
    idr.ue(0)  # slice QP delta 0
    idr.put(1, 1)  # byte alignment
    idr.put(0, -len(idr.bits) % 8)
    for _ in range(2000):
        idr.put(0xA5, 8)
    return stream + idr.nal(19) * frames


TLS_CONFIG = """[req]
distinguished_name = dn
prompt = no
[dn]
CN = exteplayer3 test CA
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[server]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = IP:127.0.0.1,DNS:localhost
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
"""


def https_cases(args, root, handler, requests, media):
    """CA certificates of https inputs on an image whose OpenSSL finds none
    by itself (VTi: no cert.pem, no hashed links), a loopback HTTPS server
    with a CA of the test. EP3_CA_FILES replaces the list of CA bundles the
    player tries, EP3_CA_DEFAULT_FILE the default file of OpenSSL the player
    checks (a cert.pem of the host has the CAs of the host, not the one of
    the test); SSL_CERT_DIR is an empty directory and SSL_CERT_FILE unset."""
    import shutil
    import ssl
    if b"https" not in run(args.ffmpeg, "-hide_banner", "-protocols").stdout.split():
        print("SKIP https CA bundle: this FFmpeg has no https protocol")
        return
    openssl = shutil.which("openssl")
    if not openssl:
        print("SKIP https CA bundle: no openssl command for the test certificates")
        return
    tls = root / "tls"
    tls.mkdir()
    config = tls / "openssl.cnf"
    config.write_text(TLS_CONFIG)
    for name in ("ca", "other"):
        run(openssl, "req", "-x509", "-config", str(config), "-extensions", "ca",
            "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", f"/CN=exteplayer3 {name} CA",
            "-keyout", str(tls / f"{name}.key"), "-out", str(tls / f"{name}.crt"))
    run(openssl, "req", "-new", "-config", str(config), "-newkey", "rsa:2048", "-nodes",
        "-subj", "/CN=127.0.0.1", "-keyout", str(tls / "server.key"), "-out", str(tls / "server.csr"))
    run(openssl, "x509", "-req", "-in", str(tls / "server.csr"), "-CA", str(tls / "ca.crt"),
        "-CAkey", str(tls / "ca.key"), "-set_serial", "2", "-days", "2", "-extfile", str(config),
        "-extensions", "server", "-out", str(tls / "server.crt"))
    bundle = tls / "ca-certificates.crt"  # the only CA file of a VTi image
    bundle.write_bytes((tls / "other.crt").read_bytes() + (tls / "ca.crt").read_bytes())
    empty = tls / "certs"
    empty.mkdir()
    missing = tls / "missing.crt"

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(tls / "server.crt", tls / "server.key")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"https://127.0.0.1:{server.server_port}"

    def environment(candidates, **extra):
        env = {key: value for key, value in os.environ.items()
               if key not in ("SSL_CERT_FILE", "SSL_CERT_DIR", "EP3_CA_FILES",
                              "EP3_CA_DEFAULT_FILE")}
        env.update(SSL_CERT_DIR=str(empty), EP3_CA_FILES=":".join(map(str, candidates)),
                   EP3_CA_DEFAULT_FILE=str(tls / "cert.pem"), **extra)
        return env

    def opened(options, url, env):
        """PLAYBACK_OPEN sts and the FF_ERROR messages; a player that opened plays."""
        player = Player(args.player, *options, url, env=env)
        status = None
        try:
            status = player.wait("PLAYBACK_OPEN")["sts"]
            if status == 0:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        finally:
            if status == 0:
                player.stop()
            else:
                try:
                    player.process.wait(timeout=10)
                finally:
                    if player.process.poll() is None:
                        player.process.kill()
                        player.process.wait()
                    player.reader.join(timeout=2)
                    player.process.stdin.close()
                    player.process.stderr.close()
                    player.log.close()
        return status, [message["FF_ERROR"] for message in player.messages if "FF_ERROR" in message]

    failed = []
    try:
        # Without a CA the open fails: this FFmpeg verifies by default.
        status, errors = opened([], base + "/master.m3u8", environment([missing]))
        if status == 0:
            print("SKIP https CA bundle: this FFmpeg does not verify certificates by default")
            return

        def case(description, check):
            requests.clear()
            try:
                check()
            except Exception as error:
                failed.append(description)
                print(f"FAIL {description}: {error!r}"[:4000])
            else:
                print("PASS " + description)

        def fallback_hls():
            status, errors = opened([], base + "/master.m3u8", environment([missing, bundle]))
            assert status == 0, errors
            assert ("/high.m3u8", 0) in requests and ("/high.ts", 0) in requests, requests

        case("https HLS without a CA of OpenSSL: the first CA bundle of the list that exists, "
             "also for the variant playlist and the segment", fallback_hls)

        def fallback_separate_audio():
            status, errors = opened(["-x", base + "/sound.ts"], str(media),
                                    environment([missing, bundle]))
            assert status == 0, errors
            assert any(path == "/sound.ts" for path, _ in requests), requests

        case("https -x audio file: the same CA bundle", fallback_separate_audio)

        def ca_file_hls():
            status, errors = opened(["-f", f"ca_file={bundle}"], base + "/master.m3u8",
                                    environment([missing]))
            assert status == 0, errors
            assert ("/high.ts", 0) in requests, requests

        case("https HLS with -f ca_file: the variant playlist and the segment trust it too, "
             "FFmpeg passes the option on to neither", ca_file_hls)

        def ca_file_direct():
            status, errors = opened(["-f", f"ca_file={bundle}"], base + "/media.ts",
                                    environment([missing]))
            assert status == 0, errors

        case("https file with -f ca_file", ca_file_direct)

        def explicit_wins():
            status, errors = opened([], base + "/master.m3u8",
                                    environment([bundle], SSL_CERT_FILE=str(tls / "other.crt")))
            assert status != 0, "opened with the CA of another bundle"
            assert errors, "no FF_ERROR"

        case("https with SSL_CERT_FILE set: it is kept, the certificate of another CA is refused",
             explicit_wins)

        def self_signed_refused():
            status, errors = opened([], base + "/master.m3u8",
                                    environment([missing, tls / "other.crt"]))
            assert status != 0, "opened without the CA of the server in the bundle"

        case("https with a bundle that lacks the CA of the server: verification stays on",
             self_signed_refused)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    assert not failed, failed


# Closed captions: CEA-608 byte pairs (odd parity, control codes sent twice)
# for CC1, carried as ATSC A/53 cc_data() in the video stream or as an SCC file.
CC_RCL, CC_ENM, CC_EOC, CC_EDM, CC_RU2 = (0x14, 0x20), (0x14, 0x2E), (0x14, 0x2F), (0x14, 0x2C), (0x14, 0x25)
CC_PAC15 = (0x14, 0x70)  # row 15, white, column 0
# (seconds, what is sent from then on): a pop-on caption to be seen early, the
# pop-on caption of the test from 4 s to 6 s, a roll-up line from 7 s to 9 s
CC_EVENTS = [(0.4, [CC_RCL, CC_ENM, CC_PAC15, "READY", CC_EOC]), (1.0, [CC_EDM]),
             (3.0, [CC_RCL, CC_ENM, CC_PAC15, "HELLO WORLD"]), (4.0, [CC_EOC]), (6.0, [CC_EDM]),
             (7.0, [CC_RU2, CC_PAC15, "SECOND LINE"]), (9.0, [CC_EDM])]


def cc_byte(value):
    return value | (0 if bin(value).count("1") % 2 else 0x80)


def cc_pairs(items):
    pairs = []
    for item in items:
        if isinstance(item, str):
            for i in range(0, len(item), 2):
                chunk = item[i:i + 2].ljust(2, "\0")
                pairs.append((cc_byte(ord(chunk[0])), cc_byte(ord(chunk[1]))))
        else:
            pairs += [(cc_byte(item[0]), cc_byte(item[1]))] * 2
    return pairs


def cc_frames(count, rate=25, per_frame=2):
    """The byte pairs of each frame in display order, padding where nothing is sent."""
    events = {round(seconds * rate): cc_pairs(items) for seconds, items in CC_EVENTS}
    queue, frames = [], []
    for number in range(count):
        queue += events.get(number, [])
        frames.append(queue[:per_frame] + [(0x80, 0x80)] * (per_frame - len(queue[:per_frame])))
        queue = queue[per_frame:]
    return frames


def cc_ga94(pairs):
    """ATSC_user_data() from the user_identifier on, field 1 triplets."""
    return (b"GA94\x03" + bytes([0x40 | len(pairs), 0xFF])
            + b"".join(bytes([0xFC, hi, lo]) for hi, lo in pairs) + b"\xFF")


def cc_mpeg2(es, frames):
    """MPEG-2 video with the pairs of each picture as user data, by display order."""
    codes, pos = [], es.find(b"\0\0\1")
    while 0 <= pos < len(es) - 3:
        codes.append((pos, es[pos + 3]))
        pos = es.find(b"\0\0\1", pos + 3)
    out, last, base, in_gop, picture, count = bytearray(), 0, 0, 0, None, 0
    for pos, code in codes:
        if code == 0xB8:  # GOP: temporal_reference starts again
            base, in_gop = base + in_gop, 0
        elif code == 0x00:
            picture = base + ((es[pos + 4] << 2) | (es[pos + 5] >> 6))
            in_gop += 1
        elif 0x01 <= code <= 0xAF and picture is not None:
            pairs = frames[picture] if picture < len(frames) else [(0x80, 0x80)]
            out += es[last:pos] + b"\0\0\1\xB2" + cc_ga94(pairs)
            last, picture, count = pos, None, count + 1
    assert count > 200, count
    return bytes(out + es[last:])


def cc_h264(frames):
    """16x16 H.264, one IDR I_PCM picture per frame with its pairs in an SEI."""
    def rbsp(bits):
        bits.put(1, 1)
        bits.put(0, -len(bits.bits) % 8)
        return bytes(int("".join(map(str, bits.bits[i:i + 8])), 2) for i in range(0, len(bits.bits), 8))

    def unit(header, payload):
        out, zeros = bytearray(b"\0\0\0\1" + header), 0
        for byte in payload:
            if zeros >= 2 and byte <= 3:
                out.append(3)
                zeros = 0
            out.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        return bytes(out)

    sps = HevcBits()
    sps.put(66, 8)  # Baseline
    sps.put(0xC0, 8)
    sps.put(30, 8)
    for value in (0, 0, 2, 1):  # SPS id, frame_num 4 bits, POC type 2, one reference
        sps.ue(value)
    sps.put(0, 1)
    sps.ue(0)  # one macroblock wide
    sps.ue(0)  # and high
    sps.put(0b1100, 4)  # frame_mbs_only, direct_8x8_inference, no cropping, no VUI
    pps = HevcBits()
    for value in (0, 0):
        pps.ue(value)
    pps.put(0, 2)  # CAVLC
    for value in (0, 0, 0):
        pps.ue(value)
    pps.put(0, 3)
    for value in (0, 0, 0):  # QP, QS, chroma QP offset
        pps.ue(value)
    pps.put(0, 3)
    parameter_sets = unit(b"\x67", rbsp(sps)) + unit(b"\x68", rbsp(pps))
    stream = b""
    for number, pairs in enumerate(frames):
        t35 = b"\xB5\x00\x31" + cc_ga94(pairs)
        idr = HevcBits()
        for value in (0, 7, 0):  # first macroblock, I slice, PPS 0
            idr.ue(value)
        idr.put(0, 4)  # frame_num
        idr.ue(number % 2)  # idr_pic_id
        idr.put(0, 2)  # no_output_of_prior_pics, long_term_reference
        idr.ue(0)  # slice_qp_delta
        idr.ue(25)  # I_PCM
        idr.put(0, -len(idr.bits) % 8)
        for _ in range(384):
            idr.put(0x80, 8)
        stream += (unit(b"\x09", b"\xF0") + parameter_sets
                   + unit(b"\x06", bytes([4, len(t35)]) + t35 + b"\x80") + unit(b"\x65", rbsp(idr)))
    return stream


def constrained_high_h264(frames):
    """64x64 H.264 High profile with constraint_set4/5 (0x0C), level 4.0,
    CAVLC, SPS and PPS in front of every IDR I_PCM picture."""
    b = h264_avcc_mp4.Bits()
    b.put(100, 8)   # High
    b.put(0x0C, 8)  # constraint_set4_flag, constraint_set5_flag
    b.put(40, 8)    # level 4.0
    b.ue(0)         # seq_parameter_set_id
    b.ue(1)         # chroma_format_idc 4:2:0
    b.ue(0)         # bit_depth_luma_minus8
    b.ue(0)         # bit_depth_chroma_minus8
    b.put(0, 1)     # qpprime_y_zero_transform_bypass_flag
    b.put(0, 1)     # seq_scaling_matrix_present_flag
    b.ue(0)         # log2_max_frame_num_minus4
    b.ue(2)         # pic_order_cnt_type
    b.ue(1)         # max_num_ref_frames
    b.put(0, 1)     # gaps_in_frame_num_value_allowed_flag
    b.ue(3)         # pic_width_in_mbs_minus1
    b.ue(3)         # pic_height_in_map_units_minus1
    b.put(1, 1)     # frame_mbs_only_flag
    b.put(1, 1)     # direct_8x8_inference_flag
    b.put(0, 1)     # frame_cropping_flag
    b.put(0, 1)     # vui_parameters_present_flag
    sps = b.nal(0x67)
    assert sps[:4] == b"\x67\x64\x0c\x28", sps.hex()
    start = b"\0\0\0\1"
    sets = start + sps + start + h264_avcc_mp4.pps(0)
    return b"".join(sets + start + h264_avcc_mp4.idr(n) for n in range(frames))


def cc_scc():
    """The same captions as a Scenarist SCC file: one line per event, 30 frames a second."""
    lines = ["Scenarist_SCC V1.0", ""]
    for seconds, items in CC_EVENTS:
        frames = round(seconds * 30)
        words = " ".join(f"{hi:02x}{lo:02x}" for hi, lo in cc_pairs(items))
        lines += [f"00:00:{frames // 30:02}:{frames % 30:02}\t{words}", ""]
    # the MOV track made from it loses its last sample: padding after the end
    lines += ["00:00:10:00\t8080 8080", ""]
    return "\n".join(lines)


def cc_check(cues, tolerance=40):
    """The pop-on caption from 4 s to 6 s, the roll-up line from 7 s to 9 s,
    written in pieces that meet and never overlap."""
    cues = sorted(cues, key=lambda cue: cue["s"])
    for before, after in zip(cues, cues[1:]):
        assert after["s"] >= before["e"], ("overlap", before, after, cues)
    for cue in cues:
        assert 0 < cue["e"] - cue["s"] <= 540 + tolerance, ("piece", cue, cues)
        assert cue["t"] in ("READY", "HELLO WORLD") or "SECOND LINE".startswith(cue["t"]), cues
    hello = [cue for cue in cues if cue["t"] == "HELLO WORLD"]
    assert hello, cues
    for before, after in zip(hello, hello[1:]):
        assert after["s"] == before["e"], ("gap", before, after, cues)
    end = hello[-1]["e"]
    assert abs(end - hello[0]["s"] - 2000) <= tolerance, cues
    roll = [cue for cue in cues if cue["s"] >= end]
    assert roll and roll[-1]["t"] == "SECOND LINE", cues
    assert 1000 - tolerance <= roll[0]["s"] - end <= 1300, cues
    assert abs(roll[-1]["e"] - end - 3000) <= tolerance, cues


def cc_play(player_path, target, from_video):
    """Plays target, selects its caption track, returns the subtitle track and the cues."""
    player = Player(player_path, str(target))
    try:
        track = None
        while track is None:
            # the captions of the video stream are listed once they come
            tracks = player.wait("s_l")
            track = next((track for track in tracks if track["e"] == "S_TEXT/EIA608"), None)
            assert track is not None or from_video, tracks
        player.command(f"s{track['id']}")  # by id
        assert player.wait("s_s")["sts"] == 0
        player.process.wait(timeout=30)
    finally:
        player.stop()
    return track, [message["s_a"] for message in player.messages
                   if "s_a" in message and message["s_a"]["id"] == track["id"]]


def cc_seek_back(player_path, target):
    """Once the end of the roll-up line (9 s) has been read, a seek back to 5 s
    (between the end of the pop-on caption and the roll-up line): the line must
    come again. A flushed ccaption decoder would keep the time of 9 s and hold
    the line back. Returns the cues sent after the seek."""
    player = Player(player_path, str(target))
    try:
        while not any(track["e"] == "S_TEXT/EIA608" for track in player.wait("s_l")):
            pass
        player.command("s65536")
        assert player.wait("s_s")["sts"] == 0
        deadline = time.monotonic() + 20
        while not any(message.get("s_a", {}).get("t") == "SECOND LINE" and message["s_a"]["e"] >= 8960
                      for message in player.messages):
            assert time.monotonic() < deadline, player.messages
            time.sleep(0.05)
        player.command("gf 5")
        assert player.wait("PLAYBACK_SEEK_ABS")["sts"] == 0
        mark = len(player.messages)
        player.process.wait(timeout=30)
    finally:
        player.stop()
    return [message["s_a"] for message in player.messages[mark:] if "s_a" in message]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--pcm-fifo", help="Isolated path compiled into the host player")
    parser.add_argument("--video-socket", default="/tmp/exteplayer3-host-video.socket",
                        help="Video socket path compiled into the host player")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="exteplayer3-integration-") as directory:
        root = Path(directory)
        video = root / "video.yuv"
        audio = root / "audio.pcm"
        with video.open("wb") as stream:
            for frame in range(300):
                stream.write(bytes(16 + (i * 17 + frame * 11) % 220 for i in range(4096)))
                stream.write(bytes([128]) * 2048)
        samples = b"".join(struct.pack("<hh", int(8000 * math.sin(i * math.pi / 48)),
                                       int(8000 * math.sin(i * math.pi / 48)))
                           for i in range(4800))
        audio.write_bytes(samples * 120)
        media = root / "media.ts"
        run(args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p",
            "-video_size", "64x64", "-framerate", "25", "-i", str(video),
            "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(audio),
            "-c:v", "mpeg2video", "-c:a", "aac", "-b:a", "96k", "-t", "12", str(media))
        title, artist = 'Title"\n\x01', "Artist\\name"
        radio = root / "radio.m4a"
        run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), "-c:a", "aac", "-metadata", f"title={title}",
            "-metadata", f"artist={artist}", "-f", "mp4", str(radio))
        subtitles = root / "subtitles.srt"
        subtitles.write_text("1\n00:00:00,000 --> 00:00:10,000\nHello\n", encoding="utf-8")
        # Every quote takes two bytes in the JSON message: 2200 for this cue.
        cue = '"one"\n' + '"' * 1100
        quoted_cues = root / "quoted.srt"
        quoted_cues.write_text("".join(
            f"{i + 1}\n00:00:{i // 2:02},{i % 2 * 500:03} --> "
            f"00:00:{i // 2:02},{i % 2 * 500 + 400:03}\nSay {cue}\n\n"
            for i in range(24)), encoding="utf-8")
        quoted = []
        for name, codec in (("quoted.mp4", "mov_text"), ("quoted-srt.mkv", "copy"),
                            ("quoted-ass.mkv", "ass")):
            target = root / name
            run(args.ffmpeg, "-y", "-i", str(media), "-f", "srt", "-i", str(quoted_cues),
                "-map", "0:v", "-map", "0:a", "-map", "1", "-c", "copy", "-c:s", codec,
                str(target))
            quoted.append(target)
        embedded = []
        for extension, codec in (("mp4", "mov_text"), ("mkv", "copy")):
            target = root / ("subtitles." + extension)
            run(args.ffmpeg, "-y", "-i", str(media), "-f", "srt", "-i", str(subtitles),
                "-f", "srt", "-i", str(subtitles), "-map", "0:v", "-map", "0:a",
                "-map", "1", "-map", "2", "-c", "copy", "-c:s", codec,
                "-metadata:s:s:0", "language=eng", "-metadata:s:s:1", "language=deu",
                "-disposition:s:0", "forced", "-disposition:s:1", "0", str(target))
            embedded.append(target)
        # mov_text samples carry a length, the text and the style of the cue.
        words = ["Hello", "Hi there", "one\ntwo"]
        word_cues = root / "words.srt"
        word_cues.write_text("".join(
            f"{i + 1}\n00:00:{i // 2:02},{i % 2 * 500:03} --> "
            f"00:00:{i // 2:02},{i % 2 * 500 + 400:03}\n"
            f"{words[i % 3].replace('Hi', '<i>Hi</i>')}\n\n"
            for i in range(24)), encoding="utf-8")
        worded = root / "words.mp4"
        run(args.ffmpeg, "-y", "-i", str(media), "-f", "srt", "-i", str(word_cues),
            "-map", "0:v", "-map", "0:a", "-map", "1", "-c", "copy", "-c:s", "mov_text",
            str(worded))
        named = []
        for name, language in (('na"me.mkv', 'a"b\\c\x01'), ("long.mkv", "x" * 1500)):
            target = root / name
            run(args.ffmpeg, "-y", "-i", str(media), "-c", "copy",
                "-metadata:s:a:0", "language=" + language, str(target))
            named.append((target, language))
        codecs = root / "codecs.mkv"
        run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-map", "0:a", "-map", "0:a",
            "-c:v", "copy", "-c:a:0", "opus", "-c:a:1", "vorbis", "-strict", "-2", str(codecs))
        # Closed captions: MPEG-2 with B-frames and H.264 (Annex B in TS, avcC
        # in MP4) carrying them in the video stream, an SCC file in a MOV track.
        caption_frames = cc_frames(260)
        plain_m2v = root / "plain.m2v"
        run(args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p",
            "-video_size", "64x64", "-framerate", "25", "-i", str(video), "-frames:v", "260",
            "-c:v", "mpeg2video", "-g", "12", "-bf", "2", "-f", "mpeg2video", str(plain_m2v))
        (root / "cc.m2v").write_bytes(cc_mpeg2(plain_m2v.read_bytes(), caption_frames))
        (root / "cc.h264").write_bytes(cc_h264(caption_frames))
        captioned = []
        # raw video has no timestamps: the demuxer makes them for the B-frames
        # of MPEG-2, the H.264 frames are counted
        frame_times = ["-bsf:v", "setts=ts=N/(25*TB)"]
        for source, demuxer, name, options in (
                ("cc.m2v", "mpegvideo", "cc-mpeg2.ts", ["-fflags", "+genpts"]),
                ("cc.h264", "h264", "cc-h264.ts", []), ("cc.h264", "h264", "cc-h264.mp4", [])):
            run(args.ffmpeg, "-y", *options, "-f", demuxer, "-framerate", "25",
                "-i", str(root / source), "-c", "copy",
                *(frame_times if demuxer == "h264" else []), str(root / name))
            captioned.append(root / name)
        (root / "cc.scc").write_text(cc_scc(), encoding="ascii")
        caption_mov = root / "cc.mov"
        run(args.ffmpeg, "-y", "-i", str(media), "-f", "scc", "-i", str(root / "cc.scc"),
            "-map", "0:v", "-map", "1", "-c", "copy", str(caption_mov))
        # what libavcodec makes of them on its own: the test media are right
        for target in captioned[:2]:
            text = subprocess.run(
                [args.ffmpeg, "-v", "error", "-f", "lavfi", "-i", f"movie={target.name}[out0+subcc]",
                 "-map", "0:1", "-f", "srt", "-"], cwd=root, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=60).stdout.decode()
            assert "HELLO WORLD" in text and "SECOND LINE" in text, (target.name, text)

        requests = []
        agents = []  # (path, User-Agent) of every request
        # Accept-Encoding of every request; paths that fail n more times, that
        # carry a Content-Type, that are compressed when gzip is accepted.
        encodings, failures, content_types, compressed = [], {}, {}, set()
        # Paths whose answer waits: (set when it is asked for, set to answer).
        held = {}
        # Paths whose body stops at byte n, the connection kept open without
        # another byte until released (also for requests that start there).
        stalls, stall_release = {}, threading.Event()
        payload = media.read_bytes()
        master = (b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\nlow.m3u8\n"
                  b"#EXT-X-STREAM-INF:BANDWIDTH=900000\nhigh.m3u8\n")
        resources = {"/media.ts": payload, "/low.ts": payload, "/high.ts": payload,
                     "/master.m3u8": master}
        run(args.ffmpeg, "-y", "-stream_loop", "-1", "-i", str(media),
            "-map", "0:v", "-map", "0:a", "-map", "0:a", "-t", "40",
            "-c:v", "mpeg2video", "-g", "12", "-bf", "2", "-c:a", "aac",
            "-single_file", "1", "-seg_duration", "1",
            "-f", "dash", str(root / "single.mpd"))
        ns = {"d": "urn:mpeg:dash:schema:mpd:2011"}
        ET.register_namespace("", ns["d"])
        mpd = ET.parse(root / "single.mpd").getroot()
        for representation in mpd.findall(".//d:Representation", ns):
            segments = representation.find("d:SegmentList", ns)
            segment_base = ET.Element(f"{{{ns['d']}}}SegmentBase")
            segment_base.append(segments.find("d:Initialization", ns))
            representation.remove(segments)
            representation.append(segment_base)
        resources["/single.mpd"] = ET.tostring(mpd)
        for single_file in root.glob("single-stream*.mp4"):
            resources["/" + single_file.name] = single_file.read_bytes()
        for quality in ("low", "high"):
            resources[f"/{quality}.m3u8"] = (
                f"#EXTM3U\n#EXT-X-TARGETDURATION:12\n#EXTINF:12,\n"
                f"{quality}.ts\n#EXT-X-ENDLIST\n").encode()

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError):
                    pass

            def do_GET(self):
                # a query string the resource is not stored with is left out
                data = resources.get(self.path, resources.get(self.path.split("?", 1)[0]))
                if data is None:
                    requests.append((self.path, 0))
                    self.send_error(404)
                    return
                encoding = self.headers.get("Accept-Encoding")
                encodings.append((self.path, encoding))
                gzipped = self.path in compressed and "gzip" in (encoding or "")
                if gzipped:
                    data = gzip.compress(data)
                start = 0
                header = self.headers.get("Range")
                if header:
                    start = int(header.split("=")[1].split("-")[0])
                requests.append((self.path, start))
                agents.append((self.path, self.headers.get("User-Agent")))
                if failures.get(self.path):
                    failures[self.path] -= 1
                    self.send_error(503)
                    return
                # requests behind the stall point (the duration probe at the
                # end of a TS) are answered in full
                if self.path in stalls and start <= stalls[self.path]:
                    self.send_response(206 if header else 200)
                    self.send_header("Content-Length", str(len(data) - start))
                    self.send_header("Accept-Ranges", "bytes")
                    if header:
                        self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
                    self.end_headers()
                    try:
                        self.wfile.write(data[start:stalls[self.path]])
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    stall_release.wait(150)
                    return
                if self.path in held:
                    asked, answer = held[self.path]
                    asked.set()
                    answer.wait(8)
                self.send_response(206 if header else 200)
                self.send_header("Content-Length", str(len(data) - start))
                self.send_header("Accept-Ranges", "bytes")
                if self.path in content_types:
                    self.send_header("Content-Type", content_types[self.path])
                if gzipped:
                    self.send_header("Content-Encoding", "gzip")
                if header:
                    self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
                self.end_headers()
                try:
                    self.wfile.write(data[start:])
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            result = run(args.player + "-http-seek", base + "/media.ts")
            seek = json.loads(result.stdout)
            assert ("/media.ts", seek["position"]) in requests, (seek, requests)
            assert seek["position"] % seek["packet_size"] == 0
            assert abs(seek["actual_us"] - seek["target_us"]) < 3_000_000, seek
            print("PASS HTTP TS: exact aligned Range request and approximate media position")

            for options, chosen, excluded in (([], "high", "low"), (["-Q", "1"], "low", "high")):
                requests.clear()
                player = Player(args.player, *options, base + "/master.m3u8")
                try:
                    assert player.wait("PLAYBACK_OPEN")["sts"] == 0
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    assert player.wait("v_c")["w"] == 64
                finally:
                    started = time.monotonic()
                    player.stop()
                    assert time.monotonic() - started < 2
                assert (f"/{chosen}.ts", 0) in requests, requests
                assert not any(path == f"/{excluded}.ts" for path, _ in requests), requests
            print("PASS HLS: highest Auto / lowest quality fetches, video IPC and bounded shutdown")

            for mode in ("0", "1"):
                player = Player(args.player, base + "/single.mpd",
                                env=dict(os.environ, EP3_LIVE_SYNC=mode))
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    assert len(player.wait("a_l")) == 2
                    player.command("j")
                    assert 0 <= player.wait("J")["ms"] < 2000
                    time.sleep(1.5)
                    for track in (1, 0):
                        player.command("j")
                        before = player.wait("J")["ms"]
                        player.command(f"ai{track}")
                        assert player.wait("a_s")["sts"] == 0
                        time.sleep(1.5)
                        player.command("j")
                        after = player.wait("J")["ms"]
                        assert before + 250 <= after < before + 4000, (before, after)
                finally:
                    player.stop()
            print("PASS single-file DASH: VOD startup and audio switching with live sync off/on")

            for target in embedded:
                player = Player(args.player, str(target))
                try:
                    tracks = player.wait("s_l")
                    assert len(tracks) == 2 and [track["f"] for track in tracks] == [1, 0], tracks
                    player.command("si0\nsc")
                    assert player.wait("s_s")["sts"] == 0
                    current = player.wait("s_c")
                    assert current["id"] == tracks[0]["id"] and current["f"] == 1
                    player.command("si-1\nsc")
                    assert player.wait("s_s")["sts"] == 0
                    current = player.wait("s_c")
                    assert current["id"] == -1 and current["f"] == 0
                finally:
                    player.stop()
            print("PASS MP4/MKV subtitles: forced-list/current JSON, selecting and disabling")

            for target in quoted:
                player = Player(args.player, str(target))
                try:
                    assert len(player.wait("s_l")) == 1
                    player.command("si0")
                    assert player.wait("s_s")["sts"] == 0
                    text = ""
                    while not text:  # mov_text ends every cue with an empty sample
                        text = player.wait("s_a")["t"]
                    assert text.endswith(cue), text[:80]
                finally:
                    player.stop()
            print("PASS MP4/MKV subtitles: long mov_text, SubRip and ASS cues with quotes as JSON")

            # fake_last_subtitle ends a WebVTT stream with a cue at pts INT64_MAX - 2. The
            # first cue may be read before the track is selected.
            webvtt = root / "subtitles.vtt"
            webvtt.write_text("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nEarly\n\n"
                              "00:00:04.000 --> 00:00:05.000\nFirst\n\n"
                              "00:00:06.000 --> 00:00:07.000\nSecond\n", encoding="utf-8")
            player = Player(args.player, "-x", str(webvtt), str(media))
            try:
                tracks = player.wait("s_l")
                assert len(tracks) == 1, tracks
                player.command("si0")
                assert player.wait("s_s")["sts"] == 0
                player.process.wait(timeout=20)
            finally:
                player.stop()
            atoms = [message["s_a"] for message in player.messages if "s_a" in message]
            texts = [atom["t"] for atom in atoms if atom["t"] != "Early"]
            assert texts == ["First", "Second"], atoms
            assert all(0 <= atom["s"] < 60000 for atom in atoms), atoms
            player = Player(args.player, str(worded))
            try:
                assert len(player.wait("s_l")) == 1
                player.command("si0")
                assert player.wait("s_s")["sts"] == 0
                atom = player.wait("s_a")
                while atom["t"]:  # the empty sample that ends a mov_text cue
                    atom = player.wait("s_a")
                assert 0 <= atom["s"] < 60000, atom
            finally:
                player.stop()
            print("PASS WebVTT: no cue without a time at the end; empty mov_text cues still sent")

            player = Player(args.player, str(worded))
            try:
                assert len(player.wait("s_l")) == 1
                player.command("si0")
                assert player.wait("s_s")["sts"] == 0
                seen = set()
                while len(seen) < len(words):
                    text = player.wait("s_a")["t"]
                    assert text in words + [""], text
                    seen.add(text)
                    seen.discard("")
            finally:
                player.stop()
            print("PASS MP4 subtitles: mov_text cues arrive with their whole text and nothing else")

            for target in captioned:
                track, cues = cc_play(args.player, target, True)
                assert track["id"] == 0x10000 and track["t"] == "CC1" and track["n"] == "und", track
                cc_check(cues)
            print("PASS closed captions in MPEG-2 (B-frames) and H.264 (TS, MP4): CC1 listed "
                  "when they come, cues in pieces at the times of the caption")
            # every frame of the H.264 stream is a key frame: the seek lands at 5 s
            cues = cc_seek_back(args.player, captioned[1])
            assert any(cue["t"] == "SECOND LINE" and 7000 <= cue["s"] < 9000 for cue in cues), cues
            print("PASS closed captions after a seek back: the roll-up line comes again")
            track, cues = cc_play(args.player, caption_mov, False)
            cc_check(cues, tolerance=80)
            print("PASS closed captions of an EIA-608 track (MOV c608)")

            # Graphic subtitles with -G; exteplayer3png.so is not installed here.
            sup = root / "cues.sup"
            pgs_cues(sup, [(1, 2), (3, 4), (5, 6), (7, 8)])
            graphic = []
            for name, options, encoding in (("pgs.mkv", ["-c:s", "copy"], "S_GRAPHIC/PGS"),
                                            ("dvb.ts", ["-c:s", "dvbsub"], "S_GRAPHIC/DVB")):
                target = root / name
                fix = ["-fix_sub_duration"] if "dvbsub" in options else []
                run(args.ffmpeg, "-y", "-i", str(media), *fix, "-i", str(sup),
                    "-map", "0:v", "-map", "0:a", "-map", "1", "-c:v", "copy", "-c:a", "copy",
                    *options, str(target))
                graphic.append((target, encoding))
            for target, encoding in graphic:
                frames = root / ("frames-" + target.suffix[1:])
                frames.mkdir()
                player = Player(args.player, "-G", str(frames), str(target))
                try:
                    tracks = player.wait("s_l")
                    assert [track["e"] for track in tracks] == [encoding], tracks
                    player.command("si0")
                    assert player.wait("s_s")["sts"] == 0
                    rects = []
                    while not rects:  # a cleared screen comes without rectangles
                        rects = player.wait("s_a")["r"]
                    for rect in rects:
                        picture = frames / rect["f"]
                        png = picture.read_bytes()
                        assert png[:8] == b"\x89PNG\r\n\x1a\n" and png[12:16] == b"IHDR", png[:16]
                        assert struct.unpack(">II", png[16:24]) == (rect["w"], rect["h"]), rect
                        pixels = run(args.ffmpeg, "-v", "error", "-i", str(picture),
                                     "-f", "rawvideo", "-pix_fmt", "rgba", "-").stdout
                        assert len(pixels) == rect["w"] * rect["h"] * 4 and any(pixels[3::4]), rect
                finally:
                    player.stop()
                assert not any("e_plugin" in message for message in player.messages), player.messages
                assert not list(frames.glob("*.tmp")), list(frames.iterdir())
            print("PASS PGS/DVB graphic subtitles with -G: PNG files and rectangles without the plugin")

            # A subtitle file of its own counts from the start of the TS file, whose
            # timestamps start at about 1.4 s: a cue before that is no exception.
            separate = root / "separate.vtt"
            separate.write_text("WEBVTT\n\n00:00:00.500 --> 00:00:01.500\nEarly\n\n"
                                "00:00:04.000 --> 00:00:05.000\nLater\n", encoding="utf-8")
            player = Player(args.player, "-x", str(separate), str(media))
            try:
                assert len(player.wait("s_l")) == 1
                player.command("si0")
                assert player.wait("s_s")["sts"] == 0
                starts = {}
                while "Later" not in starts:
                    atom = player.wait("s_a")
                    starts.setdefault(atom["t"], atom["s"])
            finally:
                player.stop()
            assert abs(starts.get("Early", -1) - 500) <= 100, starts
            assert abs(starts["Later"] - 4000) <= 100, starts
            print("PASS -x WebVTT file: cues at their time in the TS file, also the one before its first timestamp")

            player = Player(args.player, str(radio))
            try:
                metadata = player.wait("m_t")
                assert metadata == {"rt": artist + " - " + title, "t": title, "a": artist}
                player.process.wait(timeout=10)
            finally:
                player.stop()
            print("PASS radio: full-player metadata JSON with quotes, backslashes and control bytes")

            # Audio only: the rest of the file is read at once after a seek. The
            # fake decoder plays in real time and keeps its old position for 1.5 s
            # after the clear of the seek, as receiver decoders do while they
            # start again. The end of the file waits until it has played the rest.
            player = Player(args.player, str(radio),
                            env=dict(os.environ, EP3_FAKE_AUDIO_CLOCK="1500"))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                time.sleep(1)
                player.command("j")
                assert 300 <= player.wait("J")["ms"] < 3000
                player.command("gf 6")
                assert player.wait("PLAYBACK_SEEK_ABS")["sts"] == 0
                sought = time.monotonic()
                time.sleep(3)
                assert player.process.poll() is None, "ended while the decoder starts again"
                player.command("j")
                position = player.wait("J")["ms"]
                assert 6500 <= position < 9500, position
                player.process.wait(timeout=10)
                ended = time.monotonic() - sought
                # 1.5 s, then the 6 s from 6 s to the end at 12 s
                assert 6.5 <= ended < 9.5, ended
            finally:
                player.stop()
            print("PASS audio only: a seek near the end plays to the end, also when the decoder starts late")

            if args.pcm_fifo:
                fifo = Path(args.pcm_fifo)
                assert not fifo.exists(), "Use a new isolated FIFO path, never a production endpoint"
                os.mkfifo(fifo, 0o600)
                reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                pcm = bytearray()
                player = Player(args.player, "-K", "-a", "0", str(radio))
                try:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        chunk = read_fifo(reader)
                        pcm.extend(chunk)
                        if player.process.poll() is not None:
                            pcm.extend(read_fifo(reader))
                            break
                        time.sleep(0.005)
                    tracks = player.wait("a_l")
                    assert tracks and tracks[0]["e"] in ("A_IPCM", "A_LPCM")
                finally:
                    player.stop()
                    os.close(reader)
                    fifo.unlink()
                offset, nonzero = 0, False
                while offset < len(pcm):
                    magic, rate, channels, bits, size = struct.unpack_from("<IIHHI", pcm, offset)
                    assert magic == 0x504D4341 and rate == 48000 and channels == 2 and bits == 16
                    assert size and size % 4 == 0 and offset + 16 + size <= len(pcm)
                    nonzero |= any(pcm[offset + 16:offset + 16 + size])
                    offset += 16 + size
                assert offset == len(pcm) and nonzero
                print("PASS PCM: software decode despite later -a 0; complete S16LE FIFO frames")

                # Dual mono: a sine on the left, silence on the right.
                dual = root / "dual.m4a"
                left_only = b"".join(struct.pack("<hh", int(8000 * math.sin(i * math.pi / 48)), 0)
                                     for i in range(4800))
                (root / "dual.pcm").write_bytes(left_only * 120)
                run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
                    "-i", str(root / "dual.pcm"), "-c:a", "aac", "-b:a", "128k", str(dual))
                for channel, name in ((1, "stereo"), (0, "left"), (2, "right")):
                    os.mkfifo(fifo, 0o600)
                    reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                    pcm = bytearray()
                    player = Player(args.player, "-K", "--audio-channel", name, str(dual))
                    try:
                        deadline = time.monotonic() + 10
                        asked = False
                        while time.monotonic() < deadline and len(pcm) < 400000:
                            chunk = read_fifo(reader)
                            pcm.extend(chunk)
                            if chunk and not asked:
                                # the command loop runs: the selection it reports
                                player.command("am")
                                asked = True
                            if player.process.poll() is not None:
                                pcm.extend(read_fifo(reader))
                                break
                            time.sleep(0.005)
                        assert asked and player.wait("a_m") == {"m": channel, "sts": 0}
                    finally:
                        player.stop()
                        os.close(reader)
                        fifo.unlink()
                    lefts, rights, offset = [], [], 0
                    while offset + 16 <= len(pcm):
                        _, _, channels, _, size = struct.unpack_from("<IIHHI", pcm, offset)
                        assert channels == 2
                        frame = pcm[offset + 16:offset + 16 + size]
                        count = len(frame) // 4 * 2
                        samples = struct.unpack(f"<{count}h", frame[:count * 2])
                        lefts.extend(samples[0::2])
                        rights.extend(samples[1::2])
                        offset += 16 + size
                    assert len(lefts) >= 4096, (name, len(lefts))
                    peak_left = max(abs(value) for value in lefts)
                    peak_right = max(abs(value) for value in rights)
                    if channel == 1:
                        assert peak_left > 4000 and peak_right < 2000, (peak_left, peak_right)
                    else:
                        assert lefts == rights, name
                        assert (peak_left > 4000) == (channel == 0), (name, peak_left)
                print("PASS PCM dual mono: --audio-channel left/right copy that channel to both, stereo keeps them")

                check_drm_keys(args, root, audio, video, resources, base, requests)

            # am<n>: left/stereo/right of dual mono, am alone reports it; this
            # host output has no decoder, so every valid value gives sts 0.
            player = Player(args.player, str(media))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                player.wait("a_c")
                for command, answer in (("am", {"m": 1, "sts": 0}), ("am2", {"m": 2, "sts": 0}),
                                        ("am7", {"m": 2, "sts": -1}), ("am-1", {"m": 2, "sts": -1}),
                                        ("am0", {"m": 0, "sts": 0}), ("am", {"m": 0, "sts": 0}),
                                        ("am1", {"m": 1, "sts": 0})):
                    player.command(command)
                    assert player.wait("a_m") == answer, (command, player.messages[-3:])
                # the track commands next to it still work
                player.command("ac")
                assert player.wait("a_c")["id"] >= 0
            finally:
                player.stop()
            assert player.messages[0]["EPLAYER3_EXTENDED"]["achannel"] == 1, player.messages[0]
            print("PASS audio channel: am<n> selects and reports left/stereo/right, achannel capability")

            for option in (["-Q", "3"], ["-D", "2"], ["-M", "1920x0"],
                           ["-M", "garbage"], ["-Q", "1junk"], ["--audio-channel", "mono"],
                           ["--audio-channel", "3"]):
                invalid = subprocess.run([args.player, *option, str(media)],
                                         capture_output=True, timeout=5)
                assert invalid.returncode != 0, option
            print("PASS strict CLI validation")

            # FFmpeg reads and decodes raw AC-4 but cannot encode it: the content here is
            # AAC, the demuxer goes by the content and the container by the extension.
            raw = root / "sound.ac4"
            raw.write_bytes(radio.read_bytes())
            player = Player(args.player, str(raw))
            try:
                assert player.wait("PLAYBACK_OPEN")["sts"] == 0
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
            finally:
                player.stop()
            print("PASS .ac4 file: opened like the other audio extensions")

            # The other file extensions of FFmpeg demuxers for raw Dolby, DTS and
            # TrueHD/MLP, Matroska audio, MP4 audio books, AIFF and Monkey's Audio:
            # the same AAC content, only the extension selects the container.
            for extension in ("ac3", "eac3", "ec3", "dts", "dtshd", "thd", "mlp",
                              "mka", "m4b", "aif", "aiff", "ape"):
                raw = root / f"sound.{extension}"
                raw.write_bytes(radio.read_bytes())
                player = Player(args.player, str(raw))
                try:
                    assert player.wait("PLAYBACK_OPEN")["sts"] == 0, extension
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0, extension
                finally:
                    player.stop()
                raw.unlink()
            print("PASS raw audio, .mka, .m4b, AIFF and APE files: opened by their extension")

            missing = subprocess.run([args.player, "iptv://" + str(root / "missing")],
                                     capture_output=True, timeout=5)
            assert missing.returncode == 1 and b"cannot be read" in missing.stdout, missing
            print("PASS unreadable iptv:// file: parameter error instead of a crash")

            # The open runs in a thread of its own; its end wakes the wait
            # for commands at once instead of after its 1 s timeout.
            for target, status in ((media, 0), (root / "missing.ts", -1)):
                started = time.monotonic()
                opener = subprocess.Popen([args.player, str(target)], stdin=subprocess.PIPE,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                          text=True)
                try:
                    opened = None
                    for line in opener.stderr:
                        if '"PLAYBACK_OPEN"' in line:
                            opened = time.monotonic() - started
                            assert json.loads(line)["PLAYBACK_OPEN"]["sts"] == status, line
                            break
                    assert opened is not None and opened < 0.5, (target, opened)
                    try:
                        opener.stdin.write("q\n")
                        opener.stdin.flush()
                    except BrokenPipeError:
                        pass  # a failed open ends the player by itself
                    assert opener.wait(timeout=5) == (0 if status == 0 else 10)
                finally:
                    if opener.poll() is None:
                        opener.kill()
                        opener.wait()
                    try:
                        opener.stdin.close()
                    except BrokenPipeError:
                        pass
                    opener.stderr.close()
            print("PASS open: PLAYBACK_OPEN right after the open, not after the 1 s command wait")

            for target, language in named:
                player = Player(args.player, str(target))
                try:
                    assert player.wait("PLAYBACK_OPEN")["file"] == "file://" + str(target)
                    tracks = player.wait("a_l")
                    assert [track["n"] for track in tracks] == [language[:511]], tracks
                    assert player.wait("a_c")["n"] == language[:511]
                finally:
                    player.stop()
            print("PASS track names: valid JSON with quotes and control bytes, long names cut")

            # HLS options for http input: segment names, segment retry, gzip playlists.
            run(args.ffmpeg, "-y", "-i", str(media), "-c", "copy", "-t", "3", "-f", "hls",
                "-hls_time", "1", "-hls_list_size", "0",
                "-hls_segment_filename", str(root / "part%d.ts"), str(root / "part.m3u8"))
            lines = (root / "part.m3u8").read_text().splitlines()
            parts = [line for line in lines if line.startswith("part")]
            playlists = {"/fast/": "live?id=5", "/retry/": "index.m3u8", "/gz/": "index.m3u8"}
            for prefix, playlist in playlists.items():
                suffix = "" if prefix == "/fast/" else ".ts"
                names = {part: f"{number}{suffix}" for number, part in enumerate(parts)}
                for part, name in names.items():
                    resources[prefix + name] = (root / part).read_bytes()
                resources[prefix + playlist] = "".join(
                    names.get(line, line) + "\n" for line in lines).encode()

            # No .m3u8 in the url, the Content-Type tells; segments without an extension.
            content_types["/fast/live?id=5"] = "application/vnd.apple.mpegurl"
            requests.clear()
            player = Player(args.player, base + "/fast/live?id=5")
            try:
                assert player.wait("PLAYBACK_OPEN")["sts"] == 0, player.messages
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
            finally:
                player.stop()
            assert ("/fast/0", 0) in requests, requests
            print("PASS HLS: segments without an extension behind a playlist url without .m3u8")

            # The first segment fails once: asked for again before the next one.
            failures["/retry/0.ts"] = 1
            requests.clear()
            player = Player(args.player, base + "/retry/index.m3u8")
            try:
                assert player.wait("PLAYBACK_OPEN")["sts"] == 0
            finally:
                player.stop()
            segments = [path for path, _ in requests if path.endswith(".ts")]
            assert segments[:2] == ["/retry/0.ts", "/retry/0.ts"], segments
            print("PASS HLS: a segment that failed to open is requested again, not skipped")

            # gzip is asked for and inflated; an Accept-Encoding given with -h wins.
            compressed.add("/gz/index.m3u8")
            for options, expected in (([], "gzip"), (["-h", "Accept-Encoding: identity\r\n"], "identity")):
                encodings.clear()
                player = Player(args.player, *options, base + "/gz/index.m3u8")
                try:
                    assert player.wait("PLAYBACK_OPEN")["sts"] == 0
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                finally:
                    player.stop()
                asked = [value for path, value in encodings if path == "/gz/index.m3u8"]
                assert asked and all(value == expected for value in asked), (options, encodings)
            print("PASS HLS: playlist requested with Accept-Encoding: gzip, -h identity kept")

            # Segments disguised as pictures: a PNG or WebP image in front of
            # the TS data, named .png, .js or without an extension, also
            # inside AES-128. Played directly when the hls demuxer of the
            # linked FFmpeg skips the image (hlsseg bit 2).
            hlsseg = json.loads(subprocess.run(
                [args.player], capture_output=True, timeout=5).stderr.splitlines()[0]
            )["EPLAYER3_EXTENDED"]["hlsseg"]
            assert hlsseg & 1, hlsseg
            if hlsseg & 2:
                def chunk(kind, data):
                    return (struct.pack(">I", len(data)) + kind + data
                            + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
                # image data with 0x47 bytes 188 apart, one TS packet short of a run
                filler = bytearray(0x46 if (i * 7 + 3) % 256 == 0x47 else (i * 7 + 3) % 256
                                   for i in range(1500))
                for k in range(4):
                    filler[100 + k * 188] = 0x47
                png = (b"\x89PNG\r\n\x1a\n"
                       + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
                       + chunk(b"tEXt", b"Comment\0" + bytes(filler))
                       + chunk(b"IDAT", zlib.compress(b"".join(b"\0" + b"\x80" * 48
                                                             for _ in range(16))))
                       + chunk(b"IEND", b""))
                webp = (b"RIFF" + struct.pack("<I", 4 + 8 + len(filler)) + b"WEBP"
                        + b"VP8L" + struct.pack("<I", len(filler)) + bytes(filler))
                key, iv = bytes(range(16)), bytes(range(16, 32))
                resources["/img/aes.key"] = key
                openssl = shutil.which("openssl")
                variants = [("png", png, "{}.png", False), ("webp", webp, "s/{}?t=ab12", False)]
                if openssl:
                    variants.append(("aes", png, "{}.js", True))
                else:
                    print("SKIP HLS image head inside AES-128: no openssl tool")
                for name, head, pattern, encrypted in variants:
                    body = ""
                    for line in lines:
                        if encrypted and line.startswith("#EXTINF") and "#EXT-X-KEY" not in body:
                            body += f'#EXT-X-KEY:METHOD=AES-128,URI="aes.key",IV=0x{iv.hex()}\n'
                        if line in parts:
                            uri = pattern.format(f"{name}{parts.index(line)}")
                            data = head + (root / line).read_bytes()
                            if encrypted:
                                data = subprocess.run(
                                    [openssl, "enc", "-aes-128-cbc", "-K", key.hex(),
                                     "-iv", iv.hex()], input=data, capture_output=True,
                                    check=True, timeout=10).stdout
                            resources["/img/" + uri] = data
                            line = uri
                        body += line + "\n"
                    resources[f"/img/{name}.m3u8"] = body.encode()
                    requests.clear()
                    player = Player(args.player, base + f"/img/{name}.m3u8")
                    try:
                        assert player.wait("PLAYBACK_OPEN")["sts"] == 0, player.messages
                        assert player.wait("PLAYBACK_PLAY")["sts"] == 0, player.messages
                        assert player.wait("v_c")["w"] == 64, player.messages
                    finally:
                        player.stop()
                    first = "/img/" + pattern.format(f"{name}0")
                    assert (first, 0) in requests, requests
                    print(f"PASS HLS: a {name} image in front of the TS data of segments "
                          f"named {pattern} skipped, the 64x64 MPEG-2 video played")
            else:
                print("SKIP HLS image heads: the hls demuxer of this FFmpeg has no skip_image_head")

            # A read that waits for the network (here a segment whose answer
            # is held back) holds the container lock. The track list, the
            # current track and the position are answered meanwhile; the
            # audio switch before it makes the live values of the audio
            # list change, so "al" cannot come from the cached answer as is.
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-map", "0:a", "-map", "0:a",
                "-c", "copy", "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
                "-hls_segment_filename", str(root / "two%d.ts"), str(root / "two.m3u8"))
            lines = (root / "two.m3u8").read_text().splitlines()
            for line in lines:
                if line.startswith("two"):
                    resources["/two/" + line] = (root / line).read_bytes()
            resources["/two/index.m3u8"] = "".join(line + "\n" for line in lines).encode()
            asked, answer = threading.Event(), threading.Event()
            held["/two/two4.ts"] = (asked, answer)
            player = Player(args.player, base + "/two/index.m3u8")
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                tracks = player.wait("a_l")
                assert len(tracks) == 2, tracks
                player.command("ai1")
                assert player.wait("a_s")["sts"] == 0
                assert asked.wait(15), requests
                time.sleep(1.5)  # what was read before the segment is played
                started = time.monotonic()
                player.command("al\nac\nj")
                assert len(player.wait("a_l")) == 2
                assert player.wait("a_c")["id"] == tracks[1]["id"]
                player.wait("J")
                waited = time.monotonic() - started
                assert waited < 2, waited
            finally:
                answer.set()
                player.stop()
                del held["/two/two4.ts"]
            print("PASS commands during a read that waits for the network: al, ac and j answered")

            # The same with -C, where every list updates the tracks under the
            # container lock: al and j are answered at once, q ends the
            # player at once; then q ends a track switch that waits for the
            # lock.
            for switch in (False, True):
                asked, answer = threading.Event(), threading.Event()
                held["/two/two4.ts"] = (asked, answer)
                player = Player(args.player, "-C", base + "/two/index.m3u8")
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    tracks = player.wait("a_l")
                    assert len(tracks) == 2, tracks
                    assert asked.wait(15), requests
                    time.sleep(1.5)
                    if switch:
                        player.command(f"a{tracks[1]['id']}")
                        time.sleep(0.3)
                    else:
                        for command, name in (("al", "a_l"), ("j", "J")):
                            started = time.monotonic()
                            player.command(command)
                            player.wait(name)
                            waited = time.monotonic() - started
                            assert waited < 0.5, (command, waited)
                    started = time.monotonic()
                    player.command("q")
                    player.process.wait(timeout=8)
                    waited = time.monotonic() - started
                    assert waited < 1.5, waited
                finally:
                    answer.set()
                    player.stop()
                    del held["/two/two4.ts"]
            print("PASS -C during a read that waits for the network: al and j answered, q ends at once,"
                  " also after a track switch that waits")

            player = Player(args.player, str(embedded[1]))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                player.command("kc 900")
                position = player.wait("J")["ms"] // 1000
                length = player.wait("PLAYBACK_LENGTH")["length"]
                assert length >= 10 and position < length - 5, (position, length)
                jump = player.wait("PLAYBACK_SEEK")
                assert jump == {"sec": length - position - 5, "sts": 0}, (jump, position, length)
            finally:
                player.stop()
            print("PASS relative seek past the end: jump to five seconds before the end")

            # HEVC HDR type in v_c: PQ in the VUI, and BT.2020 (14) in the VUI
            # with HLG (18) in an alternative transfer SEI of the first frame.
            for name, transfer, alternative, expected in (("pq", 16, None, 1), ("hlg", 14, 18, 2)):
                raw = root / f"{name}.hevc"
                raw.write_bytes(hevc_stream(transfer, alternative))
                target = root / f"{name}.ts"
                run(args.ffmpeg, "-y", "-f", "hevc", "-framerate", "25", "-i", str(raw),
                    "-c", "copy", str(target))
                player = Player(args.player, str(target))
                try:
                    seen = []
                    while not seen or seen[-1].get("hdr") != expected:
                        seen.append(player.wait("v_c"))
                    types = [current.get("hdr") for current in seen]
                    assert set(types) <= {0, 3, expected}, types
                    assert seen[-1]["e"] == "V_HEVC" and seen[-1]["w"] == 64, seen
                finally:
                    player.stop()
            print("PASS HEVC HDR: HDR10 from the SPS, HLG from an SEI after a BT.2020 SPS")

            # The avcC of replay clips: SPS and PPS twice with a zero at the
            # end of a PPS, a zero between two PPS (tests/run.sh checks what
            # the writer makes of them). The player opens and plays both.
            for target in h264_avcc_mp4.write(args.ffmpeg, root):
                player = Player(args.player, str(target))
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    video = player.wait("v_c")
                    assert video["e"] == "V_MPEG4/ISO/AVC" and video["w"] == 64, video
                finally:
                    player.stop()
            print("PASS H.264 avcC with repeated sets and stray zeros: played")

            # "Constrained High": constraint_set4/5 of a High profile SPS
            # (0x0C), in front of every IDR picture in TS (Annex B) and in
            # the samples and the avcC of MP4. The writer clears them
            # (tests/run.sh checks the bytes), EP3_H264_CONSTRAINED_HIGH=0
            # keeps them; the debug log of the writer tells which.
            raw = root / "constrained-high.h264"
            raw.write_bytes(constrained_high_h264(50))
            for name in ("constrained-high.ts", "constrained-high.mp4"):
                run(args.ffmpeg, "-y", "-f", "h264", "-framerate", "25", "-i", str(raw),
                    "-c", "copy", *frame_times, str(root / name))
                for switch, cleared in ((None, True), ("0", False)):
                    env = dict(os.environ)
                    env.pop("EP3_H264_CONSTRAINED_HIGH", None)
                    if switch is not None:
                        env["EP3_H264_CONSTRAINED_HIGH"] = switch
                    player = Player(args.player, "-L", "H264_DEBUG_LEVEL:10", str(root / name), env=env)
                    try:
                        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                        video = player.wait("v_c")
                        assert video["e"] == "V_MPEG4/ISO/AVC" and video["w"] == 64, video
                        time.sleep(1)
                        player.command("q")
                        player.process.wait(timeout=5)
                        size = os.fstat(player.log.fileno()).st_size
                        log = os.pread(player.log.fileno(), size, 0).decode(errors="replace")
                    finally:
                        player.stop()
                    assert ("H264 profile 100: constraint_set4/5 cleared (0x0c -> 0x00)" in log) == cleared, (
                        name, switch, log[-3000:])
            print("PASS H.264 Constrained High: constraint_set4/5 cleared in TS and MP4, kept with the switch off")

            player = Player(args.player, "-U", "0", str(codecs))
            try:
                tracks = player.wait("a_l")
                assert [track["e"] for track in tracks] == ["A_OPUS", "A_IPCM"], tracks
            finally:
                player.stop()
            print("PASS -U 0: Opus passed through, Vorbis still software decoded")

            # Audio switch of a file while paused: no seek, the track changes.
            dual = root / "dual-audio.mkv"
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-map", "0:a", "-map", "0:a",
                "-c", "copy", str(dual))
            player = Player(args.player, str(dual))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                tracks = player.wait("a_l")
                assert len(tracks) == 2, tracks
                assert player.wait("a_c")["id"] == tracks[0]["id"]
                player.command("p")
                assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
                player.command("ai1")
                status = player.wait("a_s")
                assert status == {"id": tracks[1]["id"], "sts": 0}, status
                player.command("ac")
                assert player.wait("a_c")["id"] == tracks[1]["id"]
                player.command("c")
                assert player.wait("PLAYBACK_CONTINUE")["sts"] == 0
            finally:
                player.stop()
            print("PASS audio switch while paused: the track changes and the switch succeeds")

            # --start: the seek comes before the first packet. The fake output
            # logs the PTS of every packet it gets (LINUXDVB debug level 20).
            run(args.ffmpeg, "-y", "-i", str(media), "-c", "copy", "-f", "hls", "-hls_time", "1",
                "-hls_list_size", "0", "-hls_playlist_type", "vod",
                "-hls_segment_filename", str(root / "vod%d.ts"), str(root / "vod.m3u8"))
            for segment in [root / "vod.m3u8", *root.glob("vod*.ts")]:
                resources["/" + segment.name] = segment.read_bytes()

            def start_case(target, *options):
                """Capabilities, first position (ms) and lowest PTS written (90 kHz).

                The fake output paces video from PTS 0 on, so the first packet
                of a later start comes late: the position is polled until set."""
                player = Player(args.player, "-L", "LINUXDVB_DEBUG_LEVEL:20", *options, target)
                try:
                    caps = player.wait("EPLAYER3_EXTENDED")
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    position, deadline = 0, time.monotonic() + 15
                    while not position and time.monotonic() < deadline:
                        time.sleep(0.25)
                        player.command("j")
                        position = player.wait("J")["ms"]
                    assert position, target
                    player.command("q")
                    player.process.wait(timeout=5)
                    size = os.fstat(player.log.fileno()).st_size
                    log = os.pread(player.log.fileno(), size, 0).decode(errors="replace")
                finally:
                    player.stop()
                written = [int(line.split("Pts=")[1].split()[0]) for line in log.splitlines()
                           if "DataLength=" in line and "Pts=" in line]
                written = [pts for pts in written if pts < 0x200000000]
                assert written, log[-3000:]
                return caps, position, min(written)

            for target in (str(media), base + "/media.ts", base + "/vod.m3u8"):
                _, origin, first = start_case(target)
                caps, position, lowest = start_case(target, "--start", "5")
                assert caps["start"] == 1, caps
                assert 4000 <= position - origin < 7000, (target, origin, position)
                assert lowest >= first + 4 * 90000, (target, first, lowest)
                # at or behind the end (12 s): from the start
                _, position, lowest = start_case(target, "--start", "100")
                assert position - origin < 1000 and lowest < first + 90000, (
                    target, origin, position, first, lowest)
            for value in ("abc", "-1", "nan", "5s", ""):
                invalid = subprocess.run([args.player, "--start", value, str(media)],
                                         capture_output=True, timeout=5)
                assert invalid.returncode != 0, value
                assert b"Invalid --start value" in invalid.stdout, (value, invalid.stdout[-500:])
            print("PASS --start: file, HTTP and HLS VOD start at 5 s without a packet before it, "
                  "ignored behind the end, invalid values rejected")

            # Follow-up fixes of main and of the container. -x: see "failed open" below.
            for option in ("-F",):
                unreadable = subprocess.run(
                    [args.player, option, "iptv://" + str(root / "missing"), str(media)],
                    input=b"q\n", capture_output=True, timeout=10)
                assert unreadable.returncode == 1 and b"cannot be read" in unreadable.stdout, (
                    option, unreadable)
            print("PASS unreadable iptv:// file for -F: parameter error, not played without it")

            # A pipe has no size: ftell() fails on it.
            sizeless = subprocess.run([args.player, "iptv:///dev/stdin"], input=b"/missing\n",
                                      capture_output=True, timeout=10)
            assert sizeless.returncode == 1 and b"cannot be read" in sizeless.stdout, sizeless
            print("PASS iptv:// file without a size: parameter error, nothing read into one byte")

            import signal
            signal_media = root / "signal.ts"
            run(args.ffmpeg, "-y", "-stream_loop", "-1", "-i", str(media), "-c", "copy",
                "-t", "60", str(signal_media))

            # SIGTERM, SIGINT and SIGHUP stop the player the way "q" does, also
            # while it is paused; the command pipe stays open meanwhile.
            for number, paused in ((signal.SIGTERM, False), (signal.SIGINT, False),
                                   (signal.SIGHUP, False), (signal.SIGTERM, True)):
                player = Player(args.player, str(signal_media))
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    if paused:
                        player.command("p")
                        assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
                    started = time.monotonic()
                    player.process.send_signal(number)
                    assert player.wait("PLAYBACK_STOP")["sts"] == 0, number
                    player.process.wait(timeout=5)
                    assert time.monotonic() - started < 5, number
                finally:
                    if player.process.poll() is None:
                        player.process.kill()
                        player.process.wait()
                    player.stop()
            print("PASS stop signals: PLAYBACK_STOP and exit code 0 on SIGTERM, SIGINT, SIGHUP, also paused")

            # A signal while the input is still being opened: a FIFO without a
            # writer blocks the open. The player ends as on "q" at that point.
            blocked = root / "blocked.ts"
            os.mkfifo(blocked)
            player = Player(args.player, str(blocked))
            try:
                time.sleep(1)
                assert player.process.poll() is None
                player.process.send_signal(signal.SIGTERM)
                player.process.wait(timeout=5)
                assert player.process.returncode == 11, player.process.returncode
            finally:
                if player.process.poll() is None:
                    player.process.kill()
                    player.process.wait()
                player.reader.join(timeout=2)
                player.process.stdin.close()
                player.process.stderr.close()
                player.log.close()
                blocked.unlink()
            assert not any("PLAYBACK_OPEN" in message for message in player.messages), player.messages
            print("PASS stop signal during the open: the player ends as on q")

            # The player started by a thread of a helper process: the end of
            # that thread is no reason to stop, the end of the process is.
            helper = (
                "import os, subprocess, sys, threading\n"
                "player, media, stdin, out, err = sys.argv[1:]\n"
                "def spawn():\n"
                "    with open(out, 'wb') as o, open(err, 'wb') as e:\n"
                "        started = subprocess.Popen([player, media], stdin=int(stdin),\n"
                "                                   stdout=o, stderr=e)\n"
                "    print(started.pid, flush=True)\n"
                "    sys.stdin.readline()\n"
                "thread = threading.Thread(target=spawn)\n"
                "thread.start()\n"
                "thread.join()\n"
                "print('thread ended', flush=True)\n"
                "sys.stdin.readline()\n"
                "os._exit(0)\n")
            out, err = root / "orphan.log", root / "orphan.err"
            commands_read, commands = os.pipe()
            parent = subprocess.Popen(
                [sys.executable, "-c", helper, args.player, str(signal_media), str(commands_read),
                 str(out), str(err)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                pass_fds=(commands_read,))
            os.close(commands_read)
            orphan = int(parent.stdout.readline())

            def orphan_events():
                events = []
                for line in err.read_text(errors="replace").splitlines():
                    if not line.startswith("{"):
                        continue
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass  # a line the player is still writing
                return events

            def orphan_running():
                try:
                    stat = Path(f"/proc/{orphan}/stat").read_text()
                except (FileNotFoundError, ProcessLookupError):
                    return False
                return stat.rsplit(")", 1)[1].split()[0] != "Z"

            def orphan_wait(name, seconds=10):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if any(name in event for event in orphan_events()):
                        return
                    time.sleep(0.1)
                raise AssertionError(f"Missing event {name}: {orphan_events()}\n"
                                     + out.read_text(errors="replace")[-3000:])

            try:
                orphan_wait("PLAYBACK_PLAY")
                # the thread ends while the player plays, after its prctl()
                parent.stdin.write("thread\n")
                parent.stdin.flush()
                assert parent.stdout.readline().strip() == "thread ended"
                time.sleep(2)
                assert orphan_running(), "killed when the starting thread ended"
                os.write(commands, b"j\n")
                orphan_wait("J")
                assert not any("PLAYBACK_STOP" in event for event in orphan_events())
                parent.stdin.write("exit\n")
                parent.stdin.flush()
                assert parent.wait(timeout=5) == 0
                orphan_wait("PLAYBACK_STOP", 5)
                deadline = time.monotonic() + 5
                while orphan_running() and time.monotonic() < deadline:
                    time.sleep(0.1)
                assert not orphan_running()
                assert "Parent process is gone" in out.read_text(errors="replace")
            finally:
                if orphan_running():
                    os.kill(orphan, signal.SIGKILL)
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                parent.stdin.close()
                parent.stdout.close()
                os.close(commands)
            print("PASS parent: the end of the starting thread is ignored, the end of the process stops")

            # Variant selection: the top variant has Dirac video, for which there
            # is no writer; the lower one has MPEG-2 video.
            dirac = root / "dirac.ts"
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-map", "0:a",
                "-vf", "scale=128:128", "-c:v", "vc2", "-strict", "-1", "-c:a", "copy",
                str(dirac))
            resources["/dirac.ts"] = dirac.read_bytes()
            resources["/dirac.m3u8"] = (b"#EXTM3U\n#EXT-X-TARGETDURATION:12\n#EXTINF:12,\n"
                                        b"dirac.ts\n#EXT-X-ENDLIST\n")
            resources["/mixed.m3u8"] = (b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\nlow.m3u8\n"
                                        b"#EXT-X-STREAM-INF:BANDWIDTH=900000\ndirac.m3u8\n")
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-map", "0:v", "-map", "0:a",
                "-filter:v:1", "scale=128:128", "-c:v:0", "mpeg2video", "-b:v:0", "100k",
                "-c:v:1", "vc2", "-b:v:1", "2M", "-strict", "-1", "-c:a", "aac", "-t", "6",
                "-single_file", "1", "-seg_duration", "1", "-f", "dash", str(root / "mixed.mpd"))
            mpd = ET.parse(root / "mixed.mpd").getroot()
            for representation in mpd.findall(".//d:Representation", ns):
                segments = representation.find("d:SegmentList", ns)
                segment_base = ET.Element(f"{{{ns['d']}}}SegmentBase")
                segment_base.append(segments.find("d:Initialization", ns))
                representation.remove(segments)
                representation.append(segment_base)
            resources["/mixed.mpd"] = ET.tostring(mpd)
            for single_file in root.glob("mixed-stream*.mp4"):
                resources["/" + single_file.name] = single_file.read_bytes()
            for options, uri in (([], "/mixed.m3u8"), (["-M", "1920x1080"], "/mixed.m3u8"),
                                 ([], "/mixed.mpd")):
                player = Player(args.player, *options, base + uri)
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    player.wait("a_l")  # a video track is reported before the audio tracks
                    video = [message["v_c"] for message in player.messages if "v_c" in message]
                    assert video and video[0]["e"] == "V_MPEG2" and video[0]["w"] == 64, (
                        "no MPEG-2 video track", options, uri, video)
                finally:
                    player.stop()
            print("PASS HLS/DASH: a variant whose video has no writer is passed over")

            latin = root / "latin1.mkv"
            language = b"Fran\xe7ais \xc3\xa9t\xc3\xa9"  # 0xE7 is Latin-1, the rest UTF-8
            run(args.ffmpeg, "-y", "-i", str(media), "-c", "copy",
                "-metadata:s:a:0", b"language=" + language, str(latin))
            player = Player(args.player, str(latin))
            try:
                tracks = player.wait("a_l")
                expected = language.replace(b"\xe7", b"\xc3\xa7").decode()
                assert [track["n"] for track in tracks] == [expected], tracks
            finally:
                player.stop()
            print("PASS track names: a Latin-1 byte escaped, the line stays UTF-8")

            # Track messages: channel count "c", codec name "d" and title "t".
            # Every case runs on its own; the failed ones are listed at the end.
            failed = []

            def track_case(description, check):
                try:
                    check()
                except Exception as error:
                    failed.append(description)
                    print(f"FAIL {description}: {error!r}"[:4000])
                else:
                    print("PASS " + description)

            # Encoder, channels, "d"; titles with quotes, a backslash, UTF-8,
            # Latin-1 (with a Latin-1 language), cut ones, a control byte, none.
            latin = b"Fran\xe7ais \xc3\xa9t\xc3\xa9"
            audio_tracks = (
                ("ac3", 6, "Dolby Digital", 'Kommentar "Regie"', 'Kommentar "Regie"'),
                ("eac3", 2, "Dolby Digital +", "Back\\slash \\\\", "Back\\slash \\\\"),
                ("aac", 6, "AAC-LC", "Größe Übersicht", "Größe Übersicht"),
                ("mp2", 1, "MP2", latin, "Français été"),
                ("opus", 2, "Opus", "x" * 1500, "x" * 255),
                ("vorbis", 2, "Vorbis", "ä" * 200, "ä" * 127),
                ("flac", 1, "FLAC", "Ctrl\x01", "Ctrl\x01"),
                ("pcm_s16le", 6, "PCM", None, ""))
            described = root / "described.mkv"
            command = [args.ffmpeg, "-y", "-i", str(media), "-f", "s16le", "-ar", "48000",
                       "-ac", "2", "-i", str(audio), "-map", "0:v", "-c:v", "copy"]
            for index, (codec, channels, _, title, _) in enumerate(audio_tracks):
                command += ["-map", "1:a", f"-c:a:{index}", codec, f"-ac:a:{index}", str(channels)]
                if title is not None:
                    tag = title if isinstance(title, bytes) else title.encode()
                    command += [f"-metadata:s:a:{index}", b"title=" + tag]
            command += ["-metadata:s:a:3", b"language=d" + b"\xe9u", "-strict", "-2", "-t", "12",
                        str(described)]
            run(*command)

            def described_list():
                player = Player(args.player, str(described))
                try:
                    tracks = player.wait("a_l")
                    current = player.wait("a_c")
                finally:
                    player.stop()
                assert len(tracks) == len(audio_tracks), tracks
                return tracks, current

            def codec_names():
                tracks, current = described_list()
                assert [(track["c"], track["d"]) for track in tracks] == [
                    (channels, name) for _, channels, name, _, _ in audio_tracks], tracks
                assert current == tracks[0], (current, tracks)

            track_case("a_l/a_c: channel count and codec name of AC-3, E-AC-3, AAC, MP2, "
                       "Opus, Vorbis, FLAC and PCM in mono, stereo and 5.1", codec_names)

            def titles():
                tracks, current = described_list()
                assert [track["t"] for track in tracks] == [
                    expected for *_, expected in audio_tracks], tracks
                assert tracks[3]["n"] == "déu", tracks[3]
                assert current["t"] == tracks[0]["t"], current

            track_case("a_l/a_c: titles with quotes, backslashes, UTF-8, Latin-1, a control "
                       "byte and none as JSON, long ones cut between characters", titles)

            def switched():
                player = Player(args.player, str(described))
                try:
                    tracks = player.wait("a_l")
                    player.wait("a_c")
                    for index in (1, 4, 6, 7):
                        player.command(f"ai{index}")
                        assert player.wait("a_s")["id"] == tracks[index]["id"]
                        player.command("ac")
                        current = player.wait("a_c")
                        assert current == tracks[index], (index, current, tracks[index])
                        assert "c" in current and "t" in current, current
                finally:
                    player.stop()

            track_case("a_c after a switch: the keys of the selected track", switched)

            titled = root / "titled-subtitles.mkv"
            subtitle_titles = ('Signs "only"', "SDH \\ Größe")
            run(args.ffmpeg, "-y", "-i", str(media), "-f", "srt", "-i", str(subtitles),
                "-f", "srt", "-i", str(subtitles), "-map", "0:v", "-map", "0:a",
                "-map", "1", "-map", "2", "-c", "copy", "-c:s:0", "copy", "-c:s:1", "ass",
                "-metadata:s:s:0", "title=" + subtitle_titles[0],
                "-metadata:s:s:1", "title=" + subtitle_titles[1], str(titled))

            def subtitle_titles_case():
                player = Player(args.player, str(titled))
                try:
                    tracks = player.wait("s_l")
                    assert [track["t"] for track in tracks] == list(subtitle_titles), tracks
                    assert [track["e"] for track in tracks] == ["S_TEXT/SUBRIP", "S_TEXT/ASS"], tracks
                    player.command("si1\nsc")
                    assert player.wait("s_s")["sts"] == 0
                    current = player.wait("s_c")
                    assert current == tracks[1], (current, tracks)
                finally:
                    player.stop()

            track_case("s_l/s_c: titles of SubRip and ASS tracks", subtitle_titles_case)

            picture = root / "picture.ts"
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-c", "copy", str(picture))
            rendition_audio = root / "rendition.ts"
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:a", "-c", "copy",
                str(rendition_audio))
            resources["/picture.ts"] = picture.read_bytes()
            resources["/rendition.ts"] = rendition_audio.read_bytes()
            for playlist, segment in (("picture", "picture"), ("main", "rendition"),
                                      ("described", "rendition")):
                resources[f"/{playlist}.m3u8"] = (
                    f"#EXTM3U\n#EXT-X-TARGETDURATION:12\n#EXTINF:12,\n"
                    f"{segment}.ts\n#EXT-X-ENDLIST\n").encode()
            renditions = {"en": "Main mix", "de": "Hörfilm für Blinde"}
            resources["/renditions.m3u8"] = (
                '#EXTM3U\n'
                f'#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="{renditions["en"]}",'
                'LANGUAGE="en",DEFAULT=YES,AUTOSELECT=YES,URI="main.m3u8"\n'
                f'#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="{renditions["de"]}",'
                'LANGUAGE="de",URI="described.m3u8"\n'
                '#EXT-X-STREAM-INF:BANDWIDTH=900000,AUDIO="aud"\npicture.m3u8\n').encode()

            def rendition_names():
                player = Player(args.player, base + "/renditions.m3u8")
                try:
                    assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    tracks = player.wait("a_l")
                    current = player.wait("a_c")
                finally:
                    player.stop()
                assert {track["n"]: track["t"] for track in tracks} == renditions, tracks
                assert all((track["c"], track["d"]) == (2, "AAC-LC") for track in tracks), tracks
                assert current == next(track for track in tracks if track["id"] == current["id"])

            track_case("HLS: NAME of the EXT-X-MEDIA audio renditions as title", rendition_names)

            silent = root / "silent.mkv"
            run(args.ffmpeg, "-y", "-i", str(media), "-map", "0:v", "-c", "copy", str(silent))

            def no_track():
                player = Player(args.player, str(silent))
                try:
                    assert player.wait("a_l") == []
                    for _ in range(2):  # sent at the start, then asked for
                        assert player.wait("a_c") == {
                            "id": -1, "e": "", "n": "", "c": 0, "d": "", "t": ""}
                        assert player.wait("s_c") == {"id": -1, "e": "", "n": "", "f": 0, "t": ""}
                        player.command("ac\nsc")
                finally:
                    player.stop()

            track_case("a_c/s_c without audio and subtitle tracks: every key present", no_track)
            assert not failed, failed

            sound = root / "sound.ts"
            run(args.ffmpeg, "-y", "-i", str(media), "-vn", "-c", "copy", str(sound))
            resources["/sound.ts"] = sound.read_bytes()
            player = Player(args.player, "-o", "1", "-x", base + "/sound.ts", str(media))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                deadline = time.monotonic() + 20
                for back in (False, True):
                    if back:
                        player.command("gf 0")
                        assert player.wait("PLAYBACK_SEEK_ABS")["sts"] == 0
                    while True:  # the position is that of the video in the local file
                        player.command("j")
                        if (player.wait("J")["ms"] < 3000) == back:
                            break
                        assert time.monotonic() < deadline, ("local file not read", back)
                        time.sleep(0.05)
            finally:
                player.stop()
            print("PASS progressive local file with an http audio file: played and read after a seek")

            # Long transcoding options. Without a file the player reports what
            # the options select and ends with the usage text.
            def selected(*options):
                result = subprocess.run([args.player, *options], capture_output=True, timeout=5)
                assert result.returncode == 1, (options, result.returncode)
                lines = result.stdout.decode(errors="replace").splitlines()
                assert not any("getopt returned" in line for line in lines), (options, lines)
                return [line for line in lines
                        if "will be transcoded" in line or "transcoding (" in line
                        or line.startswith("No ")]

            caps = json.loads(subprocess.run(
                [args.player], capture_output=True, timeout=5).stderr.splitlines()[0])
            if caps["EPLAYER3_EXTENDED"]["xcode"] == 7:
                dts_ac3, dts_eac3 = (f"DTS will be transcoded to {codec} 5.1"
                                     for codec in ("AC3", "E-AC-3"))
                truehd_ac3, truehd_eac3 = (f"TrueHD will be transcoded to {codec} 5.1"
                                           for codec in ("AC3", "E-AC-3"))
                both_ac3, both_eac3 = (f"DTS and TrueHD will be transcoded to {codec} 5.1"
                                       for codec in ("AC3", "E-AC-3"))
                aac = "HE-AAC will be transcoded to AAC-LC"
                for options, expected in (
                        (["-Z"], [dts_ac3]), (["--dts-ac3"], [dts_ac3]),
                        (["-X"], [truehd_ac3]), (["--truehd-ac3"], [truehd_ac3]),
                        (["-Y"], [aac]), (["--heaac-aaclc"], [aac]),
                        (["-Z", "-X"], [both_ac3]),
                        (["--dts-ac3", "--truehd-ac3"], [both_ac3]),
                        (["-Z", "-X", "-E"], [both_eac3]),
                        (["--dts-ac3", "--truehd-ac3", "-E"], [both_eac3]),
                        (["--dts-eac3"], [dts_eac3]), (["--truehd-eac3"], [truehd_eac3]),
                        (["--dts-eac3", "--truehd-eac3"], [both_eac3]),
                        (["--dts-eac3", "-X"], [dts_eac3, truehd_ac3]),
                        (["-Z", "--truehd-eac3"], [dts_ac3, truehd_eac3]),
                        (["-E", "--dts-eac3"], [dts_eac3]),
                        (["--dts-ac3", "--dts-eac3"], [dts_eac3]),
                        (["--dts-eac3", "--dts-ac3", "--truehd-ac3"], [dts_eac3, truehd_ac3]),
                        (["--truehd-eac3", "-X", "-Z", "-Y"], [dts_ac3, truehd_eac3, aac]),
                        (["--dts-e"], [dts_eac3]),
                        (["-E"], [])):
                    assert selected(*options) == expected, (options, selected(*options))
                off = selected("-K", "--dts-eac3", "--truehd-ac3", "--heaac-aaclc")
                assert len(off) == 1 and "is off" in off[0], off
                print("PASS long transcoding options: aliases, E-AC-3 per source codec, precedence, -K")
            else:
                print("SKIP long transcoding options: no AC3, AAC and E-AC-3 encoder in this FFmpeg")

            for option in (["--dts-dts"], ["--dts"], ["--dts-eac3=1"], ["--Z"]):
                invalid = subprocess.run([args.player, *option, str(media)],
                                         capture_output=True, timeout=5)
                assert invalid.returncode != 0, option
                assert b"getopt returned" in invalid.stdout, option
            player = Player(args.player, "--dts-eac3", "--truehd-ac3", "--heaac-aaclc", str(media))
            try:
                assert player.wait("PLAYBACK_OPEN")["sts"] == 0
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
            finally:
                player.stop()
            print("PASS long options: unknown, ambiguous and with a value rejected; playback starts with them")

            # Default User-Agent: a current desktop Chrome, also with a
            # libavformat that would send "Lavf/<version>".
            browser = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36")
            for options, expected in (([], browser), (["-u", "Agent/1.0"], "Agent/1.0"),
                                      (["-h", "User-Agent: Header/2.0\r\n"], "Header/2.0")):
                for target in ("/media.ts", "/master.m3u8"):
                    agents.clear()
                    player = Player(args.player, *options, base + target)
                    try:
                        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                    finally:
                        player.stop()
                    assert agents and all(agent == expected for _, agent in agents), (
                        options, target, agents)
            agents.clear()
            player = Player(args.player, "-x", base + "/sound.ts", str(media))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
            finally:
                player.stop()
            assert agents and all(agent == browser for _, agent in agents), agents
            print("PASS HTTP User-Agent: browser string without -u (also HLS and -x), -u and -h win")

            # The front end goes away without "q": its end of the command pipe closes.
            long_media = root / "long.ts"
            run(args.ffmpeg, "-y", "-stream_loop", "-1", "-i", str(media), "-c", "copy",
                "-t", "60", str(long_media))
            player = Player(args.player, str(long_media))
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                player.process.stdin.close()
                assert player.wait("PLAYBACK_STOP")["sts"] == 0
                player.process.wait(timeout=10)
            finally:
                if player.process.poll() is None:
                    player.process.kill()
                    player.process.wait()
                player.stop()
            print("PASS closed command pipe: playback stops and the player exits")

            # An input that cannot be opened is reported with FF_ERROR and
            # PLAYBACK_OPEN sts -1, also a -x iptv:// file that cannot be read.
            for options, target in (([], root / "missing.ts"),
                                    (["-x", "iptv://" + str(root / "missing.iptv")], media)):
                # stdin stays open: its end would be a q during the open (exit code 11)
                opener = subprocess.Popen([args.player, *options, str(target)],
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                try:
                    opener.wait(timeout=10)
                finally:
                    if opener.poll() is None:
                        opener.kill()
                        opener.wait()
                    opener.stdin.close()
                    out, err = opener.stdout.read(), opener.stderr.read()
                    opener.stdout.close()
                    opener.stderr.close()
                lines = [json.loads(line) for line in err.splitlines()]
                errors = [line["FF_ERROR"] for line in lines if "FF_ERROR" in line]
                opens = [line["PLAYBACK_OPEN"] for line in lines if "PLAYBACK_OPEN" in line]
                assert opener.returncode == 10, (options, opener.returncode, out[-2000:])
                assert errors and errors[0]["code"] < 0 and errors[0]["msg"], (options, lines)
                assert len(opens) == 1 and opens[0]["sts"] < 0, (options, lines)
                assert opens[0]["file"] == "file://" + str(target), (options, opens)
                assert lines.index({"FF_ERROR": errors[0]}) < lines.index({"PLAYBACK_OPEN": opens[0]})
            print("PASS failed open: FF_ERROR and PLAYBACK_OPEN sts -1, also for a missing -x iptv:// file")

            # Playback that ends because no data comes any more (the stall
            # limit, at least 60 s) is not the end of the stream: FF_ERROR
            # comes before the player ends. The end of the input sends none.
            resources["/stall.ts"] = long_media.read_bytes()
            stalls["/stall.ts"] = len(resources["/stall.ts"]) // 4
            started = time.monotonic()
            player = Player(args.player, base + "/stall.ts")
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                player.process.wait(timeout=150)
            finally:
                player.stop()
            errors = [message["FF_ERROR"] for message in player.messages if "FF_ERROR" in message]
            assert errors and errors[-1]["code"] < 0, (time.monotonic() - started, player.messages[-8:])
            player = Player(args.player, base + "/media.ts")
            try:
                assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                player.process.wait(timeout=60)
            finally:
                player.stop()
            assert not any("FF_ERROR" in message for message in player.messages), player.messages
            print("PASS end of playback: FF_ERROR after a read stall, none at the end of the stream")

            # The video track also goes to the video socket of enigma2.
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                if os.path.exists(args.video_socket):
                    os.unlink(args.video_socket)
                listener.bind(args.video_socket)
                listener.settimeout(10)
                player = Player(args.player, str(media))
                try:
                    video = player.wait("v_c")
                    datagram = listener.recv(4096)
                finally:
                    player.stop()
            finally:
                listener.close()
                if os.path.exists(args.video_socket):
                    os.unlink(args.video_socket)
            assert datagram.endswith(b"\n") and json.loads(datagram) == {"v_c": video}, datagram
            assert video["w"] == 64 and video["h"] == 64, video
            print("PASS video socket: the v_c line as one datagram")

            # FFmpeg 8 and later find the stream of an FLV tag by AVStream.id;
            # with the audio first, the video must keep the ID the demuxer gave.
            spark = root / "spark.flv"
            run(args.ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p",
                "-video_size", "64x64", "-framerate", "25", "-i", str(root / "video.yuv"),
                "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(audio),
                "-map", "1:a", "-map", "0:v", "-c:a", "aac", "-c:v", "flv", "-t", "12", str(spark))
            for option in ([], ["-4", "0"]):
                player = Player(args.player, *option, str(spark))
                try:
                    assert player.wait("v_c")["e"] == ("V_FLV" if option else "V_MPEG4")
                    deadline = time.monotonic() + 15
                    position = 0
                    while position < 6000:  # past the packets read while the streams were probed
                        assert player.process.poll() is None, ("ended", option, position)
                        assert time.monotonic() < deadline, (option, position)
                        player.command("j")
                        position = player.wait("J")["ms"]
                        time.sleep(0.2)
                finally:
                    player.stop()
            print("PASS FLV Sorenson Spark after the audio: played on, with and without flv2mpeg4")

            https_cases(args, root, Handler, requests, media)
        finally:
            stall_release.set()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
