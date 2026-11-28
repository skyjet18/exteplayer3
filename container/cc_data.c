/*
 * Closed captions carried in the video stream: the CEA-608 byte pairs of the
 * ATSC A/53 cc_data() structure, found in H.264 and HEVC SEI messages
 * (user_data_registered_itu_t_t35) and in MPEG-2 picture user data, and a
 * small queue that puts the pictures in presentation order.
 *
 * Written from ATSC A/53 Part 4 (cc_data), ITU-T H.264 / H.265 (SEI syntax,
 * emulation prevention) and ISO/IEC 13818-2 (user_data).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Plain C without FFmpeg; included by container_ffmpeg.c and by its test.
 */

#include <stdint.h>
#include <string.h>

enum
{
    CC_CODEC_NONE = 0,
    CC_CODEC_MPEG2,
    CC_CODEC_H264,
    CC_CODEC_HEVC
};

/* bytes of CEA-608 triplets kept for one picture: 32 triplets, more than
 * cc_count can give for the first field of a frame and its second field */
#define CC_DATA_MAX     96
/* pictures waiting for their presentation time */
#define CC_REORDER_MAX  16
/* unescaped bytes of one SEI NAL unit that are looked at */
#define CC_SEI_MAX      1024

typedef struct
{
    uint8_t data[CC_DATA_MAX];
    size_t  size;   /* a multiple of 3 */
    int     text;   /* a pair that is not padding */
} CcData;

typedef struct
{
    int64_t pts;
    CcData  cc;
} CcEntry;

typedef struct
{
    CcEntry entry[CC_REORDER_MAX];
    int     count;  /* sorted by pts, the first one is the oldest */
} CcReorder;

/* ATSC_user_data() from its user_identifier on: "GA94", user_data_type_code 3,
 * cc_data(). The triplets of CEA-608 field 1 (CC1/CC2) with cc_valid set are
 * kept as they are, the decoder checks the parity. */
static void cc_atsc_user_data(const uint8_t *p, size_t size, CcData *out)
{
    size_t count;
    size_t i;

    if (size < 7 || memcmp(p, "GA94", 4) || p[4] != 3)
        return;
    if (!(p[5] & 0x40))  /* process_cc_data_flag */
        return;
    count = p[5] & 0x1f;
    p += 7;  /* flags and cc_count, em_data */
    size -= 7;
    for (i = 0; i < count && size >= 3; i++, p += 3, size -= 3)
    {
        if ((p[0] & 7) != 4)  /* cc_valid, cc_type 0 */
            continue;
        if (out->size + 3 > CC_DATA_MAX)
            return;
        memcpy(out->data + out->size, p, 3);
        out->size += 3;
        if ((p[1] & 0x7f) || (p[2] & 0x7f))
            out->text = 1;
    }
}

/* user_data_registered_itu_t_t35: United States, ATSC */
static void cc_t35(const uint8_t *p, size_t size, CcData *out)
{
    if (size < 3 || p[0] != 0xB5 || p[1] != 0x00 || p[2] != 0x31)
        return;
    cc_atsc_user_data(p + 3, size - 3, out);
}

/* sei_message()s of an SEI RBSP */
static void cc_sei(const uint8_t *p, size_t size, CcData *out)
{
    size_t pos = 0;

    while (pos + 2 <= size)
    {
        size_t type = 0;
        size_t length = 0;

        while (pos < size && p[pos] == 0xFF)
        {
            type += 255;
            pos++;
        }
        if (pos >= size)
            return;
        type += p[pos++];
        while (pos < size && p[pos] == 0xFF)
        {
            length += 255;
            pos++;
        }
        if (pos >= size)
            return;
        length += p[pos++];
        if (length > size - pos)
            return;
        if (type == 4)
            cc_t35(p + pos, length, out);
        pos += length;
    }
}

