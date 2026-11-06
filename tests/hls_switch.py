"""Verify mixed-TS HLS audio switches by identifying decoded PCM tones."""

import argparse
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import time

from integration import Player, read_fifo


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
    args = parser.parse_args()
    sys.path.insert(0, str(Path(os.environ["FFMPEG_SOURCE"]) / "tests"))
    from hls_http import generate, serve

    fifo = Path(args.pcm_fifo)
    assert not fifo.exists(), "Use a new isolated FIFO, never a production endpoint"
    with tempfile.TemporaryDirectory(prefix="exteplayer3-hls-") as directory:
        root = Path(directory)
        generate(root, args.ffmpeg)
        server, thread = serve(root)
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for live in (False, True):
                for initial in (0, 1):
                    for sync in (0, 1):
                        server.latest_segment = 0
                        server.windows.clear()
                        os.mkfifo(fifo, 0o600)
                        reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                        buffered = bytearray()
                        suffix = "-live" if live else ""
                        player = None
                        try:
                            player = Player(args.player, "-K", "-L", "FFMPEG_DEBUG_LEVEL:10",
                                            "-L", "FFMPEG_SILENT:0",
                                            "-t", "2" if initial == 0 else "0",
                                            base + f"/master{suffix}.m3u8",
                                            env=dict(os.environ, EP3_LIVE_SYNC=str(sync)))
                            assert player.wait("PLAYBACK_PLAY")["sts"] == 0
                            video = player.wait("v_c")
                            tracks = player.wait("a_l")
                            assert len(tracks) == 3, tracks
                            ids = [next(track["id"] for track in tracks if track["n"] == language)
                                   for language in ("sk", "en", "de")]
                            assert len(set(ids)) == 3 and video["id"] not in ids, (video, tracks)
                            assert player.wait("a_c")["id"] == ids[initial]
                            observe(reader, buffered, (400, 800, 1200)[initial])
                            for track in ((initial + i) % 3 for i in range(7)):
                                player.command("j")
                                before = player.wait("J")["ms"]
                                player.command(f"a{ids[track]}")
                                status = player.wait("a_s")
                                assert status["sts"] == 0 and status["id"] == ids[track], status
                                observe(reader, buffered, (400, 800, 1200)[track])
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
                                os.close(reader)
                                fifo.unlink()
                        print(f"PASS selected HLS PCM live={live} initial={initial} sync={sync}: "
                              "all seven a<ID> selections audible, video progresses")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
