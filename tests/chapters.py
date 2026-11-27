"""Chapters (c_l, cl) and the precise absolute seeks (gf with a fraction, ga) of the host player."""

import argparse
import json
import math
import os
from pathlib import Path
import queue
import re
import struct
import subprocess
import tempfile
import threading
import time

# Titles: quotes and a backslash (escaped in the JSON line), UTF-8 (passed through).
QUOTED = 'Say "hi" \\ back'
UTF8 = "Kapitel 3 – Über ☃"


def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}):\n{result.stderr.decode()}")
    return result


class Player:
    """The player with its stderr lines as bytes, exactly as sent."""

    def __init__(self, executable, *args, **extra_env):
        self.lines = []
        self.events = queue.Queue()
        env = dict(os.environ)
        env.pop("EP3_FAKE_VIDEO_LEAD", None)
        env.pop("EP3_FAKE_AUDIO_CLOCK", None)
        env.update(extra_env)
        self.process = subprocess.Popen([executable, *args], stdin=subprocess.PIPE,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stderr:
            message = json.loads(line)
            self.lines.append((line, message))
            self.events.put(message)

    def wait(self, name, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                event = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if name in event:
                return event[name]
        raise AssertionError(f"missing {name}: {[line for line, _ in self.lines][-30:]}")

    def command(self, text):
        self.process.stdin.write(text.encode() + b"\n")
        self.process.stdin.flush()

    def raw(self, name):
        """the lines of the message name, as sent"""
        return [line for line, message in self.lines if name in message]

    def stop(self):
        try:
            if self.process.poll() is None:
                try:
                    self.command("q")
                except BrokenPipeError:
                    pass
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise
        finally:
            self.reader.join(timeout=2)
            self.process.stdin.close()
            self.process.stderr.close()
        assert self.process.returncode == 0, (self.process.returncode, [line for line, _ in self.lines][-30:])


def ffmetadata(path, chapters):
    """chapters: (start ms, end ms, title or None)"""
    def escape(text):
        return re.sub(r"([=;#\\\n])", r"\\\1", text)
    text = ";FFMETADATA1\n"
    for start, end, title in chapters:
        text += f"[CHAPTER]\nTIMEBASE=1/1000\nSTART={start}\nEND={end}\n"
        if title is not None:
            text += f"title={escape(title)}\n"
    path.write_text(text, encoding="utf-8")


def c_l_line(chapters):
    """the expected line: (start, end, title) in the order of the list"""
    entries = ",".join('{"id":%d,"s":%d,"e":%d,"t":%s}' % (i, s, e, json.dumps(t or "", ensure_ascii=False))
                       for i, (s, e, t) in enumerate(chapters))
    return ('{"c_l":[' + entries + ']}\n').encode()


def check_open(player_path, media, check):
    """c_l once after the track lists, the same line again on cl. Returns the line."""
    player = Player(player_path, str(media))
    try:
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        player.wait("c_l")
        names = [next(iter(message)) for _, message in player.lines]
        assert names[0] == "EPLAYER3_EXTENDED", names
        position = names.index("c_l")
        for name in ("a_l", "s_l", "s_c"):
            assert name in names[:position], (name, names)
        assert names.count("c_l") == 1, names
        line = player.raw("c_l")[0]
        check(line)
        player.command("cl")
        deadline = time.monotonic() + 10
        while len(player.raw("c_l")) < 2:
            assert time.monotonic() < deadline, "no answer to cl"
            time.sleep(0.02)
        assert player.raw("c_l")[1] == line, player.raw("c_l")
    finally:
        player.stop()
    return line


def keyframes(ffmpeg, media):
    """the times of the keyframes of the video, in ms"""
    result = run(ffmpeg, "-hide_banner", "-skip_frame", "nokey", "-i", str(media), "-map", "0:v",
                 "-vf", "showinfo", "-f", "null", "-")
    return [round(float(value) * 1000) for value in re.findall(r"pts_time:([0-9.]+)", result.stderr.decode())]


def position(player):
    player.command("j")
    return player.wait("J")["ms"]


def landing(player, command, ack):
    """Seeks with command; the position of the first video shown after it (ms).
    The fake decoder (EP3_FAKE_VIDEO_LEAD) keeps the old position until the
    first picture after the seek comes, then plays on from it in real time."""
    prev, prev_t = position(player), time.monotonic()
    player.command(command)
    answer = player.wait("PLAYBACK_SEEK_ABS")
    for key, value in ack.items():
        assert answer.get(key) == value, (command, answer)
    seen = []
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        ms, now = position(player), time.monotonic()
        # until the seek lands the old position plays on or stands still (a
        # slow ASan run stalls now and then): the landing is a jump between
        # two samples, backwards or further ahead than the time between them
        step = ms - prev
        if seen or step < -500 or step > (now - prev_t) * 1000 + 500:
            seen.append(ms)
            if len(seen) >= 5:
                break
        prev, prev_t = ms, now
        time.sleep(0.005)
    assert seen, (command, prev, "no position after the seek")
    return min(seen)


def check_seeks(player_path, media, frames):
    """gf with a fraction lands on the keyframe before the target in ms, ga on
    the first one at or after it (the one before when there is none)"""
    def before(ms):
        return max(k for k in frames if k <= ms)

    def after(ms):
        later = [k for k in frames if k >= ms]
        return min(later) if later else before(ms)

    # the cases must tell the seconds-only seek from the precise one
    assert before(4500) != before(4000) and after(4300) != before(4300), frames
    assert after(13000) == before(13000), frames
    cases = [  # command, its answer, the target keyframe
        ("gf9", {"sec": 9, "sts": 0}, before(9000)),  # whole seconds: as before
        ("gf4.5", {"sec": 4, "ms": 4500, "after": 0, "sts": 0}, before(4500)),
        ("gf 0", {"sec": 0, "sts": 0}, 0),
        ("ga4.3", {"sec": 4, "ms": 4300, "after": 1, "sts": 0}, after(4300)),
        ("gf 9", {"sec": 9, "sts": 0}, before(9000)),
        ("gc4.000", {"sec": 4, "ms": 4000, "after": 0, "sts": 0}, before(4000)),
        ("ga13", {"sec": 13, "ms": 13000, "after": 1, "sts": 0}, after(13000)),
    ]
    player = Player(player_path, str(media), EP3_FAKE_VIDEO_LEAD="200")
    try:
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        time.sleep(0.5)
        for command, ack, expected in cases:
            ms = landing(player, command, ack)
            # the first picture after the seek, a little later at most
            assert expected <= ms <= expected + 150, (media.name, command, ms, expected, frames)
            print(f"  {media.name}: {command} -> {ms} ms (keyframe {expected})")
            time.sleep(0.3)
    finally:
        player.stop()


def start_us(ffmpeg, media):
    """the start_time of the input (us), as FFmpeg reports it"""
    info = subprocess.run([ffmpeg, "-hide_banner", "-i", str(media)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60).stderr.decode()
    return round(float(re.search(r"start: (-?[0-9.]+)", info).group(1)) * 1000000)


def from_start(chapters, origin_us):
    """(s, e, t) in ms of the file to the time line of the player: from the
    start of the media, rounded down, 0 before it"""
    def ms(value):
        us = value * 1000 - origin_us
        return us // 1000 if us > 0 else 0
    return [(ms(s), ms(e), t) for s, e, t in chapters]


def check_offset(player_path, ffmpeg, media):
    """A file that starts at 10 s: s and e from the start of the media, the
    base of J and gf/ga, so that ga<s/1000> lands on the chapter."""
    info = subprocess.run([ffmpeg, "-hide_banner", "-i", str(media)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60).stderr.decode()
    start = float(re.search(r"start: (-?[0-9.]+)", info).group(1))
    marks = [(float(s), float(e)) for s, e in re.findall(r"Chapter #0:\d+: start ([0-9.]+), end ([0-9.]+)", info)]
    assert start >= 5 and len(marks) == 4, info

    def relative(seconds):
        return max(0, round((seconds - start) * 1000000) // 1000)
    expected = [(relative(s), relative(e)) for s, e in marks]
    assert expected[0][0] == 0, (expected, info)
    frames = keyframes(ffmpeg, media)
    player = Player(player_path, str(media), EP3_FAKE_VIDEO_LEAD="200")
    try:
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        chapters = player.wait("c_l")
        assert [(c["s"], c["e"]) for c in chapters] == expected, (chapters, expected, info)
        time.sleep(0.5)
        on_keyframe, between = chapters[2]["s"], chapters[1]["s"]
        assert on_keyframe in frames and between not in frames, (chapters, frames)
        cases = [  # command, the target keyframe
            ("ga%.3f" % (on_keyframe / 1000), on_keyframe),
            ("gf9", max(k for k in frames if k <= 9000)),
            ("gf%.3f" % (between / 1000), max(k for k in frames if k <= between)),
        ]
        for command, target in cases:
            ms = landing(player, command, {"sts": 0})
            assert target <= ms <= target + 150, (command, ms, target, frames)
            print(f"  {media.name}: {command} -> {ms} ms (keyframe {target})")
            time.sleep(0.3)
    finally:
        player.stop()
    print(f"PASS chapters: start {start} s, c_l {expected} from the start of the media, ga/gf land on it")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="exteplayer3-chapters-") as directory:
        root = Path(directory)
        video = root / "video.yuv"
        with video.open("wb") as stream:
            for frame in range(500):
                stream.write(bytes(16 + (i * 17 + frame * 11) % 220 for i in range(4096)))
                stream.write(bytes([128]) * 2048)
        audio = root / "audio.pcm"
        samples = b"".join(struct.pack("<hh", int(8000 * math.sin(i * math.pi / 48)),
                                       int(8000 * math.sin(i * math.pi / 48)))
                           for i in range(4800))
        audio.write_bytes(samples * 120)
        raw_video = ["-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
                     "-framerate", "25", "-i", str(video)]
        raw_audio = ["-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(audio)]

        media = root / "media.mkv"
        run(args.ffmpeg, "-y", *raw_video, *raw_audio, "-c:v", "mpeg2video", "-c:a", "aac",
            "-b:a", "96k", "-t", "12", str(media))

        # Matroska: start and end in ms exactly, one chapter without a title
        # (the demuxer keeps only chapters that start after the one before)
        matroska = [(0, 4321, "Intro"), (4321, 8000, QUOTED), (8000, 11500, UTF8), (11500, 12000, None)]
        meta = root / "matroska.txt"
        ffmetadata(meta, matroska)
        mkv = root / "chapters.mkv"
        run(args.ffmpeg, "-y", "-i", str(media), "-i", str(meta), "-map", "0", "-map_chapters", "1",
            "-c", "copy", str(mkv))
        origin = start_us(args.ffmpeg, mkv)
        expected = c_l_line(from_start(matroska, origin))

        def exact(line):
            assert line == expected, (line, expected)
        line = check_open(args.player, mkv, exact)
        assert UTF8.encode() in line and b'Say \\"hi\\" \\\\ back' in line, line
        print(f"PASS chapters: Matroska c_l exact (ms, escaped and UTF-8 titles, no title; start {origin} us),"
              " after the track lists, cl")

        def listed(target, chapters):
            """s and t exactly, e the start of the next one (FFmpeg fills in the ends)"""
            chapters = from_start(chapters, start_us(args.ffmpeg, target))
            found = []

            def keep(line):
                found.append(json.loads(line)["c_l"])
            check_open(args.player, target, keep)
            got = found[0]
            assert [c["id"] for c in got] == list(range(len(chapters))), got
            assert [(c["s"], c["t"]) for c in got] == [(s, t or "") for s, _, t in chapters], got
            assert [c["e"] for c in got[:-1]] == [c["s"] for c in got[1:]], got
            assert got[-1]["e"] >= got[-1]["s"], got
            print(f"PASS chapters: {target.name} c_l {[(c['s'], c['e']) for c in got]}")

        # MP4 and M4A (audiobooks): the chapters one after the other
        contiguous = [(0, 4321, "Intro"), (4321, 8000, QUOTED), (8000, 12000, UTF8)]
        meta = root / "contiguous.txt"
        ffmetadata(meta, contiguous)
        mp4 = root / "chapters.mp4"
        run(args.ffmpeg, "-y", "-i", str(media), "-i", str(meta), "-map", "0", "-map_chapters", "1",
            "-c", "copy", str(mp4))
        m4a = root / "book.m4a"
        run(args.ffmpeg, "-y", *raw_audio, "-i", str(meta), "-map", "0", "-map_chapters", "1",
            "-c:a", "aac", "-t", "12", str(m4a))
        listed(mp4, contiguous)
        listed(m4a, contiguous)

        # Ogg (Opus, CHAPTERxxx comments): in the file not in the order of their
        # start, the player sorts them. The muxer rounds the seconds of the
        # time it writes (11.500 s became 00:00:12.500): fractions below .5.
        unsorted = [(4321, 8000, QUOTED), (0, 4321, "Intro"), (11400, 12000, None), (8000, 11400, UTF8)]
        meta = root / "unsorted.txt"
        ffmetadata(meta, unsorted)
        ogg = root / "chapters.opus"
        run(args.ffmpeg, "-y", *raw_audio, "-i", str(meta), "-map", "0", "-map_chapters", "1",
            "-c:a", "opus", "-strict", "-2", "-t", "12", "-f", "ogg", str(ogg))
        listed(ogg, sorted(unsorted))

        # without chapters
        def empty(line):
            assert line == b'{"c_l":[]}\n', line
        check_open(args.player, media, empty)
        print("PASS chapters: an input without chapters sends an empty c_l")

        player = Player(args.player, str(media))
        try:
            banner = player.wait("EPLAYER3_EXTENDED")
            assert banner.get("c_l") == 1 and banner.get("seekms") == 1, banner
            assert banner.get("br") == 1, banner
            # q during the open ends with exit code 11
            assert player.wait("PLAYBACK_PLAY")["sts"] == 0
            # BR: the byte counters of the selected tracks, in the format
            # ServiceApp reads; they only grow while the file plays
            counters = []
            for _ in range(2):
                time.sleep(0.5)
                player.command("BR")
                player.wait("BR")
                line = player.raw("BR")[-1]
                match = re.fullmatch(rb'\{"BR":\{"v":(\d+),"a":(\d+)\}\}\n', line)
                assert match, line
                counters.append((int(match.group(1)), int(match.group(2))))
            assert all(v > 0 and a > 0 for v, a in counters), counters
            assert counters[1][0] >= counters[0][0] and counters[1][1] >= counters[0][1], counters
            # only "BR" itself: neither "BRx" nor "B" answers
            player.command("BRx")
            player.command("B")
            player.command("BR")
            player.wait("BR")
            time.sleep(0.2)
            assert len(player.raw("BR")) == 3, player.raw("BR")
        finally:
            player.stop()
        print("PASS chapters: EPLAYER3_EXTENDED has c_l, seekms and br; BR answers the byte counters")

        # Seeks: keyframes at 0, 4.2, 4.6, 8 and 12 s, no B-frames
        seek_mkv = root / "seek.mkv"
        run(args.ffmpeg, "-y", *raw_video, "-c:v", "mpeg2video", "-g", "300", "-bf", "0",
            "-sc_threshold", "1000000000", "-force_key_frames", "0,4.2,4.6,8,12", "-an", str(seek_mkv))
        seek_mp4 = root / "seek.mp4"
        run(args.ffmpeg, "-y", "-i", str(seek_mkv), "-c", "copy", str(seek_mp4))
        for target in (seek_mkv, seek_mp4):
            frames = keyframes(args.ffmpeg, target)
            print(f"  {target.name}: keyframes {frames}")
            check_seeks(args.player, target, frames)
        print("PASS chapters: gf<seconds.ms> on the keyframe before the target, ga on the one at or after it")

        # The same video muxed to start at 10 s, chapters at 10, 14.321, 14.6
        # (a keyframe) and 18 s of that time line (-copyts keeps them there)
        shifted = root / "shifted.mkv"
        run(args.ffmpeg, "-y", "-i", str(seek_mkv), "-c", "copy", "-output_ts_offset", "10", str(shifted))
        meta = root / "offset.txt"
        ffmetadata(meta, [(10000, 14321, "Anfang"), (14321, 14600, "Zwischen"), (14600, 18000, "Mitte"),
                          (18000, 22000, "Ende")])
        offset = root / "offset.mkv"
        run(args.ffmpeg, "-y", "-copyts", "-i", str(shifted), "-i", str(meta), "-map", "0",
            "-map_chapters", "1", "-c", "copy", str(offset))
        check_offset(args.player, args.ffmpeg, offset)


if __name__ == "__main__":
    main()
