/* The sh4 writers on the host: their packets go to a WriteV of the test. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <libavcodec/avcodec.h>

#include "common.h"
#include "writer.h"
#include "pcm.h"

/* the writers link nothing of the player that asks these */
int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

static int writes;
static int fail_write;

static ssize_t count_writev(int fd, const struct iovec *iov, int ic)
{
    ssize_t len = 0;
    (void)fd;
    /* a writer that does not get on would loop here for ever */
    assert(++writes < 1000);
    if (fail_write && writes == fail_write)
        return -1;
    for (int i = 0; i < ic; i++)
        len += (ssize_t)iov[i].iov_len;
    return len;
}

static int write_pcm(int32_t rate, int32_t channels, int32_t bits)
{
    static uint8_t samples[8192];
    pcmPrivateData_t pcm = {
        .channels = channels,
        .bits_per_coded_sample = bits,
        .sample_rate = rate,
        .codec_id = bits == 24 ? AV_CODEC_ID_PCM_S24LE : AV_CODEC_ID_PCM_S16LE,
    };
    WriterAVCallData_t call = {
        .fd = 1,
        .data = samples,
        .len = sizeof(samples),
        .private_data = (uint8_t *)&pcm,
        .private_size = sizeof(pcm),
        .WriteV = count_writev,
    };
    writes = 0;
    WriterAudioPCM.reset();
    return WriterAudioPCM.writeData(&call);
}

/* 48 kHz has a code in the LPCM header: written in whole subframes */
static void pcm_48khz_is_written(void)
{
    assert(write_pcm(48000, 2, 16) == 8192 && writes > 0);
}

/* 32 kHz has none: the subframe length was 0, a division by zero */
static void pcm_32khz_is_not_written(void)
{
    assert(write_pcm(32000, 2, 16) == 0 && writes == 0);
}

/* 8 channels of 24 bit at 192 kHz do not fit a PES: no subframe per PES,
 * the writer wrote empty PES packets without end */
static void pcm_subframe_larger_than_pes_is_not_written(void)
{
    assert(write_pcm(192000, 8, 24) == 0 && writes == 0);
}

static int write_h264(const uint8_t *packet, unsigned int size)
{
    /* an avcC with no parameter set: the writer takes them from the packet */
    static uint8_t avcc[7] = {1, 0x64, 0x00, 0x28, 0xff, 0xe0, 0x00};
    /* exactly as long as the packet, so that ASan sees a read beyond it */
    uint8_t *data = malloc(size);
    assert(data);
    memcpy(data, packet, size);
    WriterAVCallData_t call = {
        .fd = 1,
        .data = data,
        .len = size,
        .private_data = avcc,
        .private_size = sizeof(avcc),
        .FrameRate = 1001,
        .FrameScale = 24000,
        .WriteV = count_writev,
    };
    writes = 0;
    WriterVideoH264.reset();
    int ret = WriterVideoH264.writeData(&call);
    free(data);
    return ret;
}

/* The length of the SPS points 96 bytes beyond the packet: the parameter
 * sets of the packet are not taken, nothing beyond it is read. */
static void h264_sps_beyond_the_packet_is_not_read(void)
{
    static const uint8_t packet[] = {0, 0, 0, 100, 0x67, 0x64, 0x00, 0x28};
    assert(write_h264(packet, sizeof(packet)) >= 0);
}

/* The avcC built from the SPS of the packet is freed when the write of the
 * first header fails (LeakSanitizer). */
static void h264_failed_header_write_frees_the_codec_data(void)
{
    static const uint8_t packet[] = {0, 0, 0, 8, 0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78};
    fail_write = 1;
    assert(write_h264(packet, sizeof(packet)) < 0 && writes == 1);
    fail_write = 0;
}

int main(void)
{
    pcm_48khz_is_written();
    pcm_32khz_is_not_written();
    pcm_subframe_larger_than_pes_is_not_written();
    h264_sps_beyond_the_packet_is_not_read();
    h264_failed_header_write_frees_the_codec_data();
    return 0;
}
