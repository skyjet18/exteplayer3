/* The FLV1 (Sorenson Spark) to MPEG-4 part 2 converter: the pictures the
 * MPEG-4 decoder of libavcodec makes of the converted stream against the ones
 * its FLV1 decoder makes of the source.
 *
 * - a picture written bit by bit in the version 0 format: AC levels as 8 bit
 *   escapes, most of them negative, and macroblocks that change the quantiser
 *   with DQUANT. Its DC values are multiples of the MPEG-4 DC scale of their
 *   macroblock, so the conversion is exact;
 * - pictures of the FLV1 encoder of libavcodec (version 1, 7 and 11 bit
 *   escapes) with spatial masking, which moves the quantiser per macroblock,
 *   I and P pictures. The DC values are rounded by the conversion here;
 * - DQUANT that takes the quantiser below 1 or above 31 (clamped as in the
 *   H.263 decoder) and a header with the quantiser 0 (picture dropped);
 * - cut and damaged pictures from buffers without padding, under ASan;
 * - a picture whose MPEG-4 form does not fit into the packet buffer. */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include "flv2mpeg4/flv2mpeg4.h"

#define MAX_PACKETS 32

typedef struct
{
    AVFrame *frame[MAX_PACKETS];
    int count;
} Frames;

static uint8_t extradata[256];
static int extradata_size;
static AVPacket *converted[MAX_PACKETS];
static int converted_count;

static int write_packet(void *usr_data, int keyframe, int pts, const uint8_t *buf, int size)
{
    AVPacket *pkt = av_packet_alloc();
    (void)usr_data;
    (void)pts;
    assert(pkt && size > 0 && av_new_packet(pkt, size) == 0);
    memcpy(pkt->data, buf, size);
    if (keyframe)
        pkt->flags |= AV_PKT_FLAG_KEY;
    assert(converted_count < MAX_PACKETS);
    converted[converted_count++] = pkt;
    return 0;
}

static int write_extradata(void *usr_data, int width, int height, int bitrate, const uint8_t *data, int size)
{
    (void)usr_data;
    (void)width;
    (void)height;
    (void)bitrate;
    assert(size > 0 && size <= (int)sizeof(extradata));
    memcpy(extradata, data, size);
    extradata_size = size;
    return 0;
}

/* every source packet becomes one MPEG-4 packet: the times leave no gap that
 * the converter would fill with not coded pictures */
static void convert(AVPacket **packets, int count, int width, int height)
{
    flv2mpeg4_CTX *ctx = flv2mpeg4_init_ctx(NULL, width, height, write_packet, write_extradata);
    int i;
    assert(ctx);
    extradata_size = 0;
    converted_count = 0;
    assert(flv2mpeg4_prepare_extra_data(ctx) == 0 && extradata_size > 0);
    for (i = 0; i < count; i++)
        assert(flv2mpeg4_process_flv_packet(ctx, 0, packets[i]->data, packets[i]->size, i * 1000 / 30) == 0);
    flv2mpeg4_release_ctx(&ctx);
    assert(converted_count == count);
}

static void decode(enum AVCodecID id, const uint8_t *config, int config_size, AVPacket **packets, int count, Frames *out)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *ctx;
    AVFrame *frame = av_frame_alloc();
    int i, ret;

    assert(codec && frame);
    ctx = avcodec_alloc_context3(codec);
    assert(ctx);
    ctx->thread_count = 1;
    if (config_size > 0)
    {
        ctx->extradata = av_mallocz(config_size + AV_INPUT_BUFFER_PADDING_SIZE);
        assert(ctx->extradata);
        memcpy(ctx->extradata, config, config_size);
        ctx->extradata_size = config_size;
    }
    assert(avcodec_open2(ctx, codec, NULL) == 0);
    out->count = 0;
    for (i = 0; i <= count; i++)
    {
        ret = avcodec_send_packet(ctx, i < count ? packets[i] : NULL);
        assert(ret == 0);
        while ((ret = avcodec_receive_frame(ctx, frame)) == 0)
        {
            assert(out->count < MAX_PACKETS);
            out->frame[out->count++] = av_frame_clone(frame);
            av_frame_unref(frame);
        }
        assert(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    }
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
}

static void free_frames(Frames *frames)
{
    int i;
    for (i = 0; i < frames->count; i++)
        av_frame_free(&frames->frame[i]);
    frames->count = 0;
}

