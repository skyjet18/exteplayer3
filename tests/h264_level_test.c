#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

/* The SPS level limit of the H.264 writer as a MIPS build has it: run.sh
 * builds this program with -DH264_LEVEL41_DEFAULT=1. The host build of the
 * writer keeps the level (tests/output_test.c). The setting is read once per
 * process, so every case runs in a child of its own. */

static uint8_t written[4096];
static size_t written_size;

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    int i;
    (void)fd;
    written_size = 0;
    for (i = 0; i < count; i++)
    {
        assert(written_size + iov[i].iov_len <= sizeof(written));
        memcpy(written + written_size, iov[i].iov_base, iov[i].iov_len);
        written_size += iov[i].iov_len;
    }
    return written_size;
}

/* Writes one frame with avcC codec data of two SPS (profile, level) and two
 * PPS, checks that both SPS reach the decoder with the expected level and
 * that the codec data of the caller is not changed. */
static void write_level(uint8_t profile, uint8_t level, uint8_t expected)
{
    uint8_t avcc[] = {1, profile, 0, level, 255, 226, 0, 4, 103, profile, 0, level,
                      0, 4, 103, profile, 0, level, 2, 0, 2, 104, 1, 0, 2, 104, 2};
    uint8_t frame[] = {0, 0, 0, 2, 101, 136};
    WriterAVCallData_t call = {.fd = 1, .data = frame, .len = sizeof(frame), .Pts = 90000,
                               .Dts = 86400, .private_data = avcc,
                               .private_size = sizeof(avcc), .WriteV = capture};
    size_t i;
    int sets = 0;
    WriterVideoH264.reset();
    assert(WriterVideoH264.writeData(&call) > 0);
    for (i = 0; i + 8 <= written_size; i++)
    {
        if (!memcmp(written + i, "\0\0\0\1\x67", 5))
        {
            assert(written[i + 5] == profile && written[i + 7] == expected);
            sets++;
        }
    }
    assert(sets == 2);
    assert(avcc[3] == level && avcc[11] == level && avcc[17] == level);
    WriterVideoH264.reset();
}

static void limited(void) { write_level(100, 42, 41); }
static void limited_main(void) { write_level(77, 51, 41); }
static void lower_level_kept(void) { write_level(100, 40, 40); }
static void other_profile_kept(void) { write_level(110, 42, 42); }
static void empty_keeps_default(void) { assert(!setenv("EP3_H264_LEVEL41", "", 1)); write_level(100, 42, 41); }
static void forced_off(void) { assert(!setenv("EP3_H264_LEVEL41", "0", 1)); write_level(100, 42, 42); }
static void forced_on(void) { assert(!setenv("EP3_H264_LEVEL41", "1", 1)); write_level(100, 42, 41); }

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
    fprintf(stderr, "h264_level: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_H264_LEVEL41");
    failed += run("high profile level 4.2 written as 4.1", limited);
    failed += run("main profile level 5.1 written as 4.1", limited_main);
    failed += run("level 4.0 kept", lower_level_kept);
    failed += run("high 10 profile kept", other_profile_kept);
    failed += run("empty EP3_H264_LEVEL41 keeps the build default", empty_keeps_default);
    failed += run("EP3_H264_LEVEL41=0", forced_off);
    failed += run("EP3_H264_LEVEL41=1", forced_on);
    return failed ? 1 : 0;
}
