"""--cover <path>: the embedded cover of audio files written as it is, one "cover" message.

Media made with the ffmpeg tool of the FFmpeg build: an MP3 (MPEG audio with
an ID3v2 APIC frame), FLAC with a PNG picture block, M4A with covr, Ogg Vorbis
with METADATA_BLOCK_PICTURE, Matroska with a cover attachment (image mimetype
and, as fallback, application/octet-stream), files without a cover and one
with a cover larger than the limit. Every file plays the same with and
without --cover."""

import argparse
import base64
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import zlib

LIMIT = 8 * 1024 * 1024
# messages that tell what the player made of the file, no timing in them
SAME = ("EPLAYER3_EXTENDED", "PLAYBACK_OPEN", "OUTPUT_OPEN", "PLAYBACK_PLAY", "v_c", "a_l", "a_c", "s_l", "s_c")


def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}):\n{result.stderr.decode()}")
    return result


def play(player, *args):
    """Plays a file to its end: the JSON messages, every one parsed."""
    process = subprocess.Popen([player, *args], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: [lines.put(line) for line in process.stderr], daemon=True)
    reader.start()
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise
    reader.join(timeout=5)
    process.stdin.close()
    process.stderr.close()
    messages = []
    while not lines.empty():
        messages.append(json.loads(lines.get()))
    assert process.returncode == 0, (args, process.returncode, messages)
    assert any("PLAYBACK_PLAY" in m and m["PLAYBACK_PLAY"]["sts"] == 0 for m in messages), (args, messages)
    return messages


def covers(messages):
    return [m["cover"] for m in messages if "cover" in m]


def same(messages):
    return [m for m in messages if next(iter(m)) in SAME]


def png(width, height, pixels, level=9):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level)) + chunk(b"IEND", b""))


def id3_apic(picture, mime):
    """ID3v2.3 tag with one APIC frame, picture type 3 (front cover)."""
    body = b"\0" + mime.encode() + b"\0" + b"\x03" + b"\0" + picture
    frame = b"APIC" + struct.pack(">I", len(body)) + b"\0\0" + body
    size = len(frame)
    syncsafe = bytes([(size >> 21) & 127, (size >> 14) & 127, (size >> 7) & 127, size & 127])
    return b"ID3\x03\x00\x00" + syncsafe + frame


