#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../container/cc_data.c"

/* The CEA-608 triplets of ATSC A/53 cc_data() in H.264 and HEVC SEI (Annex B
 * and length prefixed) and in MPEG-2 user data, and the queue that puts them
 * in presentation order. Every buffer handed to the parser is a heap copy of
 * exactly its size, so ASan sees any read beyond it. */

typedef struct
{
    uint8_t data[4096];
    size_t size;
} Buf;

static void add(Buf *buf, const void *data, size_t size)
{
    assert(buf->size + size <= sizeof(buf->data));
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}

static void byte(Buf *buf, uint8_t value)
{
    add(buf, &value, 1);
}

static int has_emulation_prevention(const Buf *buf)
{
    size_t i;
    for (i = 0; i + 2 < buf->size; i++)
        if (!buf->data[i] && !buf->data[i + 1] && buf->data[i + 2] == 3)
            return 1;
    return 0;
}

/* "GA94" user data with these triplets; flags is the byte with cc_count */
static void ga94(Buf *buf, const uint8_t *triplets, int count, uint8_t flags)
{
    add(buf, "GA94", 4);
    byte(buf, 3);
    byte(buf, (uint8_t)(flags | count));
    byte(buf, 0xFF);  /* em_data */
    add(buf, triplets, (size_t)count * 3);
    byte(buf, 0xFF);  /* marker_bits */
}

/* Field 1 "HI" (odd parity), padding of field 1, field 2 "X", DTVCC, field 1 not valid. */
static const uint8_t mixed[] = {
    0xFC, 0xC8, 0x49,  /* odd('H'), odd('I') */
    0xFC, 0x80, 0x80,
    0xFD, 0x58, 0x80,
    0xFE, 0x01, 0x02,
    0xFF, 0x03, 0x04,
    0xF8, 0x10, 0x20,
};

static void expect_mixed(const CcData *cc)
{
    static const uint8_t want[] = { 0xFC, 0xC8, 0x49, 0xFC, 0x80, 0x80 };
    assert(cc->size == sizeof(want));
    assert(!memcmp(cc->data, want, sizeof(want)));
    assert(cc->text == 1);
}

/* SEI RBSP: an unregistered user data message full of zero bytes (they need
 * emulation prevention), then the registered ITU-T T.35 one with GA94. */
static void sei_rbsp(Buf *rbsp, const uint8_t *triplets, int count, uint8_t flags)
{
    Buf t35 = {{0}, 0};
    uint8_t zeros[20] = {0};

    byte(rbsp, 5);
    byte(rbsp, sizeof(zeros));
    add(rbsp, zeros, sizeof(zeros));
    byte(&t35, 0xB5);
    byte(&t35, 0x00);
    byte(&t35, 0x31);
    ga94(&t35, triplets, count, flags);
    byte(rbsp, 4);
    byte(rbsp, (uint8_t)t35.size);
    add(rbsp, t35.data, t35.size);
    byte(rbsp, 0x80);  /* rbsp_trailing_bits */
}

/* NAL unit (header bytes + RBSP) with emulation prevention */
static void nal(Buf *out, const uint8_t *header, size_t header_size, const Buf *rbsp)
{
    size_t i;
    int zeros = 0;
    add(out, header, header_size);
    for (i = 0; i < rbsp->size; i++)
    {
        uint8_t b = rbsp->data[i];
        if (zeros >= 2 && b <= 3)
        {
            byte(out, 3);
            zeros = 0;
        }
        byte(out, b);
        zeros = b ? 0 : zeros + 1;
    }
}

static void annexb(Buf *out, const Buf *unit)
{
    static const uint8_t start[] = { 0, 0, 0, 1 };
    add(out, start, sizeof(start));
    add(out, unit->data, unit->size);
}

static void prefixed(Buf *out, const Buf *unit, int length_size)
{
    int i;
    for (i = length_size - 1; i >= 0; i--)
        byte(out, (uint8_t)(unit->size >> (8 * i)));
    add(out, unit->data, unit->size);
}

