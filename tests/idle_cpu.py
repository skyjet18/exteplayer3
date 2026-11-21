"""CPU time and wake-ups of the host player while it waits for its decoders.

The player is built with HOST_AUDIO_OUTPUT and HOST_VIDEO_OUTPUT of
tests/build.sh set to --audio-output and --video-output. Both become FIFOs
here: the "decoders", which this test drains while the player plays and stops
draining while it is paused, as paused receiver decoders take no more data.
Measured from /proc over a few seconds: the CPU time (utime + stime) and the
voluntary context switches of all threads of the player, playing and paused,
unbuffered, with the write buffer (-b) and with EP3_OUTPUT_PACING_MS. A
paused player waits for the continue or for room in a decoder, it does not
wake up to look again every few milliseconds. The continue and the quit of a
paused player are timed as well."""

import argparse
import fcntl
import os
from pathlib import Path
import struct
import sys
import tempfile
import termios
import threading
import time

from integration import Player, run

PIPE_SIZE = 16384
WARMUP = 1.5
PLAYING = 3.0
SETTLE = 2.0
PAUSED = 5.0
# voluntary context switches of all threads during PAUSED seconds: a 1 ms
# retry makes thousands, a 10 ms poll hundreds
MAX_PAUSED_SWITCHES = 150


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as stat:
        fields = stat.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def context_switches(pid):
    total = 0
    for task in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{task}/status") as status:
                for line in status:
                    if line.startswith("voluntary_ctxt_switches:"):
                        total += int(line.split()[1])
        except FileNotFoundError:
            pass
    return total


def measure(pid, seconds):
    ticks, switches, start = cpu_ticks(pid), context_switches(pid), time.monotonic()
    time.sleep(seconds)
    elapsed = time.monotonic() - start
    return cpu_ticks(pid) - ticks, context_switches(pid) - switches, elapsed


class Decoders:
    """The two output FIFOs, drained by a thread while running is set."""

    def __init__(self, paths):
        self.paths, self.fds = paths, []
        for path in paths:
            assert not path.exists(), f"{path}: use new isolated output paths"
            os.mkfifo(path, 0o600)
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, PIPE_SIZE)
            self.fds.append(fd)
        self.running = threading.Event()
        self.running.set()
        self.done = False
        self.received = 0
        self.thread = threading.Thread(target=self.drain, daemon=True)
        self.thread.start()

    def drain(self):
        while not self.done:
            if not self.running.wait(0.05):
                continue
            got = False
            for fd in self.fds:
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    data = b""
                if data:
                    self.received += len(data)
                    got = True
            if not got:
                time.sleep(0.002)

    def queued(self):
        return sum(struct.unpack("i", fcntl.ioctl(fd, termios.FIONREAD, struct.pack("i", 0)))[0]
                   for fd in self.fds)

    def close(self):
        self.done = True
        self.running.set()
        self.thread.join(timeout=2)
        for fd in self.fds:
            os.close(fd)
        for path in self.paths:
            path.unlink()


def case(args, media, name, options, env_extra):
    env = dict(os.environ)
    env.pop("EP3_OUTPUT_PACING_MS", None)
    env.update(env_extra)
    decoders = Decoders([Path(args.audio_output), Path(args.video_output)])
    player = None
    result = {"name": name}
    try:
        player = Player(args.player, *options, str(media), env=env)
        pid = player.process.pid
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        time.sleep(WARMUP)
        result["playing"] = measure(pid, PLAYING)
        player.command("p")
        assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
        decoders.running.clear()
        time.sleep(SETTLE)
        result["paused"] = measure(pid, PAUSED)
        # continue: how soon new data comes after what was queued
        queued = decoders.queued()
        mark = decoders.received
        player.command("c")
        assert player.wait("PLAYBACK_CONTINUE")["sts"] == 0
        start = time.monotonic()
        decoders.running.set()
        while decoders.received - mark <= queued and time.monotonic() - start < 5:
            time.sleep(0.001)
        result["continue_ms"] = (time.monotonic() - start) * 1000
        time.sleep(1)
        # quit while paused, with full decoders
        player.command("p")
        assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
        decoders.running.clear()
        time.sleep(SETTLE)
        start = time.monotonic()
        player.command("q")
        player.process.wait(timeout=10)
        result["quit_ms"] = (time.monotonic() - start) * 1000
    finally:
        if player:
            player.stop()
        decoders.close()
    return result


def describe(label, sample):
    ticks, switches, elapsed = sample
    hz = os.sysconf("SC_CLK_TCK")
    return (f"{label} {elapsed:.1f} s: {ticks} ticks ({100.0 * ticks / hz / elapsed:.1f} % CPU), "
            f"{switches} context switches ({switches / elapsed:.0f}/s)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True,
                        help="host player built with HOST_AUDIO_OUTPUT/HOST_VIDEO_OUTPUT below")
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--audio-output", required=True, help="HOST_AUDIO_OUTPUT of the player")
    parser.add_argument("--video-output", required=True, help="HOST_VIDEO_OUTPUT of the player")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        media = Path(directory) / "noise.ts"
        # intra frames at the finest quantizer, several Mbit/s: the decoders
        # and the 1 MB write buffer fill at once
        run(args.ffmpeg, "-y", "-v", "error",
            "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-c:v", "mpeg2video", "-g", "1", "-qmin", "1", "-q:v", "1",
            "-c:a", "aac", "-b:a", "128k", "-t", "30", str(media))
        results = [
            case(args, media, "unbuffered", [], {}),
            case(args, media, "write buffer -b 1", ["-b", "1"], {}),
            case(args, media, "EP3_OUTPUT_PACING_MS=500", [], {"EP3_OUTPUT_PACING_MS": "500"}),
        ]

    failed = []
    for result in results:
        print(f"{result['name']}: " + describe("playing", result["playing"]) + "; "
              + describe("paused", result["paused"]) + f"; continue {result['continue_ms']:.0f} ms"
              + f", quit while paused {result['quit_ms']:.0f} ms")
        if result["paused"][1] > MAX_PAUSED_SWITCHES:
            failed.append(f"{result['name']}: {result['paused'][1]} context switches while paused"
                          f" (at most {MAX_PAUSED_SWITCHES})")
        if result["continue_ms"] > 500:
            failed.append(f"{result['name']}: no data {result['continue_ms']:.0f} ms after the continue")
        if result["quit_ms"] > 2000:
            failed.append(f"{result['name']}: quit took {result['quit_ms']:.0f} ms")
    sys.stdout.flush()
    if failed:
        print("FAIL " + "; ".join(failed))
        sys.exit(1)
    print("PASS a paused player waits without polling; continue and quit react at once")


if __name__ == "__main__":
    main()
