#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../output/writer/mipsel/dts_frame.h"

/* The frame helpers of the DTS writer: which packets are padded to a multiple
 * of four bytes and what the padded frame looks like. Every buffer has the
 * exact size of the packet or of the padded frame, so ASan sees a byte read
 * or written beyond it. */

#define X96_SYNC "\x1d\x95\xf2\x62"
#define XCH_SYNC "\x5a\x5a\x5a\x5a"

static size_t core_size(const uint8_t *frame)
{
    return (((size_t)frame[5] & 3) << 12 | (size_t)frame[6] << 4 | frame[7] >> 4) + 1;
}

static size_t x96_size(const uint8_t *extension)
{
    return ((size_t)extension[4] << 4 | extension[5] >> 4) + 1;
}

static size_t xch_size(const uint8_t *extension)
{
    return ((size_t)extension[4] << 2 | extension[5] >> 6) + 1;
}

static void set_x96_size(uint8_t *extension, size_t size)
{
    extension[4] = (uint8_t)((size - 1) >> 4);
    extension[5] = (uint8_t)((extension[5] & 0x0f) | ((size - 1) & 0x0f) << 4);
}

static void set_xch_size(uint8_t *extension, size_t size)
{
    extension[4] = (uint8_t)((size - 1) >> 2);
    extension[5] = (uint8_t)((extension[5] & 0x3f) | ((size - 1) & 3) << 6);
}

/* A 16 bit big endian core frame of "size" bytes with FSIZE "fsize" and
 * "blocks" blocks of 32 samples: 3/2 channels, 48 kHz, no CRC, no extension.
 * The payload counts up in steps of 7, so a moved or changed byte shows. */
static uint8_t *core_frame(size_t size, size_t fsize, unsigned int blocks)
{
    uint8_t *frame = malloc(size);
    size_t i, field = fsize - 1;
    assert(frame && size >= 16 && blocks >= 1 && blocks <= 128);
    for (i = 0; i < size; i++)
        frame[i] = (uint8_t)(i * 7 + 1);
    memcpy(frame, "\x7f\xfe\x80\x01", 4);
    frame[4] = (uint8_t)(0xfc | (blocks - 1) >> 6);
    frame[5] = (uint8_t)(((blocks - 1) & 0x3f) << 2 | (field >> 12 & 3));
    frame[6] = (uint8_t)(field >> 4);
    frame[7] = (uint8_t)((field & 0x0f) << 4 | 0x02);
    frame[8] = 0x77;
    frame[9] = 0x00;
    frame[10] = 0x00;
    assert(core_size(frame) == fsize);
    return frame;
}

/* 2013 bytes, 512 samples: a DVD style full rate frame. */
static uint8_t *dvd_frame(void)
{
    return core_frame(2013, 2013, 16);
}

static void assert_untouched(uint8_t *frame, size_t size)
{
    assert(dts_core_aligned_size(frame, size) == 0);
    free(frame);
}

/* Pads "frame" and checks the header (FSIZE raised, its other bits kept) and
 * the zero bytes after the frame; the case checks the bytes in between. */
static uint8_t *aligned(const uint8_t *frame, size_t size, size_t expected)
{
    uint8_t *output;
    size_t i;
    assert(dts_core_aligned_size(frame, size) == expected);
    output = malloc(expected);
    assert(output);
    memset(output, 0xaa, expected);
    dts_core_align(output, expected, frame, size);
    assert(!memcmp(output, frame, 5));
    assert(core_size(output) == expected);
    assert(output[5] >> 2 == frame[5] >> 2 && (output[7] & 0x0f) == (frame[7] & 0x0f));
    for (i = size; i < expected; i++)
        assert(output[i] == 0);
    return output;
}

static void dvd_frame_padded(void)
{
    uint8_t *frame = dvd_frame();
    uint8_t *output = aligned(frame, 2013, 2016);
    assert(!memcmp(output + 8, frame + 8, 2013 - 8));
    free(output);
    free(frame);
}

static void aligned_frames_untouched(void)
{
    assert_untouched(core_frame(2012, 2012, 16), 2012);
    assert_untouched(core_frame(2016, 2016, 16), 2016);
}

static void crc_frame_untouched(void)
{
    uint8_t *frame = dvd_frame();
    frame[4] |= 0x02;
    assert_untouched(frame, 2013);
}