static void extract(int codec, int length_size, const Buf *buf, CcData *cc)
{
    uint8_t *copy = malloc(buf->size ? buf->size : 1);
    assert(copy);
    memcpy(copy, buf->data, buf->size);
    cc_data_extract(codec, length_size, copy, buf->size, cc);
    free(copy);
}

/* every cut of the packet: no read beyond it, never more than the whole gives */
static void cuts(int codec, int length_size, const Buf *buf, const CcData *whole)
{
    size_t size;
    for (size = 0; size < buf->size; size++)
    {
        Buf cut = *buf;
        CcData cc;
        cut.size = size;
        extract(codec, length_size, &cut, &cc);
        assert(cc.size <= whole->size);
        assert(cc.size % 3 == 0);
    }
}

/* H.264 access unit: AUD, SEI with captions, IDR slice, an SEI after the
 * slice that is not looked at any more. */
static void h264_units(Buf units[4])
{
    static const uint8_t aud[] = { 0x09, 0xF0 };
    static const uint8_t h_sei[] = { 0x06 };
    static const uint8_t h_idr[] = { 0x65 };
    static const uint8_t late[] = { 0xFC, 0xC1, 0xC1 };
    Buf rbsp = {{0}, 0};
    int i;

    memset(units, 0, 4 * sizeof(Buf));
    add(&units[0], aud, sizeof(aud));
    sei_rbsp(&rbsp, mixed, 6, 0x40);
    nal(&units[1], h_sei, 1, &rbsp);
    add(&units[2], h_idr, 1);
    for (i = 0; i < 300; i++)
        byte(&units[2], (uint8_t)(i * 7 + 1));
    rbsp.size = 0;
    sei_rbsp(&rbsp, late, 1, 0x40);
    nal(&units[3], h_sei, 1, &rbsp);
}

static void test_h264(void)
{
    Buf units[4], buf;
    CcData cc;
    int i, length_size;

    h264_units(units);
    /* the zeros of the first message must have been escaped */
    assert(has_emulation_prevention(&units[1]));

    memset(&buf, 0, sizeof(buf));
    for (i = 0; i < 4; i++)
        annexb(&buf, &units[i]);
    extract(CC_CODEC_H264, 0, &buf, &cc);
    expect_mixed(&cc);
    cuts(CC_CODEC_H264, 0, &buf, &cc);

    for (length_size = 1; length_size <= 4; length_size++)
    {
        if (length_size == 3)
            continue;
        memset(&buf, 0, sizeof(buf));
        for (i = 0; i < 4; i++)
        {
            if (length_size == 1 && units[i].size > 255)
                units[i].size = 255;
            prefixed(&buf, &units[i], length_size);
        }
        extract(CC_CODEC_H264, length_size, &buf, &cc);
        expect_mixed(&cc);
        cuts(CC_CODEC_H264, length_size, &buf, &cc);
        h264_units(units);
    }
    printf("PASS H.264: GA94 in SEI, Annex B and length prefixed, only before the first slice\n");
}

static void test_hevc(void)
{
    static const uint8_t prefix_sei[] = { 39 << 1, 1 };
    static const uint8_t suffix_sei[] = { 40 << 1, 1 };
    static const uint8_t trail[] = { 1 << 1, 1 };
    static const uint8_t vps[] = { 32 << 1, 1, 0x0C, 0x01 };
    static const uint8_t other[] = { 0xFC, 0xC1, 0xC1 };
    Buf rbsp = {{0}, 0}, unit = {{0}, 0}, buf = {{0}, 0};
    CcData cc;

    add(&unit, vps, sizeof(vps));
    annexb(&buf, &unit);
    /* a suffix SEI belongs to the picture before */
    sei_rbsp(&rbsp, other, 1, 0x40);
    unit.size = 0;
    nal(&unit, suffix_sei, 2, &rbsp);
    annexb(&buf, &unit);
    rbsp.size = 0;
    sei_rbsp(&rbsp, mixed, 6, 0x40);
    unit.size = 0;
    nal(&unit, prefix_sei, 2, &rbsp);
    annexb(&buf, &unit);
    unit.size = 0;
    add(&unit, trail, sizeof(trail));
    byte(&unit, 0xAF);
    annexb(&buf, &unit);
    extract(CC_CODEC_HEVC, 0, &buf, &cc);
    expect_mixed(&cc);
    cuts(CC_CODEC_HEVC, 0, &buf, &cc);
    printf("PASS HEVC: GA94 in the prefix SEI, not in the suffix SEI\n");
}

