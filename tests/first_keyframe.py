"""The first video keyframe a little before the start time of the container.

Over HTTP the player cuts the stream analysis short (1 us), so for Matroska
with H.264, whose codec parameters come from the avcC in the header, FFmpeg
reads hardly any packet: when an audio block (2 ms) comes before the first
video keyframe (0 ms), the start time of the container is the one of the
audio. The player starts its timeline there; the keyframe before it was
dropped and the decoder got only frames that depend on it (on a Vu+ the
picture then started at the next keyframe, its timestamps running ahead).

The test writes such a file: H.264 written bit by bit (no encoder needed,
see h264_avcc_mp4.py), an IDR picture every 2 s and P pictures of skipped
macroblocks between them, PCM audio 2 ms later, the first audio block moved
in front of the first video block. It plays the file over HTTP and as a
local file with the player that captures its video PES (HOST_VIDEO_OUTPUT)
and checks that the first picture written is the IDR picture.

usage: first_keyframe.py --player PLAYER --ffmpeg FFMPEG --video-output PATH [--audio-output PATH]
the paths are the HOST_VIDEO_OUTPUT / HOST_AUDIO_OUTPUT compiled into PLAYER;
they are removed afterwards.
"""

import argparse
import functools
import http.server
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from h264_avcc_mp4 import Bits, idr, pps, sps  # noqa: E402

CLUSTER = 0x1F43B675
SEGMENT = 0x18538067
SIMPLE_BLOCK = 0xA3
GOP = 50      # an IDR picture every 2 s at 25 fps
FRAMES = 200  # 8 s


def p_skip(frame_num):
    """P picture whose 16 macroblocks are all skipped (a reference picture)."""
    b = Bits()
    b.ue(0)                 # first_mb_in_slice
    b.ue(5)                 # slice_type: P, all slices
    b.ue(0)                 # pic_parameter_set_id
    b.put(frame_num % 16, 4)  # frame_num (log2_max_frame_num 4)
    b.put(0, 1)             # num_ref_idx_active_override_flag
    b.put(0, 1)             # ref_pic_list_modification_flag_l0
    b.put(0, 1)             # adaptive_ref_pic_marking_mode_flag
    b.ue(0)                 # slice_qp_delta (se 0)
    b.ue(1)                 # disable_deblocking_filter_idc
    b.ue(16)                # mb_skip_run: every macroblock
    return b.nal(0x41)


