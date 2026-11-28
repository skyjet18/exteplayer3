#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../output/writer/mipsel/dts.c"
/* PutBits() for the PES header; the box type is the one of the case. */
#define GetSTBType misc_GetSTBType
#include "../output/writer/common/misc.c"
#undef GetSTBType

/* The DTS writer with a DVD style 2013-byte core frame: on every box but a
 * Dreambox, or with EP3_DTS_PAD=1, the PES carries the frame padded to 2016 bytes with
 * FSIZE raised. The choice is taken once per process, so every case runs in a
 * child of its own. */

static stb_type_t stb = STB_OTHER;
stb_type_t GetSTBType(void) { return stb; }

static uint8_t header[64], payload[4096];
static size_t header_size, payload_size;
static const void *payload_base;
static int calls;

static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    assert(fd == 1 && count == 2 && !calls++);
    assert(iov[0].iov_len <= sizeof(header) && iov[1].iov_len <= sizeof(payload));
    memcpy(header, iov[0].iov_base, iov[0].iov_len);
    memcpy(payload, iov[1].iov_base, iov[1].iov_len);
    header_size = iov[0].iov_len;
    payload_size = iov[1].iov_len;
    payload_base = iov[1].iov_base;
    return (ssize_t)(header_size + payload_size);
}

/* 16 bit big endian, 512 samples, no CRC, FSIZE = size; the payload counts up. */
static uint8_t *core_frame(size_t size)
{
    uint8_t *frame;
    size_t i, field;
    assert(size >= 16); /* the header below writes bytes 0..10 */
    frame = malloc(size);
    field = size - 1;
    assert(frame);
    for (i = 0; i < size; i++)
        frame[i] = (uint8_t)(i * 7 + 1);
    memcpy(frame, "\x7f\xfe\x80\x01\xfc\x3c", 6);
    frame[5] |= (uint8_t)(field >> 12 & 3);
    frame[6] = (uint8_t)(field >> 4);
    frame[7] = (uint8_t)((field & 0x0f) << 4 | 0x02);
    frame[10] = 0x00;
    return frame;
}

static void write_frame(uint8_t *frame, size_t size)
{
    WriterAVCallData_t call = {.fd = 1, .data = frame, .len = size, .Pts = 90000,
                               .WriteV = capture};
    int32_t written;
    calls = 0;
    written = WriterAudioDTS.writeData(&call);
    assert(calls == 1 && header_size == 14 && written == (int32_t)(14 + payload_size));
    /* PES_packet_length: the payload, 3 flag bytes and the PTS */
    assert(!memcmp(header, "\0\0\1\xc0", 4));
    assert(((size_t)header[4] << 8 | header[5]) == payload_size + 8);
}

static void padded(void)
{
    uint8_t *frame = core_frame(2013);
    write_frame(frame, 2013);
    assert(payload_size == 2016 && payload_base != frame);
    /* FSIZE 2016 - 1, everything else as in the packet, three zero bytes */
    assert(!memcmp(payload, frame, 6) && payload[6] == 0x7d && payload[7] == 0xf2);
    assert(!memcmp(payload + 8, frame + 8, 2013 - 8));
    assert(!payload[2013] && !payload[2014] && !payload[2015]);
    /* An aligned frame and a core with a DTS-HD substream go out as they are. */
    free(frame);
    frame = core_frame(2012);
    write_frame(frame, 2012);
    assert(payload_size == 2012 && payload_base == frame);
    free(frame);
    frame = core_frame(2013 + 16);
    frame[6] = 0x7d;
    frame[7] = 0xc2;
    memcpy(frame + 2013, "\x64\x58\x20\x25", 4);
    write_frame(frame, 2013 + 16);
    assert(payload_size == 2013 + 16 && payload_base == frame);
    free(frame);
}

static void unchanged(void)
{
    uint8_t *frame = core_frame(2013);
    write_frame(frame, 2013);
    assert(payload_size == 2013 && payload_base == frame);
    assert(!memcmp(payload, frame, 2013));
    free(frame);
}

static void vuplus(void) { stb = STB_VUPLUS; padded(); }
static void hisilicon(void) { stb = STB_HISILICON; padded(); }
static void other(void) { padded(); }
static void dreambox(void) { stb = STB_DREAMBOX; unchanged(); }
static void forced_off(void) { stb = STB_VUPLUS; assert(!setenv("EP3_DTS_PAD", "0", 1)); unchanged(); }
static void forced_on(void) { stb = STB_DREAMBOX; assert(!setenv("EP3_DTS_PAD", "1", 1)); padded(); }
static void other_value(void) { assert(!setenv("EP3_DTS_PAD", "10", 1)); padded(); }
static void other_value_dreambox(void) { stb = STB_DREAMBOX; assert(!setenv("EP3_DTS_PAD", "10", 1)); unchanged(); }
static void forced_off_other(void) { assert(!setenv("EP3_DTS_PAD", "0", 1)); unchanged(); }

static int run(const char *name, void (*test)(void))
{
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        test();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && !WEXITSTATUS(status))
        return 0;
    fprintf(stderr, "dts_pad: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_DTS_PAD");
    failed += run("Vu+", vuplus);
    failed += run("HiSilicon", hisilicon);
    failed += run("other box", other);
    failed += run("Dreambox", dreambox);
    failed += run("EP3_DTS_PAD=0 on Vu+", forced_off);
    failed += run("EP3_DTS_PAD=0 on another box", forced_off_other);
    failed += run("EP3_DTS_PAD=1 on a Dreambox", forced_on);
    failed += run("EP3_DTS_PAD=10 on another box", other_value);
    failed += run("EP3_DTS_PAD=10 on a Dreambox", other_value_dreambox);
    return failed ? 1 : 0;
}
