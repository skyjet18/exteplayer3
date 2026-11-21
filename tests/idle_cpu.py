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
paused player are timed as well.

Commands right after a continue, unbuffered and with -b 8: the longest answer
to `j` in the 1.5 s after each of three continues, and a seek back sent at
once after a continue (its answer, the first position at the new place). The
host outputs have no driver that sleeps in its poll; the delay that caused on
the Vu+ is covered by tests/video_info_test.c, here the command path is
timed.

-b 8 with a driver that sleeps in its poll: the Broadcom driver of the Vu+
sleeps in its poll callback while the decoder is full, up to 5 s when a pause
comes meanwhile, and neither the continue nor the wake-up pipe of the write
buffer ends that sleep. A preloaded select() (built here with $CC) does the
same for the FIFO decoders, DRIVER_SLEEP_MS per call. Pause, continue, and at
once a seek back and an audio track switch: both answer at once, and the
position is at the new place once that sleep has ended. That no data of the
old position reaches the device after the clear is checked by
tests/buffering_chunk_test.c (the host FIFO cannot be cleared)."""

import argparse
import fcntl
import os
from pathlib import Path
import struct
import subprocess
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
# commands right after a continue: the longest answer to j, a seek back of
# SEEK s (back: the host video output paces a jump ahead in real time)
MAX_ANSWER_MS = 250
MAX_SEEK_POSITION_MS = 1500
SEEK = 6
# the sleep of the preloaded select() on a full FIFO decoder
DRIVER_SLEEP_MS = 2000

DRIVER_SLEEP_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

/* A FIFO decoder in the write set that has no room: sleep "in the driver",
 * EP3_TEST_DRIVER_SLEEP_MS, whatever else happens meanwhile, then report
 * what is ready now. */
int select(int count, fd_set *readable, fd_set *writable, fd_set *failed, struct timeval *timeout)
{
    static int (*real)(int, fd_set *, fd_set *, fd_set *, struct timeval *);
    const char *sleep_ms = getenv("EP3_TEST_DRIVER_SLEEP_MS");
    int fd;
    if (!real)
        real = (int (*)(int, fd_set *, fd_set *, fd_set *, struct timeval *))dlsym(RTLD_NEXT, "select");
    for (fd = 0; writable && sleep_ms && fd < count; fd++)
    {
        struct stat st;
        struct pollfd room = {fd, POLLOUT, 0};
        if (FD_ISSET(fd, writable) && !fstat(fd, &st) && S_ISFIFO(st.st_mode) && poll(&room, 1, 0) == 0)
        {
            long ms = atol(sleep_ms);
            struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
            struct timeval now = {0, 0};
            while (nanosleep(&pause, &pause))
                ;
            return real(count, readable, writable, failed, &now);
        }
    }
    return real(count, readable, writable, failed, timeout);
}
"""


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


def timed(player, command, name):
    start = time.monotonic()
    player.command(command)
    answer = player.wait(name)
    return answer, (time.monotonic() - start) * 1000


def responsiveness(args, media, name, options):
    env = dict(os.environ)
    env.pop("EP3_OUTPUT_PACING_MS", None)
    decoders = Decoders([Path(args.audio_output), Path(args.video_output)])
    player = None
    result = {"name": name, "j_ms": []}
    try:
        player = Player(args.player, *options, str(media), env=env)
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        time.sleep(WARMUP)
        for _ in range(3):
            player.command("p")
            assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
            decoders.running.clear()
            time.sleep(1.0)
            player.command("c")
            assert player.wait("PLAYBACK_CONTINUE")["sts"] == 0
            decoders.running.set()
            worst, until = 0.0, time.monotonic() + 1.5
            while time.monotonic() < until:
                worst = max(worst, timed(player, "j", "J")[1])
                time.sleep(0.03)
            result["j_ms"].append(worst)
        # a seek sent at once after the continue
        player.command("p")
        assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
        decoders.running.clear()
        time.sleep(1.0)
        before = timed(player, "j", "J")[0]["ms"]
        player.command("c")
        assert player.wait("PLAYBACK_CONTINUE")["sts"] == 0
        decoders.running.set()
        start = time.monotonic()
        answer, result["seek_ack_ms"] = timed(player, f"kc -{SEEK}", "PLAYBACK_SEEK")
        assert answer["sts"] == 0
        result["seek_pos_ms"] = None
        while time.monotonic() - start < 5:
            if timed(player, "j", "J")[0]["ms"] <= before - (SEEK - 2) * 1000:
                result["seek_pos_ms"] = (time.monotonic() - start) * 1000
                break
            time.sleep(0.02)
    finally:
        if player:
            player.stop()
        decoders.close()
    return result


