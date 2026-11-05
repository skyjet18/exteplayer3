"""Native-player and loopback HTTP regression fixtures; no receiver required."""

import argparse
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
    def __init__(self, executable, *args):
        self.events = queue.Queue()
        self.messages = []
        self.raw_stderr = []
        self.diagnostic_error = False
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [executable, *args], stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.PIPE, text=True)
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
        embedded = []
        for extension, codec in (("mp4", "mov_text"), ("mkv", "copy")):
            target = root / ("subtitles." + extension)
            run(args.ffmpeg, "-y", "-i", str(media), "-f", "srt", "-i", str(subtitles),
                "-f", "srt", "-i", str(subtitles), "-map", "0:v", "-map", "0:a",
                "-map", "1", "-map", "2", "-c", "copy", "-c:s", codec,
                "-metadata:s:s:0", "language=eng", "-metadata:s:s:1", "language=deu",
                "-disposition:s:0", "forced", "-disposition:s:1", "0", str(target))
            embedded.append(target)

        requests = []
        payload = media.read_bytes()
        master = (b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\nlow.m3u8\n"
                  b"#EXT-X-STREAM-INF:BANDWIDTH=900000\nhigh.m3u8\n")
        resources = {"/media.ts": payload, "/low.ts": payload, "/high.ts": payload,
                     "/master.m3u8": master}
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
                start = 0
                header = self.headers.get("Range")
                if header:
                    start = int(header.split("=")[1].split("-")[0])
                requests.append((self.path, start))
                self.send_response(206 if header else 200)
                self.send_header("Content-Length", str(len(data) - start))
                self.send_header("Accept-Ranges", "bytes")
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
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