static void test_mpeg2(void)
{
    static const uint8_t sequence[] = { 0, 0, 1, 0xB3, 0x04, 0x00, 0x40, 0x13, 0xFF, 0xFF, 0xE0, 0x18 };
    static const uint8_t picture[] = { 0, 0, 1, 0x00, 0x00, 0x0F, 0xFF, 0xF8 };
    static const uint8_t user[] = { 0, 0, 1, 0xB2 };
    static const uint8_t slice[] = { 0, 0, 1, 0x01, 0x12, 0x34, 0x00, 0x00, 0x01 };
    static const uint8_t second[] = { 0xFC, 0x94, 0x2F };
    static const uint8_t dvd[] = { 0x43, 0x43, 0x01, 0xF8, 0xFF, 0x80, 0x80 };
    Buf buf = {{0}, 0};
    CcData cc;

    add(&buf, sequence, sizeof(sequence));
    add(&buf, picture, sizeof(picture));
    add(&buf, user, sizeof(user));
    add(&buf, dvd, sizeof(dvd));
    add(&buf, user, sizeof(user));
    ga94(&buf, mixed, 6, 0x40);
    add(&buf, slice, sizeof(slice) - 3);
    add(&buf, slice, sizeof(slice));
    /* the second field of the frame in the same packet */
    add(&buf, picture, sizeof(picture));
    add(&buf, user, sizeof(user));
    ga94(&buf, second, 1, 0xC0);
    add(&buf, slice, sizeof(slice) - 3);
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == 9);
    assert(!memcmp(cc.data, "\xFC\xC8\x49\xFC\x80\x80\xFC\x94\x2F", 9));
    assert(cc.text);
    cuts(CC_CODEC_MPEG2, 0, &buf, &cc);
    printf("PASS MPEG-2: GA94 user data of both fields, other user data left alone\n");
}

static void test_cc_data(void)
{
    uint8_t padding[3 * 40];
    Buf buf = {{0}, 0};
    CcData cc;
    int i;

    /* padding only: data but no text */
    for (i = 0; i < 2; i++)
        memcpy(padding + 3 * i, "\xFC\x80\x80", 3);
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, padding, 2, 0x40);
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == 6 && !cc.text);

    /* process_cc_data_flag not set */
    buf.size = 0;
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, mixed, 6, 0x00);
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == 0 && !cc.text);

    /* user_data_type_code other than 3 */
    buf.size = 0;
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, mixed, 6, 0x40);
    buf.data[8] = 4;
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == 0);

    /* cc_count says more than there is */
    buf.size = 0;
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, mixed, 1, 0x40);
    buf.data[9] = 0x40 | 31;
    buf.size -= 1;
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == 3);

    /* two fields of 31 triplets: not more than CC_DATA_MAX */
    for (i = 0; i < 40; i++)
        memcpy(padding + 3 * i, "\xFC\x94\x20", 3);
    buf.size = 0;
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, padding, 31, 0x40);
    add(&buf, "\0\0\1\xB2", 4);
    ga94(&buf, padding, 31, 0x40);
    extract(CC_CODEC_MPEG2, 0, &buf, &cc);
    assert(cc.size == CC_DATA_MAX);

    /* random input */
    srand(1);
    for (i = 0; i < 3000; i++)
    {
        size_t size = (size_t)(rand() % 600);
        size_t j;
        Buf random = {{0}, 0};
        for (j = 0; j < size; j++)
        {
            int r = rand() % 16;
            byte(&random, (uint8_t)(r < 6 ? 0 : r < 8 ? 1 : r == 8 ? 3 : rand()));
        }
        extract(CC_CODEC_H264, 0, &random, &cc);
        assert(cc.size <= CC_DATA_MAX && cc.size % 3 == 0);
        extract(CC_CODEC_H264, 4, &random, &cc);
        extract(CC_CODEC_HEVC, 0, &random, &cc);
        extract(CC_CODEC_HEVC, 2, &random, &cc);
        extract(CC_CODEC_MPEG2, 0, &random, &cc);
        assert(cc.size <= CC_DATA_MAX && cc.size % 3 == 0);
    }
    printf("PASS cc_data(): padding, flags, short and long counts, random input\n");
}