static void free_packets(AVPacket **packets, int count)
{
    int i;
    for (i = 0; i < count; i++)
        av_packet_free(&packets[i]);
}

/* PSNR over the three planes and the largest difference of a sample */
static double compare(const AVFrame *a, const AVFrame *b, int *max_diff)
{
    double sum = 0;
    long samples = 0;
    int plane, x, y;

    assert(a->format == AV_PIX_FMT_YUV420P && b->format == AV_PIX_FMT_YUV420P);
    assert(a->width == b->width && a->height == b->height);
    *max_diff = 0;
    for (plane = 0; plane < 3; plane++)
    {
        int w = plane ? (a->width + 1) / 2 : a->width;
        int h = plane ? (a->height + 1) / 2 : a->height;
        for (y = 0; y < h; y++)
        {
            for (x = 0; x < w; x++)
            {
                int d = a->data[plane][y * a->linesize[plane] + x] - b->data[plane][y * b->linesize[plane] + x];
                sum += (double)d * d;
                if (abs(d) > *max_diff)
                    *max_diff = abs(d);
                samples++;
            }
        }
    }
    if (sum == 0)
        return 99.0;
    return 10.0 * log10(255.0 * 255.0 * samples / sum);
}

/* converts, decodes both and checks every picture; returns the lowest PSNR */
static double check(const char *name, AVPacket **packets, int count, int width, int height, int max_allowed_diff, double min_psnr)
{
    Frames source = {0}, mpeg4 = {0};
    double lowest = 99.0;
    int i;

    convert(packets, count, width, height);
    decode(AV_CODEC_ID_FLV1, NULL, 0, packets, count, &source);
    decode(AV_CODEC_ID_MPEG4, extradata, extradata_size, converted, converted_count, &mpeg4);
    printf("%s: %d packets, %d FLV1 pictures, %d MPEG-4 pictures\n", name, count, source.count, mpeg4.count);
    assert(source.count == count && mpeg4.count == count);
    for (i = 0; i < count; i++)
    {
        int max_diff;
        double psnr = compare(source.frame[i], mpeg4.frame[i], &max_diff);
        printf("%s: picture %d (%c): PSNR %.2f dB, largest difference %d\n", name, i,
               av_get_picture_type_char(source.frame[i]->pict_type), psnr, max_diff);
        if (psnr < lowest)
            lowest = psnr;
        if (psnr < min_psnr || max_diff > max_allowed_diff)
        {
            fprintf(stderr, "%s: picture %d differs from the FLV1 decode (PSNR %.2f < %.2f dB or difference %d > %d)\n",
                    name, i, psnr, min_psnr, max_diff, max_allowed_diff);
            exit(1);
        }
    }
    free_frames(&source);
    free_frames(&mpeg4);
    free_packets(converted, converted_count);
    converted_count = 0;
    return lowest;
}

typedef struct
{
    uint8_t *buf;
    int size;
    int bits;
} Bits;

static void bits_init(Bits *b, int size)
{
    b->buf = calloc(1, size);
    b->size = size;
    b->bits = 0;
    assert(b->buf);
}

static void put(Bits *b, int count, unsigned value)
{
    int i;
    for (i = count - 1; i >= 0; i--)
    {
        assert(b->bits < b->size * 8);
        if (value >> i & 1)
            b->buf[b->bits >> 3] |= 0x80 >> (b->bits & 7);
        b->bits++;
    }
}

/* the bits as a packet with two zero bytes behind them; frees the bits */
static AVPacket *bits_packet(Bits *b)
{
    AVPacket *pkt = av_packet_alloc();
    assert(pkt && av_new_packet(pkt, (b->bits + 7) / 8 + 2) == 0);
    memcpy(pkt->data, b->buf, (b->bits + 7) / 8);
    memset(pkt->data + (b->bits + 7) / 8, 0, 2);
    pkt->flags |= AV_PKT_FLAG_KEY;
    free(b->buf);
    b->buf = NULL;
    return pkt;
}

/* intra DC and the AC coefficients {run, level}, every one an 8 bit escape */
static void put_block(Bits *b, int dc, const int (*ac)[2], int count)
{
    int i;
    put(b, 8, dc);
    for (i = 0; i < count; i++)
    {
        put(b, 7, 3);                /* ESCAPE */
        put(b, 1, i == count - 1);   /* LAST */
        put(b, 6, ac[i][0]);         /* RUN */
        put(b, 8, ac[i][1] & 0xff);  /* LEVEL, two's complement */
    }
}