/* NAL unit payload without the emulation prevention bytes, at most cap bytes */
static size_t cc_unescape(const uint8_t *src, size_t size, uint8_t *dst, size_t cap)
{
    size_t i;
    size_t n = 0;
    int zeros = 0;

    for (i = 0; i < size && n < cap; i++)
    {
        if (zeros >= 2 && src[i] == 3)
        {
            zeros = 0;
            continue;
        }
        dst[n++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    return n;
}

/* One H.264 or HEVC NAL unit. Returns 1 at the first slice: the SEI of a
 * picture comes before its slices. */
static int cc_nal(int codec, const uint8_t *nal, size_t size, CcData *out)
{
    uint8_t rbsp[CC_SEI_MAX];
    size_t header;
    int type;

    if (codec == CC_CODEC_H264)
    {
        if (size < 1)
            return 0;
        type = nal[0] & 0x1f;
        if (type >= 1 && type <= 5)
            return 1;
        if (type != 6)
            return 0;
        header = 1;
    }
    else
    {
        if (size < 2)
            return 0;
        type = (nal[0] >> 1) & 0x3f;
        if (type < 32)
            return 1;
        if (type != 39)  /* prefix SEI */
            return 0;
        header = 2;
    }
    size = cc_unescape(nal + header, size - header, rbsp, sizeof(rbsp));
    cc_sei(rbsp, size, out);
    return 0;
}

/* The offset of the next start code prefix 00 00 01 at or after pos, size if
 * there is none (then the prefix is complete in data[0..size)). */
static size_t cc_start_code(const uint8_t *data, size_t pos, size_t size)
{
    while (pos <= size && size - pos >= 3)
    {
        const uint8_t *one = memchr(data + pos + 2, 1, size - pos - 2);
        size_t i;

        if (!one)
            break;
        i = (size_t)(one - data);
        if (!data[i - 1] && !data[i - 2])
            return i - 2;
        pos = i - 1;
    }
    return size;
}

static void cc_annexb(int codec, const uint8_t *data, size_t size, CcData *out)
{
    size_t nal = cc_start_code(data, 0, size);

    while (nal < size)
    {
        size_t next;

        nal += 3;
        next = cc_start_code(data, nal, size);
        if (cc_nal(codec, data + nal, next - nal, out))
            return;
        nal = next;
    }
}

static void cc_length_prefixed(int codec, const uint8_t *data, size_t size,
                               int length_size, CcData *out)
{
    size_t pos = 0;

    while (size - pos >= (size_t)length_size)
    {
        size_t length = 0;
        int i;

        for (i = 0; i < length_size; i++)
            length = (length << 8) | data[pos++];
        if (length > size - pos)
            return;
        if (cc_nal(codec, data + pos, length, out))
            return;
        pos += length;
    }
}

/* All user_data() of an MPEG-2 packet: two field pictures can share one. */
static void cc_mpeg2(const uint8_t *data, size_t size, CcData *out)
{
    size_t code = cc_start_code(data, 0, size);

    while (code < size)
    {
        size_t next;

        code += 3;
        if (code >= size)
            return;
        next = cc_start_code(data, code, size);
        if (data[code] == 0xB2)
            cc_atsc_user_data(data + code + 1, next - code - 1, out);
        code = next;
    }
}

/* The CEA-608 field 1 triplets of one video packet. length_size: the NAL
 * length field size of avcC / hvcC packets, 0 for Annex B. */
static void cc_data_extract(int codec, int length_size, const uint8_t *data, size_t size,
                            CcData *out)
{
    out->size = 0;
    out->text = 0;
    if (!data || !size)
        return;
    switch (codec)
    {
    case CC_CODEC_MPEG2:
        cc_mpeg2(data, size, out);
        break;
    case CC_CODEC_H264:
    case CC_CODEC_HEVC:
        if (length_size > 0)
            cc_length_prefixed(codec, data, size, length_size, out);
        else
            cc_annexb(codec, data, size, out);
        break;
    default:
        break;
    }
}

/* The NAL length field size that avcC or hvcC extradata gives, 0 for Annex B. */
static int cc_length_size(int codec, const uint8_t *extradata, size_t size)
{
    if (!extradata)
        return 0;
    if (codec == CC_CODEC_H264 && size >= 7 && extradata[0] == 1)
        return (extradata[4] & 3) + 1;
    if (codec == CC_CODEC_HEVC && size >= 23 &&
        (extradata[0] || extradata[1] || extradata[2] > 1))
        return (extradata[21] & 3) + 1;
    return 0;
}

static void cc_reorder_clear(CcReorder *queue)
{
    queue->count = 0;
}

/* The queue must have room: count < CC_REORDER_MAX. */
static void cc_reorder_insert(CcReorder *queue, int64_t pts, const CcData *cc)
{
    int i = queue->count;

    while (i > 0 && queue->entry[i - 1].pts > pts)
    {
        queue->entry[i] = queue->entry[i - 1];
        i--;
    }
    queue->entry[i].pts = pts;
    queue->entry[i].cc = *cc;
    queue->count++;
}

/* The oldest picture is due when no later packet can come before it: its
 * time is not after the decoding time of the packet just read (a packet is
 * never shown before it is decoded), or more than depth pictures wait. */
static int cc_reorder_due(const CcReorder *queue, int64_t decoded, int depth)
{
    return queue->count > 0 && (queue->count > depth || queue->entry[0].pts <= decoded);
}

static int cc_reorder_pop(CcReorder *queue, CcEntry *out)
{
    if (queue->count <= 0)
        return 0;
    *out = queue->entry[0];
    queue->count--;
    memmove(queue->entry, queue->entry + 1, (size_t)queue->count * sizeof(CcEntry));
    return 1;
}
