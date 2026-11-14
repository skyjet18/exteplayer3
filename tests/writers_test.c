#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include "common.h"
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
    /* Without the parameter sets the header is followed by 511 whole NALs. */
    call = packet(nals, sizeof(nals), extra, sizeof(extra), record);
    assert(WriterVideoH265.writeData(&call) == 14 + 511 * 5 && part_count == 1023);
    assert_nals(nals, sizeof(nals), 1);
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

int main(void)
{
    h264_nal_lengths();
    h264_parameter_sets_in_a_later_packet();
    h265_nal_lengths();
    h265_iovec_limit();
    h265_extradata_00_xx_01_is_no_start_code();
    h265_four_byte_start_code_in_extradata();
    mpeg2_packet_shorter_than_a_start_code();
    vc1_wmv_codec_data();
    vorbis_headers();
    vp9_frame();
    vp9_frame_without_pts();
    pcm_block_duration();
    adts_frame_with_crc();
    aac_main_keeps_the_sample_rate();
    return 0;
}
