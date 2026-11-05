#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS _IOR('o', 57, __u64)
#endif
#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif
#define open test_open
#define usleep test_usleep
#define ioctl test_ioctl
int test_ioctl(int fd, unsigned long request, ...);
#include "../output/linuxdvb_mipsel.c"
#undef open
#undef usleep
#undef ioctl

static int opens, sleeps, failure = EBUSY, succeed_at, stop, resets;
static unsigned long requests[16];
static int request_count;

int test_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    opens++;
    errno = failure;
    return succeed_at && opens >= succeed_at ? 7 : -1;
}

int test_usleep(useconds_t delay)
{
    assert(delay == 10000);
    sleeps += delay;
    return 0;
}

int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    assert(request_count < 16);
    requests[request_count++] = request;
    return 0;
}

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stop;
}

static int reset_pcm(void) { resets++; return 0; }
stb_type_t GetSTBType(void) { return STB_VUPLUS; }
static WriterCaps_t caps = {.dvbStreamType = AUDIOTYPE_RAW};
static Writer_t pcm_writer = {.reset = reset_pcm, .caps = &caps};
Writer_t *getWriter(char *encoding)
{
    assert(!strcmp(encoding, "A_IPCM"));
    return &pcm_writer;
}

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup("A_IPCM");
    return 0;
}

int main(void)
{
    Context_t context = {0};
    Manager_t audio = {.Command = audio_command};
    ManagerHandler_t managers = {.audio = &audio};
    succeed_at = 4;
    assert(LinuxDvbOpenRetryOnBusy("test", 0) == 7);
    assert(opens == 4 && sleeps == 30000);
    opens = sleeps = succeed_at = 0;
    assert(LinuxDvbOpenRetryOnBusy("test", 0) == -1 && errno == EBUSY);
    assert(opens == 26 && sleeps == 250000);
    opens = sleeps = 0;
    failure = EACCES;
    assert(LinuxDvbOpenRetryOnBusy("test", 0) == -1 && errno == EACCES);
    assert(opens == 1 && sleeps == 0);
    failure = EBUSY;
    stop = 1;
    assert(LinuxDvbOpenRetryOnBusy("test", 0) == -1 && errno == ECANCELED);
    stop = 0;
    context.manager = &managers;
    audiofd = 1;
    assert(!LinuxDvbClear(&context, "audio"));
    assert(resets == 1 && request_count == 4);
    assert(requests[0] == AUDIO_CLEAR_BUFFER);
    assert(requests[1] == AUDIO_SET_BYPASS_MODE);
    assert(requests[2] == AUDIO_PLAY && requests[3] == AUDIO_CONTINUE);
    return 0;
}