/* the header of a version 0 I picture */
static void put_header(Bits *b, int width, int height, int qscale)
{
    put(b, 17, 1);       /* picture start code */
    put(b, 5, 0);        /* version 0: H.263 escapes */
    put(b, 8, 0);        /* temporal reference */
    if (width < 256 && height < 256)
    {
        put(b, 3, 0);    /* 8 bit size */
        put(b, 8, width);
        put(b, 8, height);
    }
    else
    {
        put(b, 3, 1);    /* 16 bit size */
        put(b, 16, width);
        put(b, 16, height);
    }
    put(b, 2, 0);        /* I picture */
    put(b, 1, 0);        /* deblocking */
    put(b, 5, qscale);   /* quantiser */
    put(b, 1, 0);        /* PEI */
}

/* 32x32, four intra macroblocks with the quantiser of the header, then the
 * DQUANT codes of the macroblocks 1-3 (0: -1, 1: -2, 2: +1, 3: +2) */
static AVPacket *escaped_picture(int qscale, const int *dquant_code, const int (*dc)[6])
{
    static const int ac_a[][2] = {{0, -12}, {2, 9}, {1, -5}};
    static const int ac_b[][2] = {{1, -10}, {0, 6}, {4, -3}};
    static const int ac_c[][2] = {{0, -7}, {0, -6}, {3, 4}, {2, -2}};
    const int (*ac[3])[2] = {ac_a, ac_b, ac_c};
    const int ac_count[3] = {3, 3, 4};
    Bits b;
    int mb, n;

    bits_init(&b, 1024);
    put_header(&b, 32, 32, qscale);
    for (mb = 0; mb < 4; mb++)
    {
        int code = mb ? dquant_code[mb - 1] : -1;
        if (code < 0)
            put(&b, 3, 3);   /* MCBPC: intra, both chroma blocks coded */
        else
            put(&b, 6, 3);   /* MCBPC: intra + DQUANT, both chroma blocks coded */
        put(&b, 2, 3);       /* CBPY: all luma blocks coded */
        if (code >= 0)
            put(&b, 2, code);
        for (n = 0; n < 6; n++)
            put_block(&b, dc[mb][n], ac[(mb + n) % 3], ac_count[(mb + n) % 3]);
    }
    return bits_packet(&b);
}

/* quantiser 10, then DQUANT +2, -1, +1. Luma DC * 8 is a multiple of the
 * MPEG-4 DC scale of the macroblock (18, 20, 19, 20) and chroma DC * 8 of
 * its chroma DC scale (11, 12, 12, 12). */
static AVPacket *escaped_picture_q10(void)
{
    static const int dquant_code[3] = {3, 0, 2};
    static const int dc[4][6] = {
        {90, 99, 108, 117, 121, 110},
        {100, 105, 110, 115, 126, 120},
        {95, 114, 76, 133, 117, 129},
        {120, 125, 80, 85, 123, 132},
    };
    return escaped_picture(10, dquant_code, dc);
}

static void fill_picture(AVFrame *frame, int t)
{
    int x, y;
    assert(av_frame_make_writable(frame) == 0);
    for (y = 0; y < frame->height; y++)
    {
        for (x = 0; x < frame->width; x++)
        {
            /* a flat left half and a detailed right half: masking gives
             * them different quantisers */
            int v = 60 + x + y + 3 * t;
            if (x >= frame->width / 2)
                v += ((x * 37 + y * 101 + t * 7) * 2654435761u >> 27) * 5 - 40;
            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
        }
    }
    for (y = 0; y < frame->height / 2; y++)
    {
        for (x = 0; x < frame->width / 2; x++)
        {
            frame->data[1][y * frame->linesize[1] + x] = (uint8_t)(100 + x * 2 + t);
            frame->data[2][y * frame->linesize[2] + x] = (uint8_t)(150 - y * 2 - t);
        }
    }
}

