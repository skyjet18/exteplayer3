"""Verify mixed-TS HLS audio switches by identifying decoded PCM tones."""

import argparse
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

from integration import Player, read_fifo


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
    args = parser.parse_args()
    sys.path.insert(0, str(Path(os.environ["FFMPEG_SOURCE"]) / "tests"))
    from hls_http import generate, serve

    fifo = Path(args.pcm_fifo)
    assert not fifo.exists(), "Use a new isolated FIFO, never a production endpoint"
    sink = Path(args.audio_output) if args.audio_output else None
    assert args.mode == "export" or sink, "Captured modes require --audio-output"
    if sink:
        assert not sink.exists(), "Use a new isolated output file"
    with tempfile.TemporaryDirectory(prefix="exteplayer3-hls-") as directory:
        root = Path(directory)
        generate(root, args.ffmpeg, multiple=args.multiple, video_first=args.video_first,
                 fragmented=args.fmp4)
        server, thread = serve(root)
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for live in (False, True):
                if args.case and live != (args.case == "live"):
                    continue
                for initial in (0, 1):
                    for sync in (0, 1):
                        server.latest_segment = 0
                        server.windows.clear()
                        reader = None
                        if args.mode == "export":
                            os.mkfifo(fifo, 0o600)
                            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                        capture = CapturedAudio(sink, args.mode, args.ffmpeg) if sink else None
                        buffered = bytearray()
                        suffix = "-live" if live else ""
                        player = None
                        try:
                            mode = ["-K"] if args.mode == "export" else ["-a", "3" if args.mode == "software" else "0"]
                            quality = {"auto": [], "lowest": ["-Q", "1"],
                                       "all": ["-M", "1920x1080"]}[args.quality]
                            player = Player(args.player, "-C", *mode, *quality,
                                            "-L", "FFMPEG_DEBUG_LEVEL:10",
                                            "-L", "FFMPEG_SILENT:0",
                                            "-t", "2" if initial == 0 else "0",
                                            base + f"/master{suffix}.m3u8",
                                            env=dict(os.environ, EP3_LIVE_SYNC=str(sync)))
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
                            def selected_audio(track):
                                frequency = (400, 800, 1200)[track]
                                if args.mode == "export":
                                    observe(reader, buffered, frequency)
                                else:
                                    capture.observe(frequency)
                            selected_audio(initial)
                            for track in ((initial + i) % 3 for i in range(7)):
                                player.command("j")
                                before = player.wait("J")["ms"]
                                player.command(f"a{ids[track]}")
                                status = player.wait("a_s")
                                assert status["sts"] == 0 and status["id"] == ids[track], status
                                selected_audio(track)
                                player.command("al")
                                assert player.wait("a_l") == tracks
                                player.command("ac")
                                assert player.wait("a_c")["id"] == ids[track]
                                player.command("vc")
                                assert player.wait("v_c")["id"] == video["id"]
                                player.command("j")
                                after = player.wait("J")["ms"]
                                assert before + 250 <= after < before + 5000, (before, after)
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
                                if sink and sink.exists():
                                    sink.unlink()
                        print(f"PASS selected HLS {args.mode} live={live} initial={initial} sync={sync}: "
                              "all seven a<ID> selections audible, video progresses")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
