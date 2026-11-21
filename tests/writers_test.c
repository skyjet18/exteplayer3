#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include "common.h"
#include "debug.h"
#include "aac.h"
#include "misc.h"
#include "pcm.h"
#include "pes.h"
#include "writer.h"

#define PARTS 1100

static struct iovec parts[PARTS];
static int part_count, calls;
static uint8_t written[512];
static size_t written_size;
static uint64_t pcm_pts[2];
static uint8_t xiph[45] = {2, 30, 5};
static uint8_t vorbis_frame[] = {1, 2, 3, 4};

/* Keeps the vectors only: a broken writer passes lengths beyond its packet. */
static ssize_t record(int fd, const struct iovec *iov, int count)
{
    ssize_t size = 0;
    int i;
    (void)fd;
    for (i = 0; i < count; i++)
    {
        if (part_count < PARTS)
            parts[part_count] = iov[i];
        part_count++;
        size += iov[i].iov_len;
    }
    calls++;
    return size;
}

static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    int i;
    for (i = 0; i < count; i++)
    {
        assert(written_size + iov[i].iov_len <= sizeof(written));
        memcpy(written + written_size, iov[i].iov_base, iov[i].iov_len);
        written_size += iov[i].iov_len;
    }
    return record(fd, iov, count);
}

static uint64_t read_pts(const uint8_t *p)
{
    assert((p[0] & 1) && (p[2] & 1) && (p[4] & 1));
    return ((uint64_t)(p[0] & 14) << 29) | ((uint64_t)p[1] << 22) |
           ((uint64_t)(p[2] & 254) << 14) | (p[3] << 7) | (p[4] >> 1);
}

static ssize_t pcm_packet(int fd, const struct iovec *iov, int count)
{
    assert(count == 2 && calls < 2);
    pcm_pts[calls] = read_pts((const uint8_t *)iov[0].iov_base + 9);
    return record(fd, iov, count);
}

static ssize_t vorbis_packet(int fd, const struct iovec *iov, int count)
{
    static const size_t sizes[] = {30, 5, 7};
    const uint8_t *header = xiph + 3;
    int i;
    assert(count == 8);
    for (i = 0; i < 3; i++)
    {
        const struct iovec *length = iov + 1 + 2 * i;
        assert(length[0].iov_len == 2 && !memcmp(length[0].iov_base, (uint8_t[]){0, sizes[i]}, 2));
        assert(length[1].iov_base == header && length[1].iov_len == sizes[i]);
        header += sizes[i];
    }
    assert(iov[7].iov_base == vorbis_frame && iov[7].iov_len == sizeof(vorbis_frame));
    return record(fd, iov, count);
}

/* A PES header with a timestamp takes 14 bytes. */
static WriterAVCallData_t packet(uint8_t *data, unsigned int size, void *private_data,
                                 unsigned int private_size, WriteV_t write)
{
    WriterAVCallData_t call = {.fd = 1, .data = data, .len = size, .Pts = 90000, .Dts = 90000,
                               .private_data = private_data, .private_size = private_size,
                               .WriteV = write};
    part_count = calls = 0;
    written_size = 0;
    return call;
}

/* From "first" on, every NAL is a start code and bytes of the packet. */
static void assert_nals(const uint8_t *data, size_t size, int first)
{
    int i;
    assert(part_count <= PARTS && (part_count - first) % 2 == 0);
    for (i = first; i < part_count; i += 2)
    {
        const uint8_t *nal = parts[i + 1].iov_base;
        size_t length = parts[i + 1].iov_len;
        assert(parts[i].iov_len == 4 && !memcmp(parts[i].iov_base, "\0\0\0\1", 4));
        assert(nal >= data && length <= size && (size_t)(nal - data) <= size - length);
    }
}

static void bad_nal_lengths(Writer_t *writer, uint8_t *extra, unsigned int extra_size)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    uint8_t beyond[] = {255, 255, 255, 252, 0x26, 1, 0, 0};
    uint8_t *cut = calloc(1, 2);
    WriterAVCallData_t call = packet(frame, sizeof(frame), extra, extra_size, record);
    assert(cut);
    writer->reset();
    assert(writer->writeData(&call) > 14 + 4 + 2 && part_count == 4);
    assert_nals(frame, sizeof(frame), 2);
    /* The length field says 4 GB: the NAL ends with the packet. */
    call = packet(beyond, sizeof(beyond), extra, extra_size, record);
    assert(writer->writeData(&call) == 14 + 4 + 4 && part_count == 3);
    assert_nals(beyond, sizeof(beyond), 1);
    /* Two bytes cannot hold a four byte length field. */
    call = packet(cut, 2, extra, extra_size, record);
    assert(writer->writeData(&call) == 14 && part_count == 1);
    writer->reset();
    free(cut);
}

static void h264_nal_lengths(void)
{
    uint8_t avcc[] = {1, 100, 0, 42, 255, 225, 0, 4, 103, 100, 0, 42, 1, 0, 2, 104, 1};
    bad_nal_lengths(&WriterVideoH264, avcc, sizeof(avcc));
}

static void h264_parameter_sets_in_a_later_packet(void)
{
    uint8_t avcc[] = {1, 100, 0, 42, 255, 224, 0}; /* No SPS, no PPS. */
    uint8_t slice[] = {0, 0, 0, 2, 97, 136};
    uint8_t key[] = {0, 0, 0, 4, 103, 100, 0, 42, 0, 0, 0, 2, 104, 1, 0, 0, 0, 2, 101, 136};
    WriterAVCallData_t call = packet(slice, sizeof(slice), avcc, sizeof(avcc), record);
    WriterVideoH264.reset();
    assert(!WriterVideoH264.writeData(&call) && !part_count);
    call = packet(key, sizeof(key), avcc, sizeof(avcc), record);
    assert(WriterVideoH264.writeData(&call) == 14 + 3 * 4 + 8 && part_count == 7);
    assert_nals(key, sizeof(key), 1);
    WriterVideoH264.reset();
}

/* hvcC with one SPS of two bytes; byte 21 holds the NAL length size - 1. */
static uint8_t hvcc[30] = {[0] = 1, [21] = 3, [22] = 1, [23] = 0x21, [25] = 1, [27] = 2,
                           [28] = 0x42, [29] = 1};