static int encode_pictures(AVPacket **packets, int width, int height, int pictures)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLV1);
    AVCodecContext *ctx;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int count = 0, t, ret;

    assert(codec && frame && pkt);
    ctx = avcodec_alloc_context3(codec);
    assert(ctx);
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = (AVRational){1, 30};
    ctx->framerate = (AVRational){30, 1};
    ctx->gop_size = 6;
    ctx->max_b_frames = 0;
    ctx->bit_rate = 120000;
    ctx->qmin = 2;
    ctx->qmax = 20;
    ctx->spatial_cplx_masking = 0.8f;
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_BITEXACT;
    assert(avcodec_open2(ctx, codec, NULL) == 0);
    frame->format = ctx->pix_fmt;
    frame->width = width;
    frame->height = height;
    assert(av_frame_get_buffer(frame, 0) == 0);
    for (t = 0; t <= pictures; t++)
    {
        if (t < pictures)
        {
            fill_picture(frame, t);
            frame->pts = t;
        }
        ret = avcodec_send_frame(ctx, t < pictures ? frame : NULL);
        assert(ret == 0);
        while ((ret = avcodec_receive_packet(ctx, pkt)) == 0)
        {
            assert(count < MAX_PACKETS);
            packets[count] = av_packet_alloc();
            assert(packets[count]);
            av_packet_move_ref(packets[count], pkt);
            count++;
        }
        assert(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return count;
}

/* converts one packet from a buffer of exactly its size, so that ASan sees
 * every read behind its end; returns what the converter returns and the
 * number of MPEG-4 packets it wrote in *written */
static int convert_exact(const uint8_t *data, int size, int width, int height, int *written)
{
    flv2mpeg4_CTX *ctx = flv2mpeg4_init_ctx(NULL, width, height, write_packet, write_extradata);
    uint8_t *copy = malloc(size);
    int ret;

    assert(ctx && copy);
    memcpy(copy, data, size);
    extradata_size = 0;
    converted_count = 0;
    assert(flv2mpeg4_prepare_extra_data(ctx) == 0);
    ret = flv2mpeg4_process_flv_packet(ctx, 0, copy, size, 0);
    flv2mpeg4_release_ctx(&ctx);
    free(copy);
    *written = converted_count;
    free_packets(converted, converted_count);
    converted_count = 0;
    return ret;
}

/* DQUANT that leaves 1..31 is clamped by the H.263 decoder; the conversion
 * follows it instead of dividing by the DC scale 0 or reading behind the
 * DC scale tables. A header with the quantiser 0 drops the picture. */
static void quantiser_limits(void)
{
    /* 2, then -2, -1, +1: 1, 1, 2 (0, -1, 0 without the clamp); DC scale 8 */
    static const int low_code[3] = {1, 0, 2};
    static const int low_dc[4][6] = {
        {90, 99, 108, 117, 121, 110},
        {100, 105, 110, 115, 126, 120},
        {95, 114, 76, 133, 117, 129},
        {120, 125, 80, 85, 123, 132},
    };
    /* 30, then +2, +2, -1: 31, 31, 30 (32, 34, 33 without the clamp).
     * Luma DC * 8 is a multiple of 44 (30) or 46 (31), chroma DC * 8 one
     * of 24 or 25. */
    static const int high_code[3] = {3, 3, 0};
    static const int high_dc[4][6] = {
        {66, 77, 88, 99, 120, 123},
        {92, 115, 69, 46, 100, 125},
        {46, 69, 92, 115, 75, 50},
        {110, 121, 55, 44, 111, 99},
    };
    AVPacket *pkt;
    int written;

    pkt = escaped_picture(2, low_code, low_dc);
    check("quantiser below 1", &pkt, 1, 32, 32, 1, 48.0);
    av_packet_free(&pkt);

    pkt = escaped_picture(30, high_code, high_dc);
    check("quantiser above 31", &pkt, 1, 32, 32, 1, 48.0);
    av_packet_free(&pkt);

    pkt = escaped_picture(0, low_code, low_dc);
    assert(convert_exact(pkt->data, pkt->size, 32, 32, &written) < 0);
    printf("quantiser 0: %d MPEG-4 packets\n", written);
    assert(written == 0);
    av_packet_free(&pkt);
}

/* Cut pictures: every macroblock that takes bits behind the end of the
 * packet drops the picture, nothing is read there (the buffers are exactly
 * as large as the packet, ASan sees every byte behind it). Damaged pictures:
 * a dropped or a whole picture, never a read behind the packet. */
static void broken_pictures(void)
{
    AVPacket *packets[MAX_PACKETS];
    AVPacket *pkt = escaped_picture_q10();
    uint32_t seed = 12345;
    int count, i, len, n, written, ret, dropped = 0, whole = 0;

    /* the last two bytes are zero padding, the one before them is the
     * last one of the picture */
    assert(convert_exact(pkt->data, pkt->size, 32, 32, &written) == 0 && written == 1);
    for (len = 1; len <= pkt->size - 3; len++)
    {
        ret = convert_exact(pkt->data, len, 32, 32, &written);
        if (ret >= 0 || written != 0)
        {
            fprintf(stderr, "cut picture: %d of %d bytes gave %d, %d packets\n", len, pkt->size, ret, written);
            exit(1);
        }
    }
    printf("cut picture: %d lengths dropped\n", pkt->size - 3);
    av_packet_free(&pkt);

    count = encode_pictures(packets, 96, 64, 12);
    assert(count == 12);
    for (i = 0; i < count; i++)
    {
        uint8_t *copy = malloc(packets[i]->size);
        assert(copy && packets[i]->size > 16);
        for (len = packets[i]->size / 2; len > 0; len /= 2)
        {
            ret = convert_exact(packets[i]->data, len, 96, 64, &written);
            if (ret >= 0 || written != 0)
            {
                fprintf(stderr, "cut %c picture %d: %d of %d bytes gave %d, %d packets\n",
                        i ? 'P' : 'I', i, len, packets[i]->size, ret, written);
                exit(1);
            }
        }
        for (n = 0; n < 50; n++)
        {
            int k;
            memcpy(copy, packets[i]->data, packets[i]->size);
            /* the header stays, so that the macroblocks are decoded */
            for (k = 0; k < 4; k++)
            {
                seed = seed * 1103515245u + 12345u;
                copy[8 + (seed >> 8) % (packets[i]->size - 8)] ^= (uint8_t)(1 + (seed >> 24) % 255);
            }
            ret = convert_exact(copy, packets[i]->size, 96, 64, &written);
            assert(ret == 0 ? written == 1 : ret < 0 && written == 0);
            if (ret < 0)
                dropped++;
            else
                whole++;
        }
        free(copy);
    }
    free_packets(packets, count);
    printf("damaged pictures: %d dropped, %d converted\n", dropped, whole);
}

/* A 640x480 I picture with all 64 coefficients of every block coded, the
 * AC levels +-100 as 8 bit escapes (22 bits each): about 1.3 MB of FLV1.
 * In MPEG-4 each level needs the third escape (30 bits), about 1.7 MB,
 * more than the 1 MB packet buffer of the converter: the picture is
 * dropped instead of written behind the buffer. */
static void oversized_picture(void)
{
    int ac[63][2];
    Bits b;
    AVPacket *pkt;
    int i, mb, n, written;

    for (i = 0; i < 63; i++)
    {
        ac[i][0] = 0;
        ac[i][1] = i & 1 ? -100 : 100;
    }
    bits_init(&b, 2 * 1024 * 1024);
    put_header(&b, 640, 480, 10);
    for (mb = 0; mb < 40 * 30; mb++)
    {
        put(&b, 3, 3);   /* MCBPC: intra, both chroma blocks coded */
        put(&b, 2, 3);   /* CBPY: all luma blocks coded */
        for (n = 0; n < 6; n++)
            put_block(&b, 100, (const int (*)[2])ac, 63);
    }
    pkt = bits_packet(&b);
    printf("oversized picture: %d bytes of FLV1\n", pkt->size);
    assert(convert_exact(pkt->data, pkt->size, 640, 480, &written) < 0 && written == 0);
    av_packet_free(&pkt);
}

int main(void)
{
    AVPacket *packets[MAX_PACKETS];
    int count;
    double lowest;

    packets[0] = escaped_picture_q10();
    lowest = check("8 bit escapes", packets, 1, 32, 32, 1, 48.0);
    free_packets(packets, 1);
    printf("8 bit escapes: lowest PSNR %.2f dB\n", lowest);

    count = encode_pictures(packets, 96, 64, 12);
    assert(count == 12);
    lowest = check("FLV1 encoder", packets, count, 96, 64, 255, 36.0);
    free_packets(packets, count);
    printf("FLV1 encoder: lowest PSNR %.2f dB\n", lowest);

    quantiser_limits();
    broken_pictures();
    oversized_picture();

    printf("flv2mpeg4 tests passed\n");
    return 0;
}
