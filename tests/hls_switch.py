"""Verify mixed-TS HLS switching using decoded tones or source-time/PES alignment."""

import argparse
import fcntl
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zlib

from integration import Player, read_fifo


class PesCapture:
    def __init__(self, path, stream_id, fd=None):
        self.path, self.stream_id, self.fd = path, stream_id, fd
        self.offset = 0
        self.buffer = bytearray()

    def read(self):
        if self.fd is not None:
            self.buffer.extend(read_fifo(self.fd))
        elif self.path.exists():
            with self.path.open("rb") as capture:
                capture.seek(self.offset)
                self.buffer.extend(capture.read())
                self.offset = capture.tell()
        packets = []
        while len(self.buffer) >= 9:
            assert self.buffer[:4] == b"\x00\x00\x01" + bytes([self.stream_id]), self.buffer[:16]
            size = 6 + int.from_bytes(self.buffer[4:6], "big")
            assert size >= 9 + self.buffer[8]
            if len(self.buffer) < size:
                break
            pts = None
            if self.buffer[7] & 0x80:
                assert self.buffer[8] >= 5
                p = self.buffer[9:14]
                assert p[0] & 1 and p[2] & 1 and p[4] & 1
                pts = ((p[0] & 14) << 29 | p[1] << 22 | (p[2] & 254) << 14 |
                       p[3] << 7 | p[4] >> 1)
            packets.append((pts, bytes(self.buffer[9 + self.buffer[8]:size])))
            del self.buffer[:size]
        return packets


class TimedOutput:
    def __init__(self, audio, video, reference):
        self.audio, self.video, self.reference = audio, video, reference
        self.offset = None
        self.last_video_source = None
        self.last_pts = {}

    def observe(self, track, duration=2.4, switched=None):
        started = last_video = last_audio = time.monotonic()
        recovery_started = started if switched is None else switched
        seen_audio = False
        video_source = []
        while time.monotonic() - started < duration:
            now = time.monotonic()
            for kind, capture in ((-1, self.video), (track, self.audio)):
                for pts, payload in capture.read():
                    if kind == -1:
                        # The MPEG-2 writer may repeat sequence headers before a GOP.
                        payload = payload[payload.index(b"\x00\x00\x01\x00"):]
                    fingerprint = f"{zlib.adler32(payload):08x}"
                    candidates = self.reference.get((kind, fingerprint))
                    if candidates is None and kind != -1 and now - recovery_started < 1:
                        continue  # An in-flight PES from the previous selection.
                    assert candidates, (kind, fingerprint, payload[:24])
                    candidates = sorted(candidates)
                    if pts is None:
                        # AAC encoder preroll can precede the player's zero origin.
                        assert kind != -1 and not seen_audio and now - started < 1
                        assert self.offset is not None and any(
                            -9000 < round(source * 90000 / 1000000) + self.offset < 0
                            for source in candidates)
                        continue
                    offsets = [pts - round(source * 90000 / 1000000)
                               for source in candidates]
                    if self.offset is None:
                        assert kind == -1
                        self.offset = offsets[0]
                    errors = [abs((offset - self.offset + (1 << 32)) % (1 << 33) -
                                  (1 << 32)) for offset in offsets]
                    assert min(errors) <= 9000, (kind, pts, candidates, self.offset, errors)
                    if kind == -1:
                        last_video = now
                        source = candidates[errors.index(min(errors))]
                        if self.last_video_source is not None:
                            assert 0 < source - self.last_video_source <= 80000, (
                                self.last_video_source, source)
                        self.last_video_source = source
                        video_source.append(source)
                    else:
                        if not seen_audio:
                            assert now - recovery_started < 1, "Selected audio recovery exceeded 1 s"
                        seen_audio = True
                        last_audio = now
                    self.last_pts["video" if kind == -1 else "audio"] = pts
            assert now - last_video < 1.5, "Video output stalled"
            assert now - last_audio < 1.5, "Audio output stalled"
            assert seen_audio or now - recovery_started < 1, "Selected audio did not recover within 1 s"
            if now - started >= 1:
                gap = (self.last_pts["audio"] - self.last_pts["video"] +
                       (1 << 32)) % (1 << 33) - (1 << 32)
                assert abs(gap) <= 22500, ("A/V injection gap", gap)
            time.sleep(0.01)
        assert seen_audio and len(video_source) > 5
        assert video_source[-1] > video_source[0] + 250000, video_source


