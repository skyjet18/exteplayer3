#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include "common.h"
#include "writer.h"

/* run.sh builds this program with -Dmalloc=test_malloc: the writers allocate here. */
#undef malloc
void *malloc(size_t size);

static int allocations, failing, part_count, calls;
static Writer_t *resetting;

void *test_malloc(size_t size)
{
    if (++allocations == failing)
        return NULL;
    /* The command thread resets the writer while it prepares its codec data. */
    if (resetting)
        resetting->reset();
    return malloc(size);
}

static ssize_t record(int fd, const struct iovec *iov, int count)
{
    ssize_t size = 0;
    int i;
    (void)fd;
    for (i = 0; i < count; i++)
        size += iov[i].iov_len;
    part_count = count;
    calls++;
    return size;
}

/* The allocation with this number fails; 0 for none. */
static WriterAVCallData_t packet(uint8_t *data, unsigned int size, void *private_data,
                                 unsigned int private_size, int fail)
{
    WriterAVCallData_t call = {.fd = 1, .data = data, .len = size, .Pts = 90000, .Dts = 90000,
                               .private_data = private_data, .private_size = private_size,
                               .WriteV = record};
    allocations = part_count = calls = 0;
    failing = fail;
    return call;
}

/* hvcC with one SPS of two bytes; byte 21 holds the NAL length size - 1. */
static uint8_t hvcc[30] = {[0] = 1, [21] = 3, [22] = 1, [23] = 0x21, [25] = 1, [27] = 2,
                           [28] = 0x42, [29] = 1};

static void h265_no_memory_for_the_codec_data(void)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    WriterAVCallData_t call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), 1);
    WriterVideoH265.reset();
    /* The frame is not written, the next one brings the parameter sets. */
    assert(!WriterVideoH265.writeData(&call) && allocations == 1 && !calls);
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), 0);
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    WriterVideoH265.reset();
}

static void h265_reset_while_the_codec_data_is_prepared(void)
{
    uint8_t frame[] = {0, 0, 0, 2, 0x26, 1};
    WriterAVCallData_t call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), 0);
    WriterVideoH265.reset();
    resetting = &WriterVideoH265;
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && allocations == 1);
    resetting = NULL;
    /* The reset is not lost: this frame carries the parameter sets again. */
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), 0);
    assert(WriterVideoH265.writeData(&call) == 14 + 6 + 4 + 2 && part_count == 4);
    call = packet(frame, sizeof(frame), hvcc, sizeof(hvcc), 0);
    assert(WriterVideoH265.writeData(&call) == 14 + 4 + 2 && part_count == 3);
    WriterVideoH265.reset();
}

static void h264_no_memory_for_the_codec_data(void)
{
    uint8_t avcc[] = {1, 100, 0, 42, 255, 225, 0, 4, 103, 100, 0, 42, 1, 0, 2, 104, 1};
    uint8_t frame[] = {0, 0, 0, 2, 101, 136};
    /* The first allocation is the copy of the avcC, the second the codec data. */
    WriterAVCallData_t call = packet(frame, sizeof(frame), avcc, sizeof(avcc), 2);
    WriterVideoH264.reset();
    /* No parameter sets in the frame either: the avcC is tried once more. */
    assert(WriterVideoH264.writeData(&call) == 14 + 14 + 4 + 2 && allocations == 3);
    WriterVideoH264.reset();
}

static void h264_no_memory_for_the_sets_of_the_stream(void)
{
    uint8_t avcc[] = {1, 100, 0, 42, 255, 224, 0}; /* No SPS, no PPS. */
    uint8_t key[] = {0, 0, 0, 4, 103, 100, 0, 42, 0, 0, 0, 2, 104, 1, 0, 0, 0, 2, 101, 136};
    /* The first allocation is the copy of the avcC, the second the avcC that is built. */
    WriterAVCallData_t call = packet(key, sizeof(key), avcc, sizeof(avcc), 2);
    WriterVideoH264.reset();
    assert(!WriterVideoH264.writeData(&call) && allocations == 2 && !calls);
    call = packet(key, sizeof(key), avcc, sizeof(avcc), 0);
    assert(WriterVideoH264.writeData(&call) == 14 + 3 * 4 + 8 && part_count == 7);
    WriterVideoH264.reset();
}

static void aac_latm_no_memory_for_the_context(void)
{
    /* AAC LC, 44.1 kHz, stereo, and a frame of six bytes; padded as the demuxer delivers them. */
    uint8_t config[2 + AV_INPUT_BUFFER_PADDING_SIZE] = {0x12, 0x10};
    uint8_t frame[6 + AV_INPUT_BUFFER_PADDING_SIZE] = {0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c};
    WriterAVCallData_t call = packet(frame, 6, config, 2, 1);
    WriterAudioAACLATM.reset();
    /* The frame is dropped, the next one gets a context. */
    assert(!WriterAudioAACLATM.writeData(&call) && allocations == 1 && !calls);
    call = packet(frame, 6, config, 2, 0);
    assert(WriterAudioAACLATM.writeData(&call) > 14 + 3 + 6 && part_count == 3);
    WriterAudioAACLATM.reset();
}

int main(void)
{
    h265_no_memory_for_the_codec_data();
    h265_reset_while_the_codec_data_is_prepared();
    h264_no_memory_for_the_codec_data();
    h264_no_memory_for_the_sets_of_the_stream();
    aac_latm_no_memory_for_the_context();
    return 0;
}
