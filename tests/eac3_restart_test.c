#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/types.h>
#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS _IOR('o', 57, __u64)
#endif
#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif
#define ioctl test_ioctl
int test_ioctl(int fd, unsigned long request, ...);
#define fopen test_fopen
FILE *test_fopen(const char *path, const char *mode);
#define clock_gettime test_clock_gettime
int test_clock_gettime(clockid_t clock, struct timespec *now);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef fopen
#undef clock_gettime

/* The DD+ restart after the first passthrough start on VU+ Ultimo 4K / Duo 4K SE:
 * the box is described by GetSTBType() and the two /proc files, time by a clock
 * that moves only when the test moves it. Every case runs in a child of its own,
 * as the arming is decided once per process. */

static stb_type_t stb = STB_VUPLUS;
static const char *model = "ultimo4k\n";
static const char *mode = "passthrough\n";
static const char *encoding = "A_EAC3";
static long clock_ms;
static int writes;

int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    return 0;
}

FILE *test_fopen(const char *path, const char *how)
{
    const char *text = NULL;
    if (!strcmp(path, "/proc/stb/info/vumodel"))
        text = model;
    else if (!strcmp(path, "/proc/stb/audio/ac3"))
        text = mode;
    if (!text)
        return NULL;
    return fmemopen((void *)text, strlen(text), how);
}

int test_clock_gettime(clockid_t clock, struct timespec *now)
{
    assert(clock == CLOCK_MONOTONIC);
    now->tv_sec = clock_ms / 1000;
    now->tv_nsec = (clock_ms % 1000) * 1000000;
    return 0;
}

stb_type_t GetSTBType(void) { return stb; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int write_packet(void *call) { (void)call; writes++; return 100; }
static int reset_writer(void) { return 0; }
static WriterCaps_t eac3_caps = {.dvbStreamType = AUDIOTYPE_AC3_PLUS};
static WriterCaps_t ac3_caps = {.dvbStreamType = AUDIOTYPE_AC3};
static Writer_t eac3_writer = {.reset = reset_writer, .writeData = write_packet, .caps = &eac3_caps};
static Writer_t ac3_writer = {.reset = reset_writer, .writeData = write_packet, .caps = &ac3_caps};
Writer_t *getWriter(char *name)
{
    return !strcmp(name, "A_EAC3") ? &eac3_writer : &ac3_writer;
}
Writer_t *getDefaultVideoWriter(void) { return NULL; }
Writer_t *getDefaultAudioWriter(void) { return NULL; }
ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
int32_t LinuxDvbBuffOpen(Context_t *context, char *type, int outfd, void *mtx)
{
    (void)context;
    (void)type;
    (void)outfd;
    (void)mtx;
    return 0;
}
int32_t LinuxDvbBuffClose(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlush(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlushAudio(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffResume(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup(encoding);
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t audio = {.Command = audio_command};
static ManagerHandler_t managers = {.audio = &audio};
static Context_t context = {.playback = &playback, .manager = &managers};

static void start(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    audiofd = 1;
    assert(!Command(&context, OUTPUT_PLAY, "audio"));
}

/* Armed: fast polling until the first DD+ packet and 100 ms after it, then the
 * restart is due exactly once. A second start of the file does not arm it again. */
static void armed(void)
{
    uint8_t data[4] = {0x0b, 0x77};
    AudioVideoOut_t out = {.data = data, .len = sizeof(data), .type = "audio"};
    clock_ms = 1000;
    start();
    assert(LinuxDvbEac3RestartState() == 1);
    clock_ms += 300;
    assert(LinuxDvbEac3RestartState() == 1);
    assert(!LinuxDvbOutput.Write(&context, &out) && writes == 1);
    clock_ms += 99;
    assert(LinuxDvbEac3RestartState() == 1);
    clock_ms += 1;
    assert(LinuxDvbEac3RestartState() == 2);
    assert(LinuxDvbEac3RestartState() == 0);
    assert(!LinuxDvbOutput.Write(&context, &out));
    clock_ms += 200;
    assert(LinuxDvbEac3RestartState() == 0);
    assert(!Command(&context, OUTPUT_PLAY, "audio"));
    assert(!LinuxDvbOutput.Write(&context, &out));
    clock_ms += 200;
    assert(LinuxDvbEac3RestartState() == 0);
}

static void duo4kse(void)
{
    model = "duo4kse\n";
    armed();
}

/* Not armed: nothing is ever due. */
static void never_due(void)
{
    uint8_t data[4] = {0x0b, 0x77};
    AudioVideoOut_t out = {.data = data, .len = sizeof(data), .type = "audio"};
    start();
    assert(LinuxDvbEac3RestartState() == 0);
    assert(!LinuxDvbOutput.Write(&context, &out) && writes == 1);
    clock_ms += 100;
    assert(LinuxDvbEac3RestartState() == 0);
    clock_ms += 5000;
    assert(LinuxDvbEac3RestartState() == 0);
}

static void other_model(void) { model = "uno4kse\n"; never_due(); }
static void not_passthrough(void) { mode = "downmix\n"; never_due(); }
static void not_a_vuplus(void) { stb = STB_OTHER; never_due(); }
static void plain_ac3(void) { encoding = "A_AC3"; never_due(); }
static void dash(void) { playback.isDash = 1; never_due(); }
static void forced_off(void) { assert(!setenv("EP3_EAC3_RESTART", "0", 1)); never_due(); }

static void forced_on(void)
{
    model = "uno4kse\n";
    mode = "downmix\n";
    assert(!setenv("EP3_EAC3_RESTART", "1", 1));
    armed();
}

/* A switch is the restart itself, a stop ends the playback: both cancel it. */
static void cancelled_by_switch(void)
{
    start();
    assert(LinuxDvbEac3RestartState() == 1);
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    clock_ms += 1000;
    assert(LinuxDvbEac3RestartState() == 0);
}

static void cancelled_by_stop(void)
{
    uint8_t data[4] = {0x0b, 0x77};
    AudioVideoOut_t out = {.data = data, .len = sizeof(data), .type = "audio"};
    start();
    assert(!LinuxDvbOutput.Write(&context, &out));
    assert(!Command(&context, OUTPUT_STOP, "audio"));
    clock_ms += 1000;
    assert(LinuxDvbEac3RestartState() == 0);
}

/* No fast polling for ever when the first packet is late; it is due all the same. */
static void late_first_packet(void)
{
    uint8_t data[4] = {0x0b, 0x77};
    AudioVideoOut_t out = {.data = data, .len = sizeof(data), .type = "audio"};
    start();
    clock_ms += 5000;
    assert(LinuxDvbEac3RestartState() == 0);
    assert(!LinuxDvbOutput.Write(&context, &out));
    assert(LinuxDvbEac3RestartState() == 1);
    clock_ms += 100;
    assert(LinuxDvbEac3RestartState() == 2);
}

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
    fprintf(stderr, "eac3_restart: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_EAC3_RESTART");
    failed += run("armed on ultimo4k", armed);
    failed += run("armed on duo4kse", duo4kse);
    failed += run("other model", other_model);
    failed += run("no passthrough", not_passthrough);
    failed += run("not a VU+", not_a_vuplus);
    failed += run("AC-3 writer", plain_ac3);
    failed += run("DASH", dash);
    failed += run("EP3_EAC3_RESTART=0", forced_off);
    failed += run("EP3_EAC3_RESTART=1", forced_on);
    failed += run("cancelled by a switch", cancelled_by_switch);
    failed += run("cancelled by a stop", cancelled_by_stop);
    failed += run("late first packet", late_first_packet);
    return failed ? 1 : 0;
}