def h264_stream():
    start = b"\0\0\0\1"
    out = [start + sps(), start + pps(0)]
    for n in range(FRAMES):
        if n % GOP == 0:
            out.append(start + idr(n // GOP))
        else:
            out.append(start + p_skip(n % GOP))
    return b"".join(out)


def vint(data, pos, keep_marker=False):
    first = data[pos]
    length = 1
    while length <= 8 and not first & (0x80 >> (length - 1)):
        length += 1
    assert length <= 8, ("bad EBML number", pos)
    value = first if keep_marker else first & (0xFF >> length)
    for b in data[pos + 1:pos + length]:
        value = value << 8 | b
    return value, pos + length


def elements(data, start, end):
    """(id, element start, data start, element end) of the children in [start, end)."""
    pos = start
    while pos < end:
        eid, p = vint(data, pos, keep_marker=True)
        size, p = vint(data, p)
        yield eid, pos, p, p + size
        pos = p + size


def audio_block_first(path, video_track=1, audio_track=2):
    """Move the first audio SimpleBlock of the first cluster in front of the
    first video SimpleBlock (sizes and positions of the clusters stay)."""
    data = bytearray(path.read_bytes())
    seg = next(e for e in elements(data, 0, len(data)) if e[0] == SEGMENT)
    cluster = next(e for e in elements(data, seg[2], seg[3]) if e[0] == CLUSTER)
    children = list(elements(data, cluster[2], cluster[3]))

    def track(child):
        return vint(data, child[2])[0] if child[0] == SIMPLE_BLOCK else None

    vi = next(i for i, c in enumerate(children) if track(c) == video_track)
    ai = next(i for i, c in enumerate(children) if track(c) == audio_track and i > vi)
    blocks = [bytes(data[c[1]:c[3]]) for c in children]
    blocks.insert(vi, blocks.pop(ai))
    payload = b"".join(blocks)
    assert len(payload) == cluster[3] - cluster[2]
    data[cluster[2]:cluster[3]] = payload
    path.write_bytes(bytes(data))


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def pes_packets(path):
    data = path.read_bytes() if path.exists() else b""
    pos, out = 0, []
    while pos + 9 <= len(data):
        assert data[pos:pos + 3] == b"\x00\x00\x01", (pos, data[pos:pos + 16])
        size = 6 + int.from_bytes(data[pos + 4:pos + 6], "big")
        if size == 6:
            # PES_packet_length 0 (video of any size): up to the next video
            # PES header, which valid H.264 data cannot contain (a NAL unit
            # header 0xE0 has the forbidden bit set)
            end = data.find(b"\x00\x00\x01\xe0", pos + 9)
            size = (end if end >= 0 else len(data)) - pos
        pts = None
        if data[pos + 7] & 0x80:
            p = data[pos + 9:pos + 14]
            pts = ((p[0] & 14) << 29 | p[1] << 22 | (p[2] & 254) << 14 | p[3] << 7 | p[4] >> 1)
        out.append((pts, data[pos + 9 + data[pos + 8]:pos + size]))
        pos += size
    return out


def slice_types(payload):
    """NAL unit types of the coded slices in an Annex B payload (5 IDR, 1 non-IDR)."""
    types, pos = [], 0
    while True:
        pos = payload.find(b"\x00\x00\x01", pos)
        if pos < 0 or pos + 3 >= len(payload):
            return types
        nal_type = payload[pos + 3] & 0x1F
        if nal_type in (1, 5):
            types.append(nal_type)
        pos += 3


def play(player, uri, sink, seconds=4):
    if sink.exists():
        sink.unlink()
    proc = subprocess.Popen([player, uri], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        time.sleep(seconds)
        try:
            proc.stdin.write(b"q\n")
            proc.stdin.flush()
        except BrokenPipeError:
            pass  # ended by itself; its output and exit code tell why
        out = proc.communicate(timeout=20)[0].decode("utf-8", "replace")
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(10)
    packets = pes_packets(sink)
    sink.unlink(missing_ok=True)
    return proc.returncode, out, packets


def check(name, rc, out, packets):
    assert rc == 0, (name, rc, out[-3000:])
    pictures = [(pts, slice_types(payload)) for pts, payload in packets]
    pictures = [(pts, t) for pts, t in pictures if t]
    assert len(pictures) >= 25, (name, len(pictures), out[-3000:])
    first_pts, first_types = pictures[0]
    assert first_types[0] == 5, (name, "first picture written is not the IDR picture",
                                 [(pts, t) for pts, t in pictures[:4]], out[-3000:])
    print(f"PASS {name}: first picture written is the IDR picture (PTS {first_pts}), "
          f"{len(pictures)} pictures")


def run(args, sink):
    with tempfile.TemporaryDirectory(prefix="first-key-") as directory:
        root = Path(directory)
        raw = root / "video.h264"
        raw.write_bytes(h264_stream())
        media = root / "early_key.mkv"
        subprocess.run([args.ffmpeg, "-v", "error", "-y",
                        "-f", "h264", "-framerate", "25", "-i", str(raw),
                        "-itsoffset", "0.002", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                        "-map", "0:v", "-map", "1:a", "-t", "8",
                        "-c:v", "copy", "-c:a", "pcm_s16le", "-ac", "2", str(media)],
                       check=True, timeout=60)
        audio_block_first(media)

        handler = functools.partial(QuietHandler, directory=directory)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/{media.name}"
            check("HTTP, start time from the audio block", *play(args.player, url, sink))
            check("local file", *play(args.player, str(media), sink))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--video-output", required=True)
    parser.add_argument("--audio-output", help="HOST_AUDIO_OUTPUT of PLAYER, removed as well")
    args = parser.parse_args()
    sink = Path(args.video_output)
    audio_sink = Path(args.audio_output) if args.audio_output else None
    try:
        run(args, sink)
    finally:
        sink.unlink(missing_ok=True)
        if audio_sink:
            audio_sink.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