static void h265_nal_lengths(void)
{
    bad_nal_lengths(&WriterVideoH265, hvcc, sizeof(hvcc));
}

static void h265_iovec_limit(void)
{
    static uint8_t nals[1200];
    uint8_t extra[sizeof(hvcc)];
    WriterAVCallData_t call = packet(nals, 2, extra, sizeof(extra), record);
    size_t i;
    memcpy(extra, hvcc, sizeof(hvcc));
    extra[21] = 0;
    for (i = 0; i < sizeof(nals); i += 2)
    {
        nals[i] = 1;
        nals[i + 1] = 0x26;
    }
    WriterVideoH265.reset();
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 5 && part_count == 4);
    /* 600 NALs do not fit into the vector: nothing is written rather than
     * the first 511 of them as if that were the access unit. */
    call = packet(nals, sizeof(nals), extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == -1 && !calls);
    /* 511 NALs fit behind the header */
    call = packet(nals, 511 * 2, extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 511 * 5 && part_count == 1023);
    assert_nals(nals, 511 * 2, 1);
    WriterVideoH265.reset();
}

static void h265_extradata_00_xx_01_is_no_start_code(void)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    uint8_t extra[sizeof(hvcc)];
    WriterAVCallData_t call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    memcpy(extra, hvcc, sizeof(hvcc));
    extra[0] = 0;
    extra[1] = 2;
    extra[2] = 1;
    call.InfoFlags = 1;
    WriterVideoH265.reset();
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    assert(parts[1].iov_len == 6 && !memcmp(parts[1].iov_base, "\0\0\0\1\x42\1", 6));
    WriterVideoH265.reset();
}

static void h265_four_byte_start_code_in_extradata(void)
{
    uint8_t frame[] = {0, 0, 0, 1, 0x26, 1};
    uint8_t extra[] = {0, 0, 0, 1, 0x42, 1};
    WriterAVCallData_t call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    call.InfoFlags = 1;
    WriterVideoH265.reset();
    /* Annex B in, Annex B out: header with fake start code, one byte, the packet. */
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 6 && part_count == 3);
    assert(parts[2].iov_base == frame && parts[2].iov_len == sizeof(frame));
    WriterVideoH265.reset();
}

/* Other parameter sets in the extradata without a reset (another
 * representation of an adaptive stream): the next packet carries them. */
static void h265_new_parameter_sets(void)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    uint8_t extra[sizeof(hvcc)];
    WriterAVCallData_t call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), record);
    memcpy(extra, hvcc, sizeof(hvcc));
    WriterVideoH265.reset();
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    assert(parts[1].iov_len == 6 && !memcmp(parts[1].iov_base, "\0\0\0\1\x42\1", 6));
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    /* the same sets at another address: nothing again */
    call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    extra[29] = 2;
    call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    assert(parts[1].iov_len == 6 && !memcmp(parts[1].iov_base, "\0\0\0\1\x42\2", 6));
    call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    WriterVideoH265.reset();
}

/* Packets without extradata, or with a size but no buffer, neither crash
 * nor count as new parameter sets; extradata coming later is sent. */
static void h265_parameter_sets_without_extradata(void)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    uint8_t annexb[] = {0, 0, 0, 1, 0x26, 1};
    WriterAVCallData_t call = packet(frame, sizeof(frame), NULL, 0, record);
    WriterVideoH265.reset();
    /* nothing to start the decoder with yet: the frame is not written */
    assert(WriterVideoH265.writeData(&call) == 0 && !calls);
    call = packet(frame, sizeof(frame), NULL, sizeof(hvcc), record);
    assert(WriterVideoH265.writeData(&call) == 0 && !calls);
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    assert(parts[1].iov_len == 6 && !memcmp(parts[1].iov_base, "\0\0\0\1\x42\1", 6));
    call = packet(frame, sizeof(frame), NULL, sizeof(hvcc), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    call = packet(frame, sizeof(frame), NULL, 0, record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    /* the same sets after packets without them: not sent again */
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    /* Annex B packet with a size but no buffer: injected as it is */
    call = packet(annexb, sizeof(annexb), NULL, 6, record);
    call.InfoFlags = 1;
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 6 && part_count == 3);
    assert(parts[2].iov_base == annexb && parts[2].iov_len == sizeof(annexb));
    WriterVideoH265.reset();
}

/* Annex B outside TS (raw .hevc, RTSP, AVI): InfoFlags 0. The sets of the
 * extradata go once in front of a packet without its own, then the packets
 * as they are. */
static void h265_annexb_without_ts_flag(void)
{
    uint8_t sets[] = {0, 0, 0, 1, 0x40, 1, 0, 0, 0, 1, 0x42, 1};
    uint8_t idr[] = {0, 0, 0, 1, 0x26, 1};
    uint8_t key[] = {0, 0, 0, 1, 0x40, 1, 0, 0, 1, 0x26, 1};
    uint8_t short_code[] = {0, 0, 1, 0x26, 1};
    uint8_t lengths[] = {0, 0, 0, 2, 0x26, 1};
    WriterAVCallData_t call = packet(idr, sizeof(idr), sets, sizeof(sets), record);
    WriterVideoH265.reset();
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 12 + 6 && part_count == 4);
    assert(parts[2].iov_base == sets && parts[2].iov_len == sizeof(sets));
    assert(parts[3].iov_base == idr && parts[3].iov_len == sizeof(idr));
    call = packet(idr, sizeof(idr), sets, sizeof(sets), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 6 && part_count == 3);
    assert(parts[2].iov_base == idr);
    /* after a seek, a packet with its own VPS: the extradata is not sent */
    WriterVideoH265.reset();
    call = packet(key, sizeof(key), sets, sizeof(sets), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 11 && part_count == 3);
    assert(parts[2].iov_base == key);
    /* no extradata at all: the start code of the packet tells Annex B */
    WriterVideoH265.reset();
    call = packet(lengths, sizeof(lengths), NULL, 0, record);
    assert(WriterVideoH265.writeData(&call) == 0 && !calls);
    call = packet(short_code, sizeof(short_code), NULL, 0, record);
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 5 && part_count == 3);
    assert(parts[2].iov_base == short_code && parts[2].iov_len == sizeof(short_code));
    call = packet(idr, sizeof(idr), NULL, 0, record);
    assert(WriterVideoH265.writeData(&call) == 14 + 5 + 1 + 6 && part_count == 3);
    WriterVideoH265.reset();
}