def driver_sleep(args, media, name, options, preload):
    """-b with the sleeping driver poll: a seek back and an audio switch at
    once after a continue (see the module text)."""
    env = dict(os.environ)
    env.pop("EP3_OUTPUT_PACING_MS", None)
    env["LD_PRELOAD"] = str(preload)
    env["EP3_TEST_DRIVER_SLEEP_MS"] = str(DRIVER_SLEEP_MS)
    decoders = Decoders([Path(args.audio_output), Path(args.video_output)])
    player = None
    result = {"name": name, "seek_pos_ms": None}
    try:
        player = Player(args.player, *options, str(media), env=env)
        assert player.wait("PLAYBACK_PLAY")["sts"] == 0
        # far enough into the medium for the seek back
        time.sleep(3 * SEEK / 2)
        for action, command, answer_name in (("seek", f"kc -{SEEK}", "PLAYBACK_SEEK"),
                                             ("switch", "ai1", "a_s")):
            player.command("p")
            assert player.wait("PLAYBACK_PAUSE")["sts"] == 0
            decoders.running.clear()
            # the decoders fill, the write buffer sleeps in the "driver"
            time.sleep(0.5)
            before = timed(player, "j", "J")[0]["ms"]
            player.command("c")
            assert player.wait("PLAYBACK_CONTINUE")["sts"] == 0
            decoders.running.set()
            start = time.monotonic()
            answer, result[f"{action}_ack_ms"] = timed(player, command, answer_name)
            assert answer["sts"] == 0
            if action == "seek":
                while time.monotonic() - start < DRIVER_SLEEP_MS / 1000.0 + 3:
                    if timed(player, "j", "J")[0]["ms"] <= before - (SEEK - 2) * 1000:
                        result["seek_pos_ms"] = (time.monotonic() - start) * 1000
                        break
                    time.sleep(0.02)
            # the sleep that was running ends before the next round
            time.sleep(DRIVER_SLEEP_MS / 1000.0 + 0.5)
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
        answers = [
            responsiveness(args, media, "unbuffered", []),
            responsiveness(args, media, "write buffer -b 8", ["-b", "8"]),
        ]
        # two audio tracks for the switch
        media2 = Path(directory) / "noise2.ts"
        run(args.ffmpeg, "-y", "-v", "error",
            "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000",
            "-map", "0:v", "-map", "1:a", "-map", "2:a",
            "-c:v", "mpeg2video", "-g", "1", "-qmin", "1", "-q:v", "1",
            "-c:a", "aac", "-b:a", "128k", "-t", "30", str(media2))
        preload = Path(directory) / "driver_sleep.so"
        source = Path(directory) / "driver_sleep.c"
        source.write_text(DRIVER_SLEEP_SOURCE)
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-o", str(preload), str(source), "-ldl"],
                       check=True)
        sleeping = [driver_sleep(args, media2, "write buffer -b 8, sleeping driver poll", ["-b", "8"], preload)]

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
    for result in answers:
        position = result["seek_pos_ms"]
        print(f"{result['name']}, commands after a continue: longest j answer "
              + ", ".join(f"{ms:.0f}" for ms in result["j_ms"]) + " ms; seek at once after it: answer "
              + f"{result['seek_ack_ms']:.0f} ms, new position "
              + (f"{position:.0f} ms" if position is not None else "not within 5 s"))
        if max(result["j_ms"]) > MAX_ANSWER_MS:
            failed.append(f"{result['name']}: j answered after {max(result['j_ms']):.0f} ms after a continue"
                          f" (at most {MAX_ANSWER_MS})")
        if result["seek_ack_ms"] > MAX_ANSWER_MS:
            failed.append(f"{result['name']}: seek after a continue answered after {result['seek_ack_ms']:.0f} ms")
        if position is None or position > MAX_SEEK_POSITION_MS:
            failed.append(f"{result['name']}: no new position {MAX_SEEK_POSITION_MS} ms after a seek after a continue")
    for result in sleeping:
        position = result["seek_pos_ms"]
        print(f"{result['name']}: pause, continue, at once a seek back: answer {result['seek_ack_ms']:.0f} ms,"
              " new position " + (f"{position:.0f} ms" if position is not None else "not reached")
              + f"; an audio switch: answer {result['switch_ack_ms']:.0f} ms")
        for action in ("seek", "switch"):
            if result[f"{action}_ack_ms"] > MAX_ANSWER_MS:
                failed.append(f"{result['name']}: {action} after a continue answered after"
                              f" {result[f'{action}_ack_ms']:.0f} ms (at most {MAX_ANSWER_MS})")
        if position is None:
            failed.append(f"{result['name']}: no new position {DRIVER_SLEEP_MS + 3000} ms after the seek")
    sys.stdout.flush()
    if failed:
        print("FAIL " + "; ".join(failed))
        sys.exit(1)
    print("PASS a paused player waits without polling; continue, quit and the commands after a continue"
          " react at once")


if __name__ == "__main__":
    main()
