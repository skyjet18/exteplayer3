#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
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
#include "../output/linuxdvb_mipsel.c"
#undef ioctl

/* The decoder restarts of the DVB output: the audio restart behind a buffer
 * clear and the video restart of OUTPUT_SWITCH "video". The driver records
 * its requests and rejects the one the case names. Every case runs in a
 * child of its own, so that one failure does not hide the others. */

static unsigned long requests[16];
static int request_count;
static unsigned long failing_request;
static int audio_resets, video_resets;

int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    assert(request_count < 16);
    requests[request_count++] = request;
    if (request == failing_request) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
stb_type_t GetSTBType(void) { return STB_VUPLUS; }

static int reset_audio(void) { audio_resets++; return 0; }
static int reset_video(void) { video_resets++; return 0; }
static WriterCaps_t pcm_caps = {.name = "pcm", .dvbStreamType = AUDIOTYPE_RAW};
static WriterCaps_t mpeg2_caps = {.name = "mpeg2", .dvbStreamType = STREAMTYPE_MPEG2};
static Writer_t pcm_writer = {.reset = reset_audio, .caps = &pcm_caps};
static Writer_t mpeg2_writer = {.reset = reset_video, .caps = &mpeg2_caps};
Writer_t *getWriter(char *encoding)
{
    if (!strcmp(encoding, "A_IPCM"))
        return &pcm_writer;
    assert(!strcmp(encoding, "V_MPEG2"));
    return &mpeg2_writer;
}

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
void WriterWakeUp(void) {}
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup("A_IPCM");
    return 0;
}

static int video_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup("V_MPEG2");
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t audio = {.Command = audio_command};
static Manager_t video = {.Command = video_command};
static ManagerHandler_t managers = {.audio = &audio, .video = &video};
static Context_t context = {.playback = &playback, .manager = &managers};

static void start(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    audiofd = 1;
    videofd = 2;
}

/* The buffer is cleared; the PCM decoder that still runs rejects AUDIO_PLAY. */
static void clear_with_play_rejected(void)
{
    start();
    failing_request = AUDIO_PLAY;
    assert(!LinuxDvbClear(&context, "audio"));
    assert(request_count == 4 && requests[0] == AUDIO_CLEAR_BUFFER);
    assert(requests[1] == AUDIO_SET_BYPASS_MODE && requests[2] == AUDIO_PLAY);
    assert(requests[3] == AUDIO_CONTINUE && audio_resets == 1);
}

static void clear_with_continue_rejected(void)
{
    start();
    failing_request = AUDIO_CONTINUE;
    assert(!LinuxDvbClear(&context, "audio"));
    assert(request_count == 4 && requests[3] == AUDIO_CONTINUE);
}

/* The clear itself still reports its failure. */
static void clear_rejected(void)
{
    start();
    failing_request = AUDIO_CLEAR_BUFFER;
    assert(LinuxDvbClear(&context, "audio") < 0);
}

/* The restarted video decoder gets the codec data again: the writer is reset
 * before VIDEO_SET_STREAMTYPE, the audio writer is left alone. */
static void video_restart_resets_the_writer(void)
{
    start();
    assert(!LinuxDvbSwitch(&context, "video"));
    assert(video_resets == 1 && !audio_resets);
    assert(request_count == 4 && requests[0] == VIDEO_STOP && requests[1] == VIDEO_CLEAR_BUFFER);
    assert(requests[2] == VIDEO_SET_STREAMTYPE && requests[3] == VIDEO_PLAY);
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
    fprintf(stderr, "dvb_restart: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("clear, AUDIO_PLAY rejected", clear_with_play_rejected);
    failed += run("clear, AUDIO_CONTINUE rejected", clear_with_continue_rejected);
    failed += run("clear, AUDIO_CLEAR_BUFFER rejected", clear_rejected);
    failed += run("video restart resets the writer", video_restart_resets_the_writer);
    return failed ? 1 : 0;
}