static void mpeg2_packet_shorter_than_a_start_code(void)
{
    uint8_t sequence[] = {0, 0, 1, 0xb3, 1, 2, 3, 4};
    uint8_t group[] = {0, 0, 1, 0xb8, 0, 8, 0, 0};
    WriterAVCallData_t call = packet(group, 3, sequence, sizeof(sequence), capture);
    WriterVideoMPEG2.reset();
    /* The fourth byte is not part of the packet: no group start code yet. */
    assert(WriterVideoMPEG2.writeData(&call) == 1 && calls == 2 && written_size == 14 + 3);
    call = packet(group, sizeof(group), sequence, sizeof(sequence), capture);
    assert(WriterVideoMPEG2.writeData(&call) == 14 + 8 + 8 && calls == 1);
    assert(!memcmp(written + 14, sequence, 8) && !memcmp(written + 22, group, 8));
}

/* Sequence headers after a seek: the one of the packet applies; the kept
 * one, the latest, goes only in front of a GOP that comes without one. */
static void mpeg2_sequence_header_after_a_seek(void)
{
    /* sequence header (no matrices) and GOP header: 12 + 8 bytes */
    uint8_t a[] = {0, 0, 1, 0xb3, 0x2d, 0x02, 0x40, 0x33, 0xff, 0xff, 0xe0, 0x18,
                   0, 0, 1, 0xb8, 0, 8, 0, 0};
    uint8_t b[sizeof(a)];
    uint8_t group[] = {0, 0, 1, 0xb8, 0, 8, 0, 0};
    WriterAVCallData_t call = packet(a, sizeof(a), NULL, 0, capture);
    memcpy(b, a, sizeof(a));
    b[4] = 0x16; /* another width */
    WriterVideoMPEG2.reset();
    assert(WriterVideoMPEG2.writeData(&call) == 1 && written_size == 14 + sizeof(a));
    /* a seek, then a packet with another sequence header: written as it is */
    WriterVideoMPEG2.reset();
    call = packet(b, sizeof(b), NULL, 0, capture);
    assert(WriterVideoMPEG2.writeData(&call) == 1 && written_size == 14 + sizeof(b));
    assert(!memcmp(written + 14, b, sizeof(b)));
    /* a seek to a GOP without a sequence header: the latest one in front */
    WriterVideoMPEG2.reset();
    call = packet(group, sizeof(group), NULL, 0, capture);
    assert(WriterVideoMPEG2.writeData(&call) == 14 + 12 + 8 && written_size == 14 + 12 + 8);
    assert(!memcmp(written + 14, b, 12) && !memcmp(written + 26, group, 8));
    /* with codec data of the container: not behind the header of the packet */
    WriterVideoMPEG2.reset();
    call = packet(a, sizeof(a), b, 12, capture);
    assert(WriterVideoMPEG2.writeData(&call) == 1 && written_size == 14 + sizeof(a));
    assert(!memcmp(written + 14, a, sizeof(a)));
    WriterVideoMPEG2.reset();
}

/* The codec data block goes out with the first frame when the driver refuses it. */
static void codec_data_with_first_frame(Writer_t *writer, size_t size, size_t offset)
{
    static const uint8_t zero[8];
    uint8_t extra[] = {1, 2, 3, 4};
    uint8_t frame[] = {9, 8, 7, 6, 5};
    WriterAVCallData_t call = packet(frame, sizeof(frame), extra, sizeof(extra), capture);
    writer->reset();
    assert(writer->writeData(&call) == (int)(14 + 4 + size + 5) && part_count == 3);
    assert(parts[1].iov_len == size && !memcmp(written + 18, zero, 8));
    assert(!memcmp(written + 18 + offset, extra, 4) && !memcmp(written + 18 + size, frame, 5));
    writer->reset();
}

static void vc1_wmv_codec_data(void)
{
    codec_data_with_first_frame(&WriterVideoVC1, 8 + 4, 8);
    codec_data_with_first_frame(&WriterVideoWMV, 33, 22);
}

static void vorbis_headers(void)
{
    pcmPrivateData_t private = {.codec_id = AV_CODEC_ID_VORBIS, .channels = 2, .sample_rate = 48000,
                                .private_data = xiph, .private_size = sizeof(xiph)};
    WriterAVCallData_t call = packet(vorbis_frame, sizeof(vorbis_frame), &private, sizeof(private),
                                     vorbis_packet);
    size_t i;
    for (i = 3; i < sizeof(xiph); i++)
        xiph[i] = i;
    /* PES header, 22 bytes of stream description, three headers with their lengths. */
    assert(WriterAudioVORBIS.writeData(&call) == 14 + 22 + 3 * 2 + 42 + 4 && calls == 1);
}