def check_signal(pcm, frequency):
    assert len(pcm) >= 48000, f"Insufficient selected audio: {len(pcm)} bytes"
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)[::2][-12000:]
    crossings = sum((left < 0) != (right < 0)
                    for left, right in zip(samples, samples[1:]))
    measured = crossings * 48000 / (2 * len(samples))
    rms = math.sqrt(sum(value * value for value in samples) / len(samples))
    assert rms > 100 and abs(measured - frequency) < 30, (frequency, measured, rms)


class CapturedAudio:
    def __init__(self, path, mode, ffmpeg):
        self.path, self.mode, self.ffmpeg = path, mode, ffmpeg
        self.offset = 0
        self.buffer = bytearray()

    def observe(self, frequency):
        time.sleep(2.4)
        with self.path.open("rb") as capture:
            capture.seek(self.offset)
            self.buffer.extend(capture.read())
            self.offset = capture.tell()
        payloads = bytearray()
        while len(self.buffer) >= 9:
            assert self.buffer[:4] == b"\x00\x00\x01\xc0", self.buffer[:16]
            size = 6 + int.from_bytes(self.buffer[4:6], "big")
            assert size >= 9 + self.buffer[8]
            if len(self.buffer) < size:
                break
            payload = self.buffer[9 + self.buffer[8]:size]
            del self.buffer[:size]
            if self.mode == "software":
                # The PCM writer prepends sample size and WAVEFORMATEX.
                length = int.from_bytes(payload[:4], "big")
                assert len(payload) == 22 + length
                assert struct.unpack_from("<HHI", payload, 4) == (1, 2, 48000)
                payloads.extend(payload[22:])
            else:
                assert payload[:2] in (b"\xff\xf1", b"\xff\xf9"), payload[:16]
                payloads.extend(payload)
        if self.mode == "compressed":
            with tempfile.TemporaryDirectory(prefix="exteplayer3-aac-") as directory:
                encoded, decoded = Path(directory) / "audio.aac", Path(directory) / "audio.pcm"
                encoded.write_bytes(payloads)
                result = subprocess.run(
                    [self.ffmpeg, "-v", "error", "-f", "aac", "-i", str(encoded),
                     "-f", "s16le", "-acodec", "pcm_s16le", str(decoded)],
                    capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr.decode(errors="replace")
                payloads = decoded.read_bytes()
        check_signal(payloads, frequency)


def observe(reader, buffered, frequency, duration=2.4):
    deadline = time.monotonic() + 5
    matched = 0
    total = 0
    measured = rms = 0
    pending = []
    started = time.monotonic()
    while time.monotonic() < deadline:
        buffered.extend(read_fifo(reader))
        while len(buffered) >= 16:
            magic, rate, channels, bits, size = struct.unpack_from("<IIHHI", buffered)
            assert magic == 0x504D4341 and rate == 48000 and channels == 2 and bits == 16
            assert 0 < size <= 4096 and size % 4 == 0
            if len(buffered) < 16 + size:
                break
            samples = struct.unpack_from(f"<{size // 2}h", buffered, 16)[::2]
            del buffered[:16 + size]
            pending.extend(samples)
            if len(pending) < 4800:
                continue
            samples = pending[:4800]
            del pending[:4800]
            crossings = sum((left < 0) != (right < 0)
                            for left, right in zip(samples, samples[1:]))
            measured = crossings * rate / (2 * len(samples))
            rms = math.sqrt(sum(value * value for value in samples) / len(samples))
            total += len(samples)
            if rms > 100 and abs(measured - frequency) < 100:
                matched += len(samples)
            else:
                matched = 0
            # Require a quarter-second of contiguous selected audio, and
            # keep reading across segment boundaries between switches.
            if matched >= rate // 4 and time.monotonic() - started >= duration:
                return
        time.sleep(0.005)
    raise AssertionError(f"No sustained {frequency} Hz PCM: matched={matched}, "
                         f"samples={total}, last_hz={measured}, rms={rms}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--pcm-fifo", required=True,
                        help="New isolated path compiled into the host player")
    parser.add_argument("--multiple", action="store_true")
    parser.add_argument("--video-first", action="store_true")
    parser.add_argument("--fmp4", action="store_true")
    parser.add_argument("--case", choices=("vod", "live"))
    parser.add_argument("--mode", choices=("export", "compressed", "software"), default="export")
    parser.add_argument("--audio-output", help="Isolated HOST_AUDIO_OUTPUT compiled into player")
    parser.add_argument("--quality", choices=("auto", "lowest", "all"), default="auto")
    parser.add_argument("--timing-driver", help="Companion hls_http binary; verify source-time/PES alignment")
    parser.add_argument("--video-output", help="Isolated HOST_VIDEO_OUTPUT compiled into player")
    parser.add_argument("--backpressure", action="store_true",
                        help="Use an isolated audio FIFO to exercise writer backpressure and shutdown")
    parser.add_argument("--sync", choices=("default", "0", "1"))
    parser.add_argument("--initial", type=int, choices=(0, 1))
    args = parser.parse_args()
    sys.path.insert(0, str(Path(os.environ["FFMPEG_SOURCE"]) / "tests"))
    from hls_http import generate, references, serve

    fifo = Path(args.pcm_fifo)
    assert not fifo.exists(), "Use a new isolated FIFO, never a production endpoint"
    sink = Path(args.audio_output) if args.audio_output else None
    video_sink = Path(args.video_output) if args.video_output else None
    if video_sink:
        assert not video_sink.exists(), "Use a new isolated video output file"
    assert not args.backpressure or args.timing_driver
    if args.timing_driver:
        assert args.mode == "compressed" and sink and video_sink
        assert args.case == "live", "Use the existing output-mode checks for VOD"
        assert not args.fmp4, "Source-fingerprint PES timing currently covers MPEG-TS"
    assert args.mode == "export" or sink, "Captured modes require --audio-output"
    if sink:
        assert not sink.exists(), "Use a new isolated output file"
    with tempfile.TemporaryDirectory(prefix="exteplayer3-hls-") as directory:
        root = Path(directory)
        generate(root, args.ffmpeg, multiple=args.multiple, video_first=args.video_first,
                 fragmented=args.fmp4, markers=bool(args.timing_driver),
                 duration=80 if args.timing_driver else 40,
                 audio_offset=0.18 if args.timing_driver else 0)
        reference = references(root, args.timing_driver, audio_offset_us=140000) if args.timing_driver else None
        server, thread = serve(root, realtime=bool(args.timing_driver), initial_window=3)
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for live in (False, True):
                if args.case and live != (args.case == "live"):
                    continue
                for initial in (0, 1):
                    if args.initial is not None and initial != args.initial:
                        continue
                    for sync in ((None,) if args.sync == "default" else (0, 1)):
                        if args.sync not in (None, "default", str(sync)):
                            continue
                        server.started = time.monotonic()
                        server.latest_segment = 0
                        server.windows.clear()
                        reader = None
                        audio_reader = None
                        if args.mode == "export":
                            os.mkfifo(fifo, 0o600)
                            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                        if args.backpressure:
                            os.mkfifo(sink, 0o600)
                            audio_reader = os.open(sink, os.O_RDONLY | os.O_NONBLOCK)
                            fcntl.fcntl(audio_reader, fcntl.F_SETPIPE_SZ, 8192)
                        capture = CapturedAudio(sink, args.mode, args.ffmpeg) if sink and not reference else None
                        timing = TimedOutput(PesCapture(sink, 0xc0, audio_reader),
                                             PesCapture(video_sink, 0xe0), reference) if reference else None
                        buffered = bytearray()
                        suffix = "-live" if live else ""
                        player = None
                        try:
                            mode = ["-K"] if args.mode == "export" else ["-a", "3" if args.mode == "software" else "0"]
                            quality = {"auto": [], "lowest": ["-Q", "1"],
                                       "all": ["-M", "1920x1080"]}[args.quality]
                            env = dict(os.environ)
                            if sync is None:
                                env.pop("EP3_LIVE_SYNC", None)
                            else:
                                env["EP3_LIVE_SYNC"] = str(sync)
                            player = Player(args.player, "-C", *mode, *quality,
                                            "-L", "FFMPEG_DEBUG_LEVEL:10",
                                            "-L", "FFMPEG_SILENT:0",
                                            "-t", "2" if initial == 0 else "0",
                                            base + f"/master{suffix}.m3u8",
                                            env=env)
                            assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                            video = player.wait("v_c")
                            tracks = player.wait("a_l")
                            assert len(tracks) == 3, tracks
                            expected = "A_AAC" if args.mode == "compressed" else "A_IPCM"
                            assert all(track["e"] == expected for track in tracks), tracks
                            ids = [next(track["id"] for track in tracks if track["n"] == language)
                                   for language in ("sk", "en", "de")]
                            assert len(set(ids)) == 3 and video["id"] not in ids, (video, tracks)
                            assert player.wait("a_c")["id"] == ids[initial]
                            def selected_audio(track, switched=None):
                                frequency = (400, 800, 1200)[track]
                                if timing:
                                    timing.observe(track, switched=switched)
                                elif args.mode == "export":
                                    observe(reader, buffered, frequency)
                                else:
                                    capture.observe(frequency)
                            if timing and live:
                                timing.observe(initial, duration=14)
                            else:
                                selected_audio(initial)
                            for track in ((initial + i) % 3 for i in range(7)):
                                player.command("j")
                                before = player.wait("J")["ms"]
                                switched = time.monotonic()
                                player.command(f"a{ids[track]}")
                                status = player.wait("a_s")
                                assert status["sts"] == 0 and status["id"] == ids[track], status
                                selected_audio(track, switched=switched)
                                player.command("al")
                                assert player.wait("a_l") == tracks
                                player.command("ac")
                                assert player.wait("a_c")["id"] == ids[track]
                                player.command("vc")
                                assert player.wait("v_c")["id"] == video["id"]
                                player.command("j")
                                after = player.wait("J")["ms"]
                                assert before + 250 <= after < before + 5000, (before, after)
                            if args.backpressure:
                                time.sleep(2)
                                queued = read_fifo(audio_reader)
                                assert len(queued) >= fcntl.fcntl(
                                    audio_reader, fcntl.F_GETPIPE_SZ) - 1024, len(queued)
                                timing.audio.buffer.extend(queued)
                                selected_audio(track)
                                time.sleep(2)
                                queued = struct.unpack("i", fcntl.ioctl(
                                    audio_reader, termios.FIONREAD, struct.pack("i", 0)))[0]
                                assert queued >= fcntl.fcntl(
                                    audio_reader, fcntl.F_GETPIPE_SZ) - 1024, queued
                            if live:
                                assert max(server.windows) > 0, server.windows
                            player.command("q")
                            player.process.wait(timeout=5)
                            player.log.seek(0)
                            diagnostics = player.log.read().decode(errors="replace")
                            assert "timestamp seek failed" not in diagnostics
                            if live:
                                assert "seek_target_seconds[" not in diagnostics
                                assert diagnostics.count("first video:") == 1, diagnostics[-6000:]
                        except AssertionError:
                            if player:
                                if player.process.poll() is None:
                                    player.command("q")
                                    player.process.wait(timeout=5)
                                player.log.seek(0)
                                print(player.log.read().decode(errors="replace")[-12000:])
                            raise
                        finally:
                            try:
                                if player:
                                    player.stop()
                            finally:
                                if reader is not None:
                                    os.close(reader)
                                    fifo.unlink()
                                if audio_reader is not None:
                                    os.close(audio_reader)
                                if sink and sink.exists():
                                    sink.unlink()
                                if video_sink and video_sink.exists():
                                    video_sink.unlink()
                        checks = ("source-time/PES alignment, continuous video and bounded shutdown"
                                  if timing else "selected audio content and video progress")
                        print(f"PASS selected HLS {args.mode} live={live} initial={initial} "
                              f"sync={'default' if sync is None else sync}: {checks}")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
