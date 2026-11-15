"""Native-player and loopback HTTP regression fixtures; no receiver required."""

import argparse
import gzip
import http.server
import json
import math
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--pcm-fifo", help="Isolated path compiled into the host player")
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

        requests = []
        # Accept-Encoding of every request; paths that fail n more times, that
        # carry a Content-Type, that are compressed when gzip is accepted.
        encodings, failures, content_types, compressed = [], {}, {}, set()
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
                data = resources[self.path]
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
                if failures.get(self.path):
                    failures[self.path] -= 1
                    self.send_error(503)
                    return
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

            player = Player(args.player, str(radio))
            try:
                metadata = player.wait("m_t")
                assert metadata == {"rt": artist + " - " + title, "t": title, "a": artist}
                player.process.wait(timeout=10)
            finally:
                player.stop()
            print("PASS radio: full-player metadata JSON with quotes, backslashes and control bytes")

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

            for option in (["-Q", "3"], ["-D", "2"], ["-M", "1920x0"],
                           ["-M", "garbage"], ["-Q", "1junk"]):
                invalid = subprocess.run([args.player, *option, str(media)],
                                         capture_output=True, timeout=5)
                assert invalid.returncode != 0, option
            print("PASS strict CLI validation")

            missing = subprocess.run([args.player, "iptv://" + str(root / "missing")],
                                     capture_output=True, timeout=5)
            assert missing.returncode == 1 and b"cannot be read" in missing.stdout, missing
            print("PASS unreadable iptv:// file: parameter error instead of a crash")

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

            # Follow-up fixes of main and of the container.
            for option in ("-x", "-F"):
                unreadable = subprocess.run(
                    [args.player, option, "iptv://" + str(root / "missing"), str(media)],
                    input=b"q\n", capture_output=True, timeout=10)
                assert unreadable.returncode == 1 and b"cannot be read" in unreadable.stdout, (
                    option, unreadable)
            print("PASS unreadable iptv:// file for -x and -F: parameter error, not played without it")

            # A pipe has no size: ftell() fails on it.
            sizeless = subprocess.run([args.player, "iptv:///dev/stdin"], input=b"/missing\n",
                                      capture_output=True, timeout=10)
            assert sizeless.returncode == 1 and b"cannot be read" in sizeless.stdout, sizeless
            print("PASS iptv:// file without a size: parameter error, nothing read into one byte")

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
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