static void vp9_frame(void)
{
    uint8_t frame[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    WriterAVCallData_t call = packet(frame, sizeof(frame), NULL, 0, capture);
    assert(WriterVideoVP9.writeData(&call) == 1);
    assert(calls == 3 && written_size == 24 + 10 + 184);
}

static void vp9_frame_without_pts(void)
{
    uint8_t frame[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    WriterAVCallData_t call = packet(frame, sizeof(frame), NULL, 0, capture);
    call.Pts = INVALID_PTS_VALUE;
    WriterVideoVP9.writeData(&call);
    /* One PES packet that holds what its length announces, then the end marker. */
    assert(calls == 3 && written_size == 19 + 10 + 184);
    assert(written[4] == 0 && written[5] == 19 - 6 + 10 && !memcmp(written + 19, frame, 10));
}

static void pcm_block_duration(void)
{
    static uint8_t samples[2 * 51840]; /* Two 30 ms blocks: 96 kHz, 6 channels, 24 bit. */
    pcmPrivateData_t private = {.codec_id = AV_CODEC_ID_PCM_S24LE, .channels = 6,
                                .sample_rate = 96000};
    WriterAVCallData_t call = packet(samples, sizeof(samples), &private, sizeof(private),
                                     pcm_packet);
    WriterAudioPCM.reset();
    assert(!WriterAudioPCM.writeData(&call) && calls == 2);
    assert(pcm_pts[0] == 90000 && pcm_pts[1] == 90000 + 2700);
    WriterAudioPCM.reset();
}

/* A packet of the LPCM writer, exactly as long as its samples (ASan); stereo
 * sample k holds the 16 bit values first + 2k and first + 2k + 1. */
static uint8_t *lpcm_samples(int samples, int first)
{
    uint8_t *data = malloc(samples * 4);
    int i;
    assert(data);
    for (i = 0; i < samples * 2; i++)
    {
        data[2 * i] = (first + i) & 255; /* little endian, as from the resampler */
        data[2 * i + 1] = (first + i) >> 8;
    }
    return data;
}

/* The values of the frame written last start with "first" (big endian). */
static void assert_lpcm_frame(int first)
{
    int i;
    /* PES header with timestamp, 0xa0, 6 bytes of the DVD header, 80 samples */
    assert(calls == 1 && written_size == 14 + 1 + 6 + 80 * 4 && written[14] == 0xa0);
    for (i = 0; i < 80 * 2; i++)
        assert((written[21 + 2 * i] << 8 | written[22 + 2 * i]) == first + i);
}

/* 48 kHz: 80 samples a frame. Samples kept from one packet go in front of
 * the next, also when that one does not fill a frame either. */
static void lpcm_rest_buffer(void)
{
    pcmPrivateData_t private = {.bResampling = 1, .channels = 2, .sample_rate = 48000};
    uint8_t *first = lpcm_samples(64, 0), *second = lpcm_samples(10, 128), *third = lpcm_samples(80, 148);
    uint8_t *after_seek = lpcm_samples(80, 1000);
    WriterAVCallData_t call = packet(first, 64 * 4, &private, sizeof(private), capture);
    WriterAudioLPCM.reset();
    assert(!WriterAudioLPCM.writeData(&call) && !calls);
    private.bResampling = 0;
    call = packet(second, 10 * 4, &private, sizeof(private), capture);
    assert(!WriterAudioLPCM.writeData(&call) && !calls);
    call = packet(third, 80 * 4, &private, sizeof(private), capture);
    assert(WriterAudioLPCM.writeData(&call) == 14 + 1 + 6 + 80 * 4);
    /* the 74 kept samples and 6 of this packet, in order */
    assert_lpcm_frame(0);
    /* the frame starts 74 samples before the packet */
    assert(read_pts(written + 9) == 90000 - 74 * 90000 / 48000);
    /* a seek drops the 74 samples kept now */
    WriterAudioLPCM.reset();
    call = packet(after_seek, 80 * 4, &private, sizeof(private), capture);
    assert(WriterAudioLPCM.writeData(&call) == 14 + 1 + 6 + 80 * 4);
    assert_lpcm_frame(1000);
    assert(read_pts(written + 9) == 90000);
    /* without a timestamp the frame has none either */
    call = packet(after_seek, 80 * 4, &private, sizeof(private), capture);
    call.Pts = INVALID_PTS_VALUE;
    assert(WriterAudioLPCM.writeData(&call) == 9 + 1 + 6 + 80 * 4 && calls == 1);
    assert(!(written[7] & 0x80) && written[9] == 0xa0);
    /* no channels: nothing written, no division by zero */
    private.channels = 0;
    call = packet(after_seek, 80 * 4, &private, sizeof(private), capture);
    assert(!WriterAudioLPCM.writeData(&call) && !calls);
    WriterAudioLPCM.reset();
    free(first);
    free(second);
    free(third);
    free(after_seek);
}

static void adts_frame_with_crc(void)
{
    uint8_t frame[] = {0xff, 0xf0, 0x50, 0x80, 0x01, 0x7f, 0xfc, 0, 0, 0, 0};
    uint8_t no_sync[] = {0xff, 0x01, 0x50, 0x80, 0x01, 0x7f, 0xfc, 0, 0, 0, 0};
    WriterAVCallData_t call = packet(frame, sizeof(frame), NULL, 0, capture);
    assert(HasADTSHeader(frame, sizeof(frame)) && !HasADTSHeader(no_sync, sizeof(no_sync)));
    WriterAudioAAC.reset();
    /* Written as it is, without a second ADTS header in front. */
    assert(WriterAudioAAC.writeData(&call) == 14 + 11 && !memcmp(written + 14, frame, 11));
}

static void aac_main_keeps_the_sample_rate(void)
{
    /* Profile 0 (Main), sampling frequency index 8 (16 kHz). */
    uint8_t frame[] = {0xff, 0xf1, 0x20, 0x80, 0x01, 0x7f, 0xfc, 0, 0, 0, 0};
    WriterAVCallData_t call = packet(frame, sizeof(frame), NULL, 0, capture);
    WriterAudioAAC.reset();
    assert(WriterAudioAAC.writeData(&call) == 14 + 11 && frame[2] == 0x60);
}

/* Output follow-up: reset() during a write, codec data built from the stream. */

static Writer_t *resetting;

/* The command thread resets the writer while the demuxer thread is in the write. */
static ssize_t reset_then_capture(int fd, const struct iovec *iov, int count)
{
    resetting->reset();
    return capture(fd, iov, count);
}

static void h264_reset_during_a_write(void)
{
    static const uint8_t sets[] = {0, 0, 0, 1, 103, 100, 0, 42, 0, 0, 0, 1, 104, 1};
    uint8_t avcc[] = {1, 100, 0, 42, 255, 225, 0, 4, 103, 100, 0, 42, 1, 0, 2, 104, 1};
    uint8_t frame[] = {0, 0, 0, 2, 101, 136};
    WriterAVCallData_t call = packet(frame, sizeof(frame), avcc, sizeof(avcc), reset_then_capture);
    resetting = &WriterVideoH264;
    WriterVideoH264.reset();
    /* The parameter sets in the iovec are still there when the write goes on. */
    assert(WriterVideoH264.writeData(&call) == 14 + 14 + 4 + 2 && part_count == 4);
    assert(!memcmp(written + 14, sets, sizeof(sets)));
    /* The reset is not lost: the next frame carries the parameter sets again. */
    call = packet(frame, sizeof(frame), avcc, sizeof(avcc), capture);
    assert(WriterVideoH264.writeData(&call) == 14 + 14 + 4 + 2 && part_count == 4);
    assert(!memcmp(written + 14, sets, sizeof(sets)));
    call = packet(frame, sizeof(frame), avcc, sizeof(avcc), capture);
    assert(WriterVideoH264.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    WriterVideoH264.reset();
}

static void aac_latm_reset_during_a_write(void)
{
    /* AAC LC, 44.1 kHz, stereo, and a frame of six bytes; padded as the demuxer delivers them. */
    uint8_t config[2 + AV_INPUT_BUFFER_PADDING_SIZE] = {0x12, 0x10};
    uint8_t frame[6 + AV_INPUT_BUFFER_PADDING_SIZE] = {0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c};
    WriterAVCallData_t call = packet(frame, 6, config, 2, reset_then_capture);
    size_t size;
    int result;
    resetting = &WriterAudioAACLATM;
    WriterAudioAACLATM.reset();
    /* PES header, LOAS header with the length, the frame from the LATM context. */
    result = WriterAudioAACLATM.writeData(&call);
    assert(part_count == 3 && parts[1].iov_len == 3);
    size = parts[2].iov_len;
    assert(result == (int)(14 + 3 + size) && written_size == 14 + 3 + size && size > 6);
    assert(written[14] == 0x56 && written[15] == (0xe0 | size >> 8) && written[16] == (size & 255));
    assert(!(written[17] & 0x80)); /* The stream configuration is in the frame. */
    /* The reset is not lost: the next frame carries the configuration again. */
    call = packet(frame, 6, config, 2, capture);
    assert(WriterAudioAACLATM.writeData(&call) == (int)(14 + 3 + size) && !(written[17] & 0x80));
    call = packet(frame, 6, config, 2, capture);
    assert(WriterAudioAACLATM.writeData(&call) < (int)(14 + 3 + size) && (written[17] & 0x80));
    WriterAudioAACLATM.reset();
}

/* The writer names the fields of the call in its error message. */
static void vorbis_without_call_data(void)
{
    BCMA_SILENT = 0;
    assert(!WriterAudioVORBIS.writeData(NULL));
    BCMA_SILENT = 1;
}

/* The avcC of the container has no parameter sets, the key frame has them. */
static void h264_sets_from_the_stream(int length_size, uint8_t *key, unsigned int size, int nals)
{
    uint8_t avcc[] = {1, 100, 0, 42, 0xfc | (length_size - 1), 224, 0};
    WriterAVCallData_t call = packet(key, size, avcc, sizeof(avcc), record);
    WriterVideoH264.reset();
    assert(WriterVideoH264.writeData(&call) == (int)(14 + nals * 4 + size - nals * length_size));
    assert(part_count == 1 + 2 * nals);
    assert_nals(key, size, 1);
    WriterVideoH264.reset();
}

static void h264_two_byte_nal_lengths(void)
{
    uint8_t key[] = {0, 4, 103, 100, 0, 42, 0, 2, 104, 1, 0, 2, 101, 136};
    h264_sets_from_the_stream(2, key, sizeof(key), 3);
}

static void h264_long_parameter_sets(void)
{
    static const uint8_t rest[] = {0, 0, 0, 2, 104, 1, 0, 0, 0, 2, 101, 136};
    uint8_t key[4 + 300 + sizeof(rest)] = {0, 0, 1, 44, 103, 100, 0, 42};
    /* A SPS of 252 bytes fills the old buffer up to its last two bytes. */
    uint8_t two[4 + 252 + 4 + 8 + sizeof(rest)] = {0, 0, 0, 252, 103, 100, 0, 42};
    memset(key + 8, 0x55, 296);
    memcpy(key + 304, rest, sizeof(rest));
    h264_sets_from_the_stream(4, key, sizeof(key), 3);
    memset(two + 8, 0x55, 248);
    memcpy(two + 256, (uint8_t[]){0, 0, 0, 8, 103, 100, 0, 42, 1, 2, 3, 4}, 12);
    memcpy(two + 268, rest, sizeof(rest));
    h264_sets_from_the_stream(4, two, sizeof(two), 4);
}

/* avcC parameter sets: the SPS and PPS of a replay clip whose avcC made the
 * boxes show no picture. */
static const uint8_t clip_sps[] = {0x67, 0x4d, 0x40, 0x20, 0xe8, 0x80, 0x28, 0x02, 0xdd, 0x80,
                                   0xb5, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40,
                                   0x00, 0x00, 0x19, 0x03, 0xc6, 0x0c, 0x44, 0x80};
static const uint8_t clip_pps[] = {0x68, 0xeb, 0xaf, 0x20};

/* Appends a 16 bit length and the set. */
static size_t avcc_set(uint8_t *avcc, size_t pos, const uint8_t *set, size_t size)
{
    avcc[pos++] = size >> 8;
    avcc[pos++] = size & 255;
    memcpy(avcc + pos, set, size);
    return pos + size;
}

/* Writes a frame without parameter sets with the avcC and checks that the
 * sets went out as "expected" (Annex B) in front of the frame. */
static void h264_avcc_sets(const uint8_t *avcc, size_t size, const uint8_t *expected, size_t expected_size)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x65, 0x88};
    /* exactly as long as the avcC, so that ASan sees a read beyond it */
    uint8_t *copy = malloc(size);
    WriterAVCallData_t call;
    assert(copy);
    memcpy(copy, avcc, size);
    call = packet(frame, sizeof(frame), copy, size, capture);
    WriterVideoH264.reset();
    assert(WriterVideoH264.writeData(&call) == (int)(14 + expected_size + 4 + 2));
    assert(written_size == 14 + expected_size + 6);
    assert(!memcmp(written + 14, expected, expected_size));
    assert(!memcmp(written + 14 + expected_size, "\0\0\0\1\x65\x88", 6));
    WriterVideoH264.reset();
    free(copy);
}

static size_t annexb(uint8_t *out, const uint8_t *sps, size_t sps_size, const uint8_t *pps, size_t pps_size)
{
    memcpy(out, "\0\0\0\1", 4);
    memcpy(out + 4, sps, sps_size);
    memcpy(out + 4 + sps_size, "\0\0\0\1", 4);
    memcpy(out + 8 + sps_size, pps, pps_size);
    return 8 + sps_size + pps_size;
}

/* SPS and PPS twice, the second PPS with a zero byte counted into its
 * length: each set once, without the zero. */
static void h264_repeated_sets_with_a_trailing_zero(void)
{
    static const uint8_t pps_zero[] = {0x68, 0xeb, 0xaf, 0x20, 0};
    uint8_t avcc[128] = {1, 0x4d, 0x40, 0x20, 0xff, 0xe2};
    uint8_t expected[64];
    size_t pos = 6;
    pos = avcc_set(avcc, pos, clip_sps, sizeof(clip_sps));
    pos = avcc_set(avcc, pos, clip_sps, sizeof(clip_sps));
    avcc[pos++] = 2;
    pos = avcc_set(avcc, pos, clip_pps, sizeof(clip_pps));
    pos = avcc_set(avcc, pos, pps_zero, sizeof(pps_zero));
    h264_avcc_sets(avcc, pos, expected, annexb(expected, clip_sps, sizeof(clip_sps), clip_pps, sizeof(clip_pps)));
}

/* A zero byte between two PPS that no length counts: both PPS. The old
 * parser read a length of 0 there and then one beyond the avcC, took no
 * parameter set at all and dropped every frame. */
static void h264_stray_zero_between_sets(void)
{
    static const uint8_t pps1[] = {0x68, 0xce, 0x3c, 0x80};
    uint8_t avcc[128] = {1, 0x4d, 0x40, 0x20, 0xff, 0xe1};
    uint8_t expected[64];
    size_t pos = 6, size;
    pos = avcc_set(avcc, pos, clip_sps, sizeof(clip_sps));
    avcc[pos++] = 2;
    pos = avcc_set(avcc, pos, clip_pps, sizeof(clip_pps));
    avcc[pos++] = 0;
    pos = avcc_set(avcc, pos, pps1, sizeof(pps1));
    size = annexb(expected, clip_sps, sizeof(clip_sps), clip_pps, sizeof(clip_pps));
    memcpy(expected + size, "\0\0\0\1", 4);
    memcpy(expected + size + 4, pps1, sizeof(pps1));
    h264_avcc_sets(avcc, pos, expected, size + 4 + sizeof(pps1));
    /* a zero byte in front of the PPS count */
    pos = 6;
    pos = avcc_set(avcc, pos, clip_sps, sizeof(clip_sps));
    avcc[pos++] = 0;
    avcc[pos++] = 1;
    pos = avcc_set(avcc, pos, clip_pps, sizeof(clip_pps));
    h264_avcc_sets(avcc, pos, expected, size);
}

/* Two PPS announced, one there, then the four bytes of the high profile
 * fields: the PPS that is there is taken. */
static void h264_pps_count_beyond_the_avcc(void)
{
    uint8_t avcc[128] = {1, 0x4d, 0x40, 0x20, 0xff, 0xe1};
    uint8_t expected[64];
    size_t pos = 6;
    pos = avcc_set(avcc, pos, clip_sps, sizeof(clip_sps));
    avcc[pos++] = 2;
    pos = avcc_set(avcc, pos, clip_pps, sizeof(clip_pps));
    memcpy(avcc + pos, "\xfd\xf8\xf8\x00", 4);
    pos += 4;
    h264_avcc_sets(avcc, pos, expected, annexb(expected, clip_sps, sizeof(clip_sps), clip_pps, sizeof(clip_pps)));
}

/* An avcC cut within its SPS: nothing beyond it is read (ASan), the frame
 * waits for parameter sets. */
static void h264_cut_avcc(void)
{
    uint8_t avcc[] = {1, 0x4d, 0x40, 0x20, 0xff, 0xe1, 0, 28, 0x67, 0x4d, 0x40, 0x20};
    uint8_t frame[] = {0, 0, 0, 2, 0x65, 0x88};
    size_t cut;
    for (cut = 8; cut <= sizeof(avcc); cut++)
    {
        uint8_t *copy = malloc(cut);
        WriterAVCallData_t call;
        assert(copy);
        memcpy(copy, avcc, cut);
        call = packet(frame, sizeof(frame), copy, cut, record);
        WriterVideoH264.reset();
        assert(!WriterVideoH264.writeData(&call) && !calls);
        WriterVideoH264.reset();
        free(copy);
    }
}

/* An empty NAL in front of the parameter sets of the stream: they are
 * found, the avcC ones are not sent, and no empty start code goes out. */
static void h264_empty_nal_in_front_of_the_stream_sets(void)
{
    uint8_t avcc[] = {1, 100, 0, 42, 255, 225, 0, 4, 103, 100, 0, 41, 1, 0, 2, 104, 2};
    uint8_t key[] = {0, 0, 0, 0, 0, 0, 0, 4, 103, 100, 0, 42, 0, 0, 0, 3, 104, 1, 0,
                     0, 0, 0, 2, 101, 136};
    WriterAVCallData_t call = packet(key, sizeof(key), avcc, sizeof(avcc), capture);
    WriterVideoH264.reset();
    /* the zero at the end of the PPS of the stream is not written either */
    assert(WriterVideoH264.writeData(&call) == 14 + 3 * 4 + 4 + 2 + 2 && part_count == 7);
    assert(!memcmp(written + 14, "\0\0\0\1\x67\x64\0\x2a\0\0\0\1\x68\1\0\0\0\1\x65\x88", 20));
    WriterVideoH264.reset();
}

/* hvcC with the SPS twice and a PPS with a zero byte at its end. */
static void h265_repeated_sets_with_a_trailing_zero(void)
{
    uint8_t extra[23 + 3 + 4 + 4 + 3 + 5] = {[0] = 1, [21] = 3, [22] = 2,
                                             [23] = 0x21, [25] = 2, [27] = 2, [28] = 0x42, [29] = 1,
                                             [31] = 2, [32] = 0x42, [33] = 1,
                                             [34] = 0x22, [36] = 1, [38] = 3, [39] = 0x44, [40] = 1};
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    WriterAVCallData_t call = packet(frame, sizeof(frame), extra, sizeof(extra), record);
    WriterVideoH265.reset();
    assert(WriterVideoH265.writeData(&call) == 14 + 12 + 4 + 2 && part_count == 4);
    assert(parts[1].iov_len == 12 && !memcmp(parts[1].iov_base, "\0\0\0\1\x42\1\0\0\0\1\x44\1", 12));
    WriterVideoH265.reset();
}

/* Constrained High: constraint_set4/5 of High profile SPS cleared. The SPS
 * of a live encoder whose streams played without picture on Broadcom boxes:
 * High (100), constraint byte 0x0C, level 4.0. */
static const uint8_t ch_sps[] = {0x67, 0x64, 0x0c, 0x28, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xbb, 0x01, 0x10};
static const uint8_t ch_pps[] = {0x68, 0xce, 0x3c, 0x80};

/* Writes "data" (and "extra" as codec data) in buffers of their exact size,
 * so that ASan sees a read beyond them, and checks that the elementary
 * stream behind the PES header is "expected" and that the caller's buffers
 * are not changed. */
static void h264_es(const uint8_t *data, size_t size, const uint8_t *extra, size_t extra_size,
                    const uint8_t *expected, size_t expected_size)
{
    uint8_t *copy = malloc(size);
    uint8_t *extra_copy = extra_size ? malloc(extra_size) : NULL;
    WriterAVCallData_t call;
    size_t pes;
    assert(copy && (!extra_size || extra_copy));
    memcpy(copy, data, size);
    if (extra_size)
        memcpy(extra_copy, extra, extra_size);
    call = packet(copy, size, extra_copy, extra_size, capture);
    WriterVideoH264.reset();
    assert(WriterVideoH264.writeData(&call) == (int)written_size);
    /* the PES header, with the fake start code of the Annex B path */
    pes = parts[0].iov_len;
    assert(written_size == pes + expected_size);
    assert(!memcmp(written + pes, expected, expected_size));
    assert(!memcmp(copy, data, size));
    assert(!extra_size || !memcmp(extra_copy, extra, extra_size));
    WriterVideoH264.reset();
    free(copy);
    free(extra_copy);
}

/* Appends an Annex B NAL unit. */
static size_t put_nal(uint8_t *out, size_t pos, const uint8_t *nal, size_t size)
{
    memcpy(out + pos, "\0\0\0\1", 4);
    memcpy(out + pos + 4, nal, size);
    return pos + 4 + size;
}

/* A TS packet: AUD, SPS, PPS and slice, SPS and PPS in front of every IDR. */
static size_t ch_access_unit(uint8_t *out, const uint8_t *sps, size_t sps_size)
{
    size_t pos = put_nal(out, 0, (const uint8_t[]){0x09, 0xf0}, 2);
    pos = put_nal(out, pos, sps, sps_size);
    pos = put_nal(out, pos, ch_pps, sizeof(ch_pps));
    return put_nal(out, pos, (const uint8_t[]){0x65, 0x88, 0x84}, 3);
}

/* An Annex B packet with a SPS of "header", "profile", "flags" and
 * "level": written with "expected" as its constraint byte. */
static void h264_constraint_annexb(uint8_t header, uint8_t profile, uint8_t flags, uint8_t level, uint8_t expected)
{
    uint8_t sps[sizeof(ch_sps)], wanted[sizeof(ch_sps)];
    uint8_t data[64], out[64];
    size_t size;
    memcpy(sps, ch_sps, sizeof(sps));
    sps[0] = header;
    sps[1] = profile;
    sps[2] = flags;
    sps[3] = level;
    memcpy(wanted, sps, sizeof(wanted));
    wanted[2] = expected;
    size = ch_access_unit(data, sps, sizeof(sps));
    assert(ch_access_unit(out, wanted, sizeof(wanted)) == size);
    h264_es(data, size, NULL, 0, out, size);
}

static void h264_constrained_high_in_the_packet(void)
{
    /* the stream of the encoder: High, 0x0C, level 4.0 */
    h264_constraint_annexb(0x67, 100, 0x0c, 40, 0x00);
    /* one of the two flags, other High profiles, nal_ref_idc 1 */
    h264_constraint_annexb(0x67, 100, 0x08, 40, 0x00);
    h264_constraint_annexb(0x67, 100, 0x04, 42, 0x00);
    h264_constraint_annexb(0x67, 110, 0x1c, 40, 0x10);
    h264_constraint_annexb(0x67, 122, 0x0c, 41, 0x00);
    h264_constraint_annexb(0x67, 244, 0x0c, 50, 0x00);
    h264_constraint_annexb(0x27, 100, 0x0c, 40, 0x00);
    /* the other constraint flags of High stay */
    h264_constraint_annexb(0x67, 100, 0xec, 40, 0xe0);
}

static void h264_other_profiles_keep_their_flags(void)
{
    /* High without the flags, Baseline, Main, Extended, MVC */
    h264_constraint_annexb(0x67, 100, 0x00, 40, 0x00);
    h264_constraint_annexb(0x67, 100, 0x10, 40, 0x10);
    h264_constraint_annexb(0x67, 66, 0xcc, 30, 0xcc);
    h264_constraint_annexb(0x67, 77, 0x4c, 40, 0x4c);
    h264_constraint_annexb(0x67, 88, 0x0c, 30, 0x0c);
    h264_constraint_annexb(0x67, 118, 0x0c, 40, 0x0c);
    h264_constraint_annexb(0x67, 128, 0x0c, 40, 0x0c);
    /* level_idc 0: a cleared byte could start an emulated start code */
    h264_constraint_annexb(0x67, 100, 0x0c, 0, 0x0c);
}

/* Two SPS in one packet, Annex B codec data in front of a packet
 * without parameter sets. */
static void h264_constrained_high_annexb_codec_data(void)
{
    uint8_t high10[sizeof(ch_sps)];
    uint8_t data[96], out[96], extra[64], extra_out[64];
    uint8_t sps0[sizeof(ch_sps)];
    size_t size, extra_size;
    memcpy(high10, ch_sps, sizeof(high10));
    high10[1] = 110;
    high10[2] = 0x08;
    memcpy(sps0, ch_sps, sizeof(sps0));
    sps0[2] = 0;
    size = put_nal(data, 0, ch_sps, sizeof(ch_sps));
    size = put_nal(data, size, high10, sizeof(high10));
    size = put_nal(data, size, (const uint8_t[]){0x65, 0x88}, 2);
    high10[2] = 0;
    assert(put_nal(out, put_nal(out, put_nal(out, 0, sps0, sizeof(sps0)), high10, sizeof(high10)),
                   (const uint8_t[]){0x65, 0x88}, 2) == size);
    h264_es(data, size, NULL, 0, out, size);

    extra_size = put_nal(extra, put_nal(extra, 0, ch_sps, sizeof(ch_sps)), ch_pps, sizeof(ch_pps));
    put_nal(extra_out, put_nal(extra_out, 0, sps0, sizeof(sps0)), ch_pps, sizeof(ch_pps));
    size = put_nal(data, 0, (const uint8_t[]){0x65, 0x88}, 2);
    memcpy(out, extra_out, extra_size);
    memcpy(out + extra_size, data, size);
    h264_es(data, size, extra, extra_size, out, extra_size + size);
}

/* The SPS of the avcC (MP4, MKV) and SPS in length prefixed samples. */
static void h264_constrained_high_avcc(void)
{
    uint8_t avcc[64] = {1, 100, 0x0c, 40, 0xff, 0xe1};
    uint8_t sps0[sizeof(ch_sps)];
    uint8_t out[64];
    uint8_t frame[] = {0, 0, 0, 2, 0x65, 0x88};
    uint8_t key[4 + sizeof(ch_sps) + 4 + sizeof(ch_pps) + 6];
    uint8_t empty[] = {1, 100, 0x0c, 40, 0xff, 0xe0, 0};
    size_t pos = 6, size;
    memcpy(sps0, ch_sps, sizeof(sps0));
    sps0[2] = 0;
    pos = avcc_set(avcc, pos, ch_sps, sizeof(ch_sps));
    avcc[pos++] = 1;
    pos = avcc_set(avcc, pos, ch_pps, sizeof(ch_pps));
    size = put_nal(out, 0, sps0, sizeof(sps0));
    size = put_nal(out, size, ch_pps, sizeof(ch_pps));
    size = put_nal(out, size, frame + 4, 2);
    h264_es(frame, sizeof(frame), avcc, pos, out, size);

    /* the sets in the key frame, the avcC without them */
    memcpy(key, "\0\0\0\x0c", 4);
    memcpy(key + 4, ch_sps, sizeof(ch_sps));
    memcpy(key + 16, "\0\0\0\x04", 4);
    memcpy(key + 20, ch_pps, sizeof(ch_pps));
    memcpy(key + 24, frame, sizeof(frame));
    h264_es(key, sizeof(key), empty, sizeof(empty), out, size);
}

/* Packets cut within the SPS: nothing read beyond them (ASan), a SPS too
 * short for its level byte is written as it is. */
static void h264_constrained_high_cut(void)
{
    uint8_t data[64], out[64];
    uint8_t avcc[64] = {1, 100, 0x0c, 40, 0xff, 0xe1};
    uint8_t key[4 + sizeof(ch_sps)];
    size_t pos = 6, cut;
    ch_access_unit(data, ch_sps, sizeof(ch_sps));
    /* AUD, start code, then 67 64 0C 28: the level byte is the 14th */
    for (cut = 5; cut <= 6 + 4 + 4; cut++)
    {
        memcpy(out, data, cut);
        if (cut == 6 + 4 + 4)
            out[12] = 0;
        h264_es(data, cut, NULL, 0, out, cut);
    }
    /* length prefixed: the length says 12, the packet ends earlier */
    pos = avcc_set(avcc, pos, ch_sps, sizeof(ch_sps));
    avcc[pos++] = 1;
    pos = avcc_set(avcc, pos, ch_pps, sizeof(ch_pps));
    memcpy(key, "\0\0\0\x0c", 4);
    memcpy(key + 4, ch_sps, sizeof(ch_sps));
    for (cut = 6; cut <= 8; cut++)
    {
        uint8_t *copy = malloc(cut);
        WriterAVCallData_t call;
        assert(copy);
        memcpy(copy, key, cut);
        call = packet(copy, cut, avcc, pos, capture);
        WriterVideoH264.reset();
        assert(WriterVideoH264.writeData(&call) > 0);
        /* the SPS of the packet as far as it goes, its byte 2 cleared once it has a level */
        assert(!memcmp(written + written_size - (cut - 4), key + 4, cut - 4 < 3 ? cut - 4 : 2));
        if (cut == 8)
            assert(!written[written_size - 2]);
        assert(!memcmp(copy, key, cut));
        WriterVideoH264.reset();
        free(copy);
    }
}

/* More SPS in a packet than bytes to clear: the rest stays, nothing
 * beyond the iovec. */
static void h264_constrained_high_many_sets(void)
{
    uint8_t data[45 * 8 + 6], out[45 * 8 + 6];
    size_t pos = 0;
    int i;
    for (i = 0; i < 45; i++)
    {
        pos = put_nal(data, pos, (const uint8_t[]){0x67, 100, 0x0c, 40}, 4);
        put_nal(out, pos - 8, (const uint8_t[]){0x67, 100, i < 32 ? 0 : 0x0c, 40}, 4);
    }
    pos = put_nal(data, pos, (const uint8_t[]){0x65, 0x88}, 2);
    put_nal(out, pos - 6, (const uint8_t[]){0x65, 0x88}, 2);
    h264_es(data, pos, NULL, 0, out, pos);
}

int main(void)
{
    h264_constrained_high_in_the_packet();
    h264_other_profiles_keep_their_flags();
    h264_constrained_high_annexb_codec_data();
    h264_constrained_high_avcc();
    h264_constrained_high_cut();
    h264_constrained_high_many_sets();
    h264_repeated_sets_with_a_trailing_zero();
    h264_stray_zero_between_sets();
    h264_pps_count_beyond_the_avcc();
    h264_cut_avcc();
    h264_empty_nal_in_front_of_the_stream_sets();
    h265_repeated_sets_with_a_trailing_zero();
    h264_nal_lengths();
    h264_parameter_sets_in_a_later_packet();
    h265_nal_lengths();
    h265_iovec_limit();
    h265_extradata_00_xx_01_is_no_start_code();
    h265_four_byte_start_code_in_extradata();
    h265_new_parameter_sets();
    h265_parameter_sets_without_extradata();
    h265_annexb_without_ts_flag();
    mpeg2_packet_shorter_than_a_start_code();
    mpeg2_sequence_header_after_a_seek();
    vc1_wmv_codec_data();
    vorbis_headers();
    vp9_frame();
    vp9_frame_without_pts();
    pcm_block_duration();
    lpcm_rest_buffer();
    adts_frame_with_crc();
    aac_main_keeps_the_sample_rate();
    h264_reset_during_a_write();
    aac_latm_reset_during_a_write();
    vorbis_without_call_data();
    h264_two_byte_nal_lengths();
    h264_long_parameter_sets();
    return 0;
}