static void test_length_size(void)
{
    static const uint8_t avcc[] = { 1, 0x64, 0, 0x1F, 0xFF, 0xE1, 0 };
    static const uint8_t annexb_extra[] = { 0, 0, 0, 1, 0x67, 0x64, 0 };
    uint8_t hvcc[23] = { 1 };

    assert(cc_length_size(CC_CODEC_H264, avcc, sizeof(avcc)) == 4);
    assert(cc_length_size(CC_CODEC_H264, annexb_extra, sizeof(annexb_extra)) == 0);
    assert(cc_length_size(CC_CODEC_H264, NULL, 0) == 0);
    hvcc[21] = 0x0F;
    assert(cc_length_size(CC_CODEC_HEVC, hvcc, sizeof(hvcc)) == 4);
    hvcc[21] = 0x01;
    assert(cc_length_size(CC_CODEC_HEVC, hvcc, sizeof(hvcc)) == 2);
    assert(cc_length_size(CC_CODEC_HEVC, annexb_extra, sizeof(annexb_extra)) == 0);
    assert(cc_length_size(CC_CODEC_MPEG2, avcc, sizeof(avcc)) == 0);
    printf("PASS NAL length size of avcC and hvcC\n");
}

static void test_reorder(void)
{
    /* I0 P3 B1 B2 P6 B4 B5 I9 B7 B8 in decoding order, decoded one frame
     * before the first is shown */
    static const int64_t pts[] = { 3, 6, 4, 5, 9, 7, 8, 12, 10, 11 };
    static const int64_t dts[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    CcReorder queue;
    CcEntry entry;
    CcData cc = { {0}, 3, 1 };
    int64_t last = -1;
    int popped = 0;
    int i;

    cc_reorder_clear(&queue);
    for (i = 0; i < 10; i++)
    {
        cc.data[0] = (uint8_t)pts[i];
        cc_reorder_insert(&queue, pts[i], &cc);
        while (cc_reorder_due(&queue, dts[i], CC_REORDER_MAX - 1) && cc_reorder_pop(&queue, &entry))
        {
            assert(entry.pts > last);
            assert(entry.pts <= dts[i]);
            assert(entry.cc.data[0] == entry.pts);
            last = entry.pts;
            popped++;
        }
    }
    assert(popped == 9 && queue.count == 1 && queue.entry[0].pts == 12);

    /* without a decoding time only the depth tells */
    cc_reorder_clear(&queue);
    for (i = 0; i < 10; i++)
        cc_reorder_insert(&queue, 100 - i, &cc);
    assert(cc_reorder_due(&queue, INT64_MIN, 4));
    for (i = 0; i < 6; i++)
    {
        assert(cc_reorder_pop(&queue, &entry));
        assert(entry.pts == 91 + i);
    }
    assert(!cc_reorder_due(&queue, INT64_MIN, 4));
    assert(cc_reorder_due(&queue, INT64_MIN, 3));
    while (cc_reorder_pop(&queue, &entry))
        ;
    assert(queue.count == 0 && !cc_reorder_due(&queue, INT64_MAX, 4));
    printf("PASS reorder: presentation order from the decoding time, depth without it\n");
}

int main(void)
{
    test_h264();
    test_hevc();
    test_mpeg2();
    test_cc_data();
    test_length_size();
    test_reorder();
    return 0;
}
