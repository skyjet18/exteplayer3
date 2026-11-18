"""MP4 files with the avcC of the replay clips that showed no picture on the boxes.

The clips had SPS and PPS twice in the avcC and a zero byte at the end of
one PPS, and no parameter sets in the samples. The H.264 stream here is
written bit by bit: 64x64 Main profile, every frame an IDR picture of
I_PCM macroblocks, so any decoder shows it without an encoder in the
FFmpeg build. ffmpeg puts it into MP4; the avcC is then rewritten and the
parameter sets in the samples become filler NAL units of the same size.

    python3 tests/h264_avcc_mp4.py FFMPEG DIRECTORY

writes DIRECTORY/avcc-repeated.mp4 and DIRECTORY/avcc-stray.mp4.
"""

import struct
import subprocess
import sys
from pathlib import Path

FRAMES = 10


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        self.bits += [(value >> i) & 1 for i in range(count - 1, -1, -1)]

    def ue(self, value):
        code = value + 1
        self.put(0, code.bit_length() - 1)
        self.put(code, code.bit_length())

    def align(self):
        self.put(0, -len(self.bits) % 8)

    def nal(self, header):
        """NAL unit (without start code) with trailing bits and emulation prevention."""
        self.put(1, 1)
        self.align()
        raw = bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))
        out, zeros = bytearray([header]), 0
        for byte in raw:
            if zeros >= 2 and byte <= 3:
                out.append(3)
                zeros = 0
            out.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        return bytes(out)


def sps():
    b = Bits()
    b.put(77, 8)    # Main profile
    b.put(0x40, 8)  # constraint_set1_flag
    b.put(31, 8)    # level 3.1
    b.ue(0)         # seq_parameter_set_id
    b.ue(0)         # log2_max_frame_num_minus4
    b.ue(2)         # pic_order_cnt_type
    b.ue(1)         # max_num_ref_frames
    b.put(0, 1)     # gaps_in_frame_num_value_allowed_flag
    b.ue(3)         # pic_width_in_mbs_minus1
    b.ue(3)         # pic_height_in_map_units_minus1
    b.put(1, 1)     # frame_mbs_only_flag
    b.put(1, 1)     # direct_8x8_inference_flag
    b.put(0, 1)     # frame_cropping_flag
    b.put(0, 1)     # vui_parameters_present_flag
    return b.nal(0x67)


def pps(pps_id):
    b = Bits()
    b.ue(pps_id)
    b.ue(0)         # seq_parameter_set_id
    b.put(0, 1)     # CAVLC
    b.put(0, 1)     # bottom_field_pic_order_in_frame_present_flag
    b.ue(0)         # num_slice_groups_minus1
    b.ue(0)         # num_ref_idx_l0_default_active_minus1
    b.ue(0)         # num_ref_idx_l1_default_active_minus1
    b.put(0, 1)     # weighted_pred_flag
    b.put(0, 2)     # weighted_bipred_idc
    b.ue(0)         # pic_init_qp_minus26 (se 0)
    b.ue(0)         # pic_init_qs_minus26 (se 0)
    b.ue(0)         # chroma_qp_index_offset (se 0)
    b.put(1, 1)     # deblocking_filter_control_present_flag
    b.put(0, 1)     # constrained_intra_pred_flag
    b.put(0, 1)     # redundant_pic_cnt_present_flag
    return b.nal(0x68)


def idr(number):
    b = Bits()
    b.ue(0)              # first_mb_in_slice
    b.ue(7)              # slice_type: I, all slices
    b.ue(0)              # pic_parameter_set_id
    b.put(0, 4)          # frame_num
    b.ue(number % 2)     # idr_pic_id, different in consecutive IDR pictures
    b.put(0, 1)          # no_output_of_prior_pics_flag
    b.put(0, 1)          # long_term_reference_flag
    b.ue(0)              # slice_qp_delta (se 0)
    b.ue(1)              # disable_deblocking_filter_idc
    for mb in range(16):
        b.ue(25)         # mb_type I_PCM
        b.align()        # pcm_alignment_zero_bit
        luma = 16 + (number * 20 + mb * 12) % 200
        for _ in range(256):
            b.put(luma, 8)
        for _ in range(128):
            b.put(128, 8)
    return b.nal(0x65)


def boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if size < 8:
            raise ValueError("box size %d" % size)
        yield kind, pos, size
        pos += size


def find(data, path, start=0, end=None):
    """Offsets of the boxes along "path"; stsd and avc1 have fields before their children."""
    end = len(data) if end is None else end
    chain = []
    for name in path:
        for kind, pos, size in boxes(data, start, end):
            if kind == name:
                chain.append(pos)
                start, end = pos + 8, pos + size
                if name == b"stsd":
                    start += 8
                elif name == b"avc1":
                    start += 78
                break
        else:
            raise ValueError("no %s box" % name)
    return chain


def rewrite(mp4, avcc_sets, sps_nal):
    """New avcC body, ancestor sizes adjusted; SPS/PPS in the samples made filler NAL units."""
    data = bytearray(mp4)
    top = [kind for kind, _, _ in boxes(data, 0, len(data))]
    # the moov behind the mdat: a larger moov moves no sample
    assert top.index(b"moov") > top.index(b"mdat"), top
    chain = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsd", b"avc1", b"avcC"])
    avcc = chain[-1]
    size = struct.unpack(">I", data[avcc:avcc + 4])[0]
    old = bytes(data[avcc + 8:avcc + size])
    assert old[0] == 1 and old[8:8 + len(sps_nal)] == sps_nal, old.hex()
    body = old[:5] + avcc_sets
    delta = len(body) - len(old)
    data[avcc + 8:avcc + size] = body
    for pos in chain:
        data[pos:pos + 4] = struct.pack(">I", struct.unpack(">I", data[pos:pos + 4])[0] + delta)
    # samples: 4 byte lengths, the mdat holds them one after the other
    for kind, pos, size in boxes(data, 0, len(data)):
        if kind == b"mdat":
            p, end = pos + 8, pos + size
            while p + 4 <= end:
                length = struct.unpack(">I", data[p:p + 4])[0]
                if data[p + 4] & 0x1f in (7, 8):
                    data[p + 4:p + 4 + length] = bytes([0x0c]) + b"\xff" * (length - 2) + b"\x80"
                p += 4 + length
    return bytes(data)


def write(ffmpeg, directory):
    directory = Path(directory)
    s, p0, p1 = sps(), pps(0), pps(1)
    raw = directory / "avcc.h264"
    start = b"\0\0\0\1"
    raw.write_bytes(start + s + start + p0 + b"".join(start + idr(n) for n in range(FRAMES)))
    plain = directory / "avcc-plain.mp4"
    subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "h264", "-framerate", "25", "-i", str(raw),
                    "-c", "copy", "-f", "mp4", str(plain)], check=True, timeout=60)
    mp4 = plain.read_bytes()

    def entry(nal):
        return struct.pack(">H", len(nal)) + nal

    variants = {
        # SPS and PPS twice, the second PPS with a zero byte counted into its length
        "avcc-repeated.mp4": bytes([0xe2]) + entry(s) + entry(s) + bytes([2]) + entry(p0) + entry(p0 + b"\0"),
        # a zero byte between the two PPS that no length counts
        "avcc-stray.mp4": bytes([0xe1]) + entry(s) + bytes([2]) + entry(p0) + b"\0" + entry(p1),
    }
    paths = []
    for name, sets in variants.items():
        target = directory / name
        target.write_bytes(rewrite(mp4, sets, s))
        paths.append(target)
    return paths


if __name__ == "__main__":
    for path in write(sys.argv[1], sys.argv[2]):
        print(path)
