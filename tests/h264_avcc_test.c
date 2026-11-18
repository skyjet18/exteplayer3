/* MP4 files of tests/h264_avcc_mp4.py through the H.264 writer: the
 * avcC of the replay clips (SPS and PPS twice, a zero byte at the end of
 * a PPS) and one with a zero byte between two PPS. The parameter sets are
 * written once each and without trailing zeros, and libavcodec decodes
 * every frame from what the writer wrote.
 *
 *     h264_avcc_test FILE SPS PPS FRAMES */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include "common.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

static uint8_t *es;
static size_t es_size, es_room;

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

/* The elementary stream: everything but the PES header in iov[0]. */
static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    ssize_t size = 0;
    int i;
    (void)fd;
    for (i = 0; i < count; i++)
    {
        size += iov[i].iov_len;
        if (!i)
            continue;
        if (es_size + iov[i].iov_len > es_room)
        {
            es_room = (es_size + iov[i].iov_len) * 2;
            es = realloc(es, es_room);
            assert(es);
        }
        memcpy(es + es_size, iov[i].iov_base, iov[i].iov_len);
        es_size += iov[i].iov_len;
    }
    return size;
}

static size_t next_start(size_t pos)
{
    while (pos + 4 <= es_size && memcmp(es + pos, "\0\0\0\1", 4))
        pos++;
    return pos + 4 <= es_size ? pos : es_size;
}

/* The writer starts every NAL unit with 00 00 00 01. */
static void count_sets(int *sps, int *pps, int *trailing)
{
    size_t pos = next_start(0);
    *sps = *pps = *trailing = 0;
    while (pos < es_size)
    {
        size_t nal = pos + 4;
        size_t end = next_start(nal);
        int type;
        assert(end > nal);
        type = es[nal] & 0x1f;
        if (type == 7 || type == 8)
        {
            /* a zero at the end of a parameter set */
            if (!es[end - 1])
                (*trailing)++;
            if (type == 7)
                (*sps)++;
            else
                (*pps)++;
        }
        if (type == 5)
            break;
        pos = end;
    }
}

/* End of what the writer wrote for each packet: one access unit each. */
static size_t unit_end[64];
static int units;

static int receive(AVCodecContext *context, AVFrame *frame)
{
    int frames = 0;
    while (avcodec_receive_frame(context, frame) == 0)
    {
        assert(frame->width == 64 && frame->height == 64);
        frames++;
    }
    return frames;
}

/* Each access unit as one packet, no parser in between; then the
 * frames the decoder still holds. */
static int decode(void)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    size_t start = 0;
    int frames = 0, i;
    assert(context && packet && frame);
    assert(avcodec_open2(context, codec, NULL) == 0);
    for (i = 0; i < units; i++)
    {
        size_t size = unit_end[i] - start;
        if (size)
        {
            assert(av_new_packet(packet, (int)size) == 0);
            memcpy(packet->data, es + start, size);
            packet->pts = i;
            /* a unit the decoder rejects shows as a missing frame */
            avcodec_send_packet(context, packet);
            av_packet_unref(packet);
            frames += receive(context, frame);
        }
        start = unit_end[i];
    }
    assert(avcodec_send_packet(context, NULL) == 0);
    frames += receive(context, frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return frames;
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVPacket *packet = av_packet_alloc();
    int stream, sps, pps, trailing, frames, packets = 0;
    assert(argc == 5 && packet);
    assert(avformat_open_input(&format, argv[1], NULL, NULL) == 0);
    assert(avformat_find_stream_info(format, NULL) >= 0);
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    assert(stream >= 0);
    WriterVideoH264.reset();
    while (av_read_frame(format, packet) == 0)
    {
        if (packet->stream_index == stream)
        {
            AVCodecParameters *par = format->streams[stream]->codecpar;
            WriterAVCallData_t call = {.fd = 1, .data = packet->data, .len = packet->size,
                                       .Pts = 90000, .Dts = 90000, .private_data = par->extradata,
                                       .private_size = par->extradata_size, .WriteV = capture};
            WriterVideoH264.writeData(&call);
            packets++;
            assert(units < 64);
            unit_end[units++] = es_size;
        }
        av_packet_unref(packet);
    }
    count_sets(&sps, &pps, &trailing);
    frames = decode();
    fprintf(stderr, "%s: %d SPS, %d PPS (%d with trailing zeros) before the first picture, %d of %d frames decoded\n",
            argv[1], sps, pps, trailing, frames, packets);
    assert(sps == atoi(argv[2]) && pps == atoi(argv[3]) && !trailing && frames == packets && packets == atoi(argv[4]));
    WriterVideoH264.reset();
    av_packet_free(&packet);
    avformat_close_input(&format);
    free(es);
    return 0;
}