def flac_picture(picture, mime, width, height):
    """FLAC METADATA_BLOCK_PICTURE, as Vorbis comments carry it (base64)."""
    return (struct.pack(">I", 3) + struct.pack(">I", len(mime)) + mime.encode() + struct.pack(">I", 0)
            + struct.pack(">IIII", width, height, 24, 0) + struct.pack(">I", len(picture)) + picture)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", required=True)
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()
    ff = args.ffmpeg
    with tempfile.TemporaryDirectory(prefix="exteplayer3-cover-") as directory:
        root = Path(directory)
        sine = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=2"]
        jpeg = root / "cover.jpg"
        run(ff, "-y", "-f", "lavfi", "-i", "testsrc=size=32x24:rate=1", "-frames:v", "1",
            "-pix_fmt", "yuv420p", "-color_range", "pc", "-strict", "-1", "-c:v", "mjpeg", "-f", "image2", str(jpeg))
        jpeg_bytes = jpeg.read_bytes()
        assert jpeg_bytes[:3] == b"\xff\xd8\xff", jpeg_bytes[:4]
        png_bytes = png(40, 30, bytes((x * 7 + y * 3) % 256 for y in range(30) for x in range(40 * 3)))
        (root / "cover.png").write_bytes(png_bytes)

        media = []  # (name, file, expected picture, type, w, h)
        # MP3: MPEG audio (layer 2: FFmpeg has no MP3 encoder of its own)
        # behind an ID3v2 tag with an APIC frame, demuxed by the mp3 demuxer
        mp2 = root / "audio.mp2"
        run(ff, "-y", *sine, "-c:a", "mp2", "-b:a", "128k", "-f", "mp2", str(mp2))
        mp3 = root / "id3.mp3"
        mp3.write_bytes(id3_apic(jpeg_bytes, "image/jpeg") + mp2.read_bytes())
        media.append(("MP3 ID3 APIC JPEG", mp3, jpeg_bytes, "jpeg", 32, 24))
        flac = root / "picture.flac"
        run(ff, "-y", *sine, "-i", str(root / "cover.png"), "-map", "0:a", "-map", "1:v",
            "-c:a", "flac", "-c:v", "copy", "-disposition:v", "attached_pic",
            "-metadata:s:v", "comment=Cover (front)", str(flac))
        media.append(("FLAC picture PNG", flac, png_bytes, "png", 40, 30))
        m4a = root / "covr.m4a"
        run(ff, "-y", *sine, "-i", str(jpeg), "-map", "0:a", "-map", "1:v",
            "-c:a", "aac", "-b:a", "64k", "-c:v", "copy", "-disposition:v", "attached_pic", str(m4a))
        media.append(("M4A covr JPEG", m4a, jpeg_bytes, "jpeg", 32, 24))
        ogg = root / "picture.ogg"
        block = base64.b64encode(flac_picture(png_bytes, "image/png", 40, 30)).decode()
        run(ff, "-y", *sine, "-c:a", "vorbis", "-strict", "-2", "-ac", "2",
            "-metadata", "METADATA_BLOCK_PICTURE=" + block, str(ogg))
        media.append(("Ogg Vorbis METADATA_BLOCK_PICTURE PNG", ogg, png_bytes, "png", 40, 30))
        mkv = root / "attachment.mka"
        run(ff, "-y", *sine, "-c:a", "flac", "-attach", str(jpeg),
            "-metadata:s:t", "mimetype=image/jpeg", "-metadata:s:t", "filename=cover.jpg", str(mkv))
        media.append(("Matroska attachment image/jpeg", mkv, jpeg_bytes, "jpeg", 32, 24))
        mkv_other = root / "attachment-octet.mka"
        run(ff, "-y", *sine, "-c:a", "flac", "-attach", str(root / "cover.png"),
            "-metadata:s:t", "mimetype=application/octet-stream", "-metadata:s:t", "filename=cover.png",
            str(mkv_other))
        media.append(("Matroska attachment cover.png, application/octet-stream", mkv_other, png_bytes, "png", 0, 0))
        # without a cover: a picture attachment not named cover is none
        plain = root / "plain.m4a"
        run(ff, "-y", *sine, "-c:a", "aac", "-b:a", "64k", str(plain))
        media.append(("M4A without cover", plain, None, None, 0, 0))
        other = root / "other-attachment.mka"
        run(ff, "-y", *sine, "-c:a", "flac", "-attach", str(root / "cover.png"),
            "-metadata:s:t", "mimetype=application/octet-stream", "-metadata:s:t", "filename=booklet.png",
            str(other))
        media.append(("Matroska attachment booklet.png, no cover", other, None, None, 0, 0))
        # larger than the limit: not written; stored PNG of noise, 1800x1600
        big_png = png(1800, 1600, os.urandom(1800 * 1600 * 3), level=0)
        assert len(big_png) > LIMIT
        (root / "big.png").write_bytes(big_png)
        big = root / "big.flac"
        run(ff, "-y", *sine, "-i", str(root / "big.png"), "-map", "0:a", "-map", "1:v",
            "-c:a", "flac", "-c:v", "copy", "-disposition:v", "attached_pic", str(big))
        media.append(("FLAC picture over 8 MiB", big, None, None, 0, 0))

        out = root / "out"
        out.mkdir()
        target = out / 'co"ver\\.img'
        for name, path, picture, kind, width, height in media:
            before = sorted(os.listdir(out))
            plain_messages = play(args.player, str(path))
            assert not covers(plain_messages), (name, plain_messages)
            assert sorted(os.listdir(out)) == before, (name, before, os.listdir(out))
            extended = next(m["EPLAYER3_EXTENDED"] for m in plain_messages if "EPLAYER3_EXTENDED" in m)
            assert extended.get("cover") == 1, extended

            # an old cover at the path: replaced, or removed without one
            target.write_bytes(b"old")
            messages = play(args.player, "--cover", str(target), str(path))
            found = covers(messages)
            assert len(found) == 1, (name, messages)
            cover = found[0]
            if picture is None:
                assert cover == {}, (name, cover)
                assert not target.exists(), name
            else:
                assert cover == {"file": str(target), "type": kind, "w": width, "h": height,
                                 "size": len(picture)}, (name, cover)
                assert target.read_bytes() == picture, name
            assert sorted(os.listdir(out)) == ([target.name] if picture else []), (name, os.listdir(out))
            # the cover message before PLAYBACK_OPEN, the rest as without --cover
            keys = [next(iter(m)) for m in messages]
            assert keys.index("cover") < keys.index("PLAYBACK_OPEN"), keys
            assert same(messages) == same(plain_messages), (name, same(plain_messages), same(messages))
            assert not any(m.get("v_c", {}).get("id", -1) >= 0 for m in messages), (name, messages)
            if target.exists():
                target.unlink()
            print(f"PASS --cover {name}: {json.dumps(cover)}, playback unchanged")

        # an empty path is refused
        result = subprocess.run([args.player, "--cover", "", str(mp3)], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        assert result.returncode != 0 and b"Invalid --cover value" in result.stdout, result
        print("PASS --cover with an empty path is refused")


if __name__ == "__main__":
    main()