/* The same 2013 bytes as 14 bit and as little endian streams. */
static void other_formats_untouched(void)
{
    static const char *syncs[] = {"\x1f\xff\xe8\x00", "\xfe\x7f\x01\x80", "\xff\x1f\x00\xe8"};
    size_t i, j;
    for (i = 0; i < 3; i++)
    {
        uint8_t *frame = dvd_frame();
        memcpy(frame, syncs[i], 4);
        if (i)
        {
            for (j = 4; j + 1 < 2013; j += 2)
            {
                uint8_t byte = frame[j];
                frame[j] = frame[j + 1];
                frame[j + 1] = byte;
            }
        }
        assert_untouched(frame, 2013);
    }
}

/* A core of 2013 bytes followed by a DTS-HD extension substream, and the
 * substream alone: FSIZE is not the packet size. */
static void dts_hd_untouched(void)
{
    uint8_t *frame = core_frame(2013 + 16, 2013, 16);
    memcpy(frame + 2013, "\x64\x58\x20\x25", 4);
    assert_untouched(frame, 2013 + 16);
    frame = dvd_frame();
    memcpy(frame, "\x64\x58\x20\x25", 4);
    assert_untouched(frame, 2013);
}

/* A packet shorter or longer than its FSIZE, and buffers too short or missing. */
static void fsize_not_the_packet_untouched(void)
{
    assert_untouched(core_frame(2013, 2009, 16), 2013);
    assert_untouched(core_frame(2009, 2013, 16), 2009);
    assert_untouched(core_frame(16, 13, 1), 13);
    assert(dts_core_aligned_size(NULL, 2013) == 0);
}

/* The IEC 61937 burst of 512 samples has 2048 bytes, 8 of them its header:
 * a frame is padded to at most 2040 bytes. With 1024 samples it fits. */
static void room_in_the_burst(void)
{
    uint8_t *frame = core_frame(2037, 2037, 16);
    free(aligned(frame, 2037, 2040));
    free(frame);
    assert_untouched(core_frame(2041, 2041, 16), 2041);
    assert_untouched(core_frame(2045, 2045, 16), 2045);
    frame = core_frame(2045, 2045, 32);
    free(aligned(frame, 2045, 2048));
    free(frame);
}

/* DTS 96/24: the X96 size field is raised with FSIZE. A sync word whose size
 * field does not reach the end of the frame is no extension and stays. */
static void x96_size_raised(void)
{
    uint8_t *frame = dvd_frame();
    uint8_t *output;
    frame[10] = 0x50; /* EXT_AUDIO_ID 2, EXT_AUDIO */
    memcpy(frame + 800, X96_SYNC, 4);
    set_x96_size(frame + 800, 100);
    memcpy(frame + 1600, X96_SYNC, 4);
    set_x96_size(frame + 1600, 2013 - 1600);
    output = aligned(frame, 2013, 2016);
    assert(!memcmp(output + 8, frame + 8, 1604 - 8));
    assert(x96_size(output + 1600) == 2016 - 1600);
    assert((output[1605] & 0x0f) == (frame[1605] & 0x0f));
    assert(!memcmp(output + 1606, frame + 1606, 2013 - 1606));
    free(output);
    free(frame);
}

/* DTS-ES: the XCH size field is raised; an X96 sync in an XCH frame is no
 * extension of it. */
static void xch_size_raised(void)
{
    uint8_t *frame = dvd_frame();
    uint8_t *output;
    frame[10] = 0x10; /* EXT_AUDIO_ID 0, EXT_AUDIO */
    memcpy(frame + 1600, X96_SYNC, 4);
    set_x96_size(frame + 1600, 2013 - 1600);
    memcpy(frame + 1800, XCH_SYNC, 4);
    set_xch_size(frame + 1800, 2013 - 1800);
    output = aligned(frame, 2013, 2016);
    assert(!memcmp(output + 8, frame + 8, 1804 - 8));
    assert(xch_size(output + 1800) == 2016 - 1800);
    assert((output[1805] & 0x3f) == (frame[1805] & 0x3f));
    assert(!memcmp(output + 1806, frame + 1806, 2013 - 1806));
    free(output);
    free(frame);
}

/* No extension flag: a sync word with a matching size field stays as it is. */
static void extension_needs_its_flag(void)
{
    uint8_t *frame = dvd_frame();
    uint8_t *output;
    memcpy(frame + 1600, X96_SYNC, 4);
    set_x96_size(frame + 1600, 2013 - 1600);
    output = aligned(frame, 2013, 2016);
    assert(!memcmp(output + 8, frame + 8, 2013 - 8));
    free(output);
    free(frame);
}

int main(void)
{
    dvd_frame_padded();
    aligned_frames_untouched();
    crc_frame_untouched();
    other_formats_untouched();
    dts_hd_untouched();
    fsize_not_the_packet_untouched();
    room_in_the_burst();
    x96_size_raised();
    xch_size_raised();
    extension_needs_its_flag();
    return 0;
}
