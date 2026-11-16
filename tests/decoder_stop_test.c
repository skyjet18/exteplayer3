#include <assert.h>
#include <fcntl.h>
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
#define open test_open
int test_open(const char *path, int flags, ...);
#define close test_close
int test_close(int fd);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef open
#undef close

/* The stop of the DVB decoders: OUTPUT_STOP and OUTPUT_CLOSE both called
 * LinuxDvbStop(), so a playback ended with "q" stopped the decoders twice.
 * The driver records the requests of each device and the close of it; every
 * case runs in a child of its own. */

#define VIDEO_FD 10
#define AUDIO_FD 11
#define CLOSED 0xc105edUL

struct request { int fd; unsigned long request; int arg; };
static struct request requests[256];
static int request_count;
static unsigned long failing_request;
static const char *audio_encoding = "A_AC3";

int test_ioctl(int fd, unsigned long request, ...)
{
    int arg = -1;
    if (request == VIDEO_SELECT_SOURCE || request == AUDIO_SELECT_SOURCE)
    {
        va_list ap;
        va_start(ap, request);
        arg = (int)va_arg(ap, long);
        va_end(ap);
    }
    assert(fd == VIDEO_FD || fd == AUDIO_FD);
    assert(request_count < 256);
    requests[request_count++] = (struct request){fd, request, arg};
    if (request == failing_request)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

int test_open(const char *path, int flags, ...)
{
    (void)flags;
    if (!strcmp(path, VIDEODEV))
        return VIDEO_FD;
    assert(!strcmp(path, AUDIODEV));
    return AUDIO_FD;
}

int test_close(int fd)
{
    assert(fd == VIDEO_FD || fd == AUDIO_FD);
    assert(request_count < 256);
    requests[request_count++] = (struct request){fd, CLOSED, -1};
    return 0;
}

stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int reset_writer(void) { return 0; }
static WriterCaps_t ac3_caps = {.name = "ac3", .dvbStreamType = AUDIOTYPE_AC3};
static WriterCaps_t pcm_caps = {.name = "pcm", .dvbStreamType = AUDIOTYPE_RAW};
static WriterCaps_t mpeg2_caps = {.name = "mpeg2", .dvbStreamType = STREAMTYPE_MPEG2};
static Writer_t ac3_writer = {.reset = reset_writer, .caps = &ac3_caps};
static Writer_t pcm_writer = {.reset = reset_writer, .caps = &pcm_caps};
static Writer_t mpeg2_writer = {.reset = reset_writer, .caps = &mpeg2_caps};
Writer_t *getWriter(char *name)
{
    if (!strcmp(name, "A_IPCM"))
        return &pcm_writer;
    if (!strcmp(name, "A_AC3"))
        return &ac3_writer;
    assert(!strcmp(name, "V_MPEG2"));
    return &mpeg2_writer;
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
    *(char **)argument = strdup(audio_encoding);
    return 0;
}

static int video_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    /* the fallback frame rate asks for the track: none here */
    if (command == MANAGER_GET_TRACK)
    {
        *(Track_t **)argument = NULL;
        return -1;
    }
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup("V_MPEG2");
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t audio = {.Command = audio_command};
static Manager_t video = {.Command = video_command};
static ManagerHandler_t managers = {.audio = &audio, .video = &video};
static Context_t context = {.playback = &playback, .manager = &managers};

/* What output.c sends for a playback with video and audio. */
static void both(OutputCmd_t command)
{
    assert(!Command(&context, command, "video"));
    assert(!Command(&context, command, "audio"));
}

static int count(int fd, unsigned long request)
{
    int i, n = 0;
    for (i = 0; i < request_count; i++)
        n += requests[i].fd == fd && requests[i].request == request;
    return n;
}

static int count_source(int fd, unsigned long request, int source)
{
    int i, n = 0;
    for (i = 0; i < request_count; i++)
        n += requests[i].fd == fd && requests[i].request == request && requests[i].arg == source;
    return n;
}

/* The device is stopped once, given back to the demux once and closed after
 * that: the last requests are the source and the close. */
static void stopped_once(int stops_before)
{
    int i, last_video = -1, last_audio = -1, before_video = -1, before_audio = -1;
    assert(count(VIDEO_FD, VIDEO_STOP) == stops_before + 1);
    assert(count(AUDIO_FD, AUDIO_STOP) == stops_before + 1);
    assert(count_source(VIDEO_FD, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX) == 1);
    assert(count_source(AUDIO_FD, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX) == 1);
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
    for (i = 0; i < request_count; i++)
    {
        if (requests[i].fd == VIDEO_FD)
        {
            before_video = last_video;
            last_video = i;
        }
        else
        {
            before_audio = last_audio;
            last_audio = i;
        }
    }
    assert(before_video >= 0 && before_audio >= 0);
    assert(requests[last_video].request == CLOSED);
    assert(requests[before_video].request == VIDEO_SELECT_SOURCE);
    assert(requests[last_audio].request == CLOSED);
    assert(requests[before_audio].request == AUDIO_SELECT_SOURCE);
    assert(videofd == -1 && audiofd == -1);
}

static void start(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    both(OUTPUT_OPEN);
    assert(videofd == VIDEO_FD && audiofd == AUDIO_FD);
    both(OUTPUT_PLAY);
}

/* "q": PlaybackStop() sends OUTPUT_STOP, main() OUTPUT_CLOSE after it. */
static void stop_then_close(void)
{
    start();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

/* The end of the file and a termination: OUTPUT_CLOSE alone stops them. */
static void close_without_stop(void)
{
    start();
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

/* Opened, never played: the close still stops them and gives the source back. */
static void close_without_play(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    both(OUTPUT_OPEN);
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

static void stop_while_paused(void)
{
    start();
    both(OUTPUT_PAUSE);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

static void stop_in_slow_motion(void)
{
    start();
    playback.SlowMotion = 2;
    both(OUTPUT_SLOWMOTION);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
    assert(count(VIDEO_FD, VIDEO_SLOWMOTION) == 2);
}

/* The switch stops and starts the audio decoder, the stop after it once more. */
static void stop_after_audio_switch(void)
{
    start();
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(count(AUDIO_FD, AUDIO_STOP) == 1);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(count(VIDEO_FD, VIDEO_STOP) == 1);
    assert(count(AUDIO_FD, AUDIO_STOP) == 2);
    assert(count_source(AUDIO_FD, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX) == 1);
    assert(count_source(VIDEO_FD, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX) == 1);
}

static void stop_after_video_switch(void)
{
    start();
    assert(!LinuxDvbSwitch(&context, "video"));
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(count(VIDEO_FD, VIDEO_STOP) == 2);
    assert(count(AUDIO_FD, AUDIO_STOP) == 1);
    assert(count_source(VIDEO_FD, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX) == 1);
}

/* A DASH switch that fails after AUDIO_STOP leaves the decoder stopped:
 * the stop gives the source back without a second AUDIO_STOP. */
static void failed_dash_switch(void)
{
    playback.isDash = 1;
    start();
    failing_request = AUDIO_CLEAR_BUFFER;
    assert(Command(&context, OUTPUT_SWITCH, "audio") < 0);
    failing_request = 0;
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

/* The PCM decoder restarted by a buffer clear (seek) is stopped once. */
static void stop_after_pcm_clear(void)
{
    audio_encoding = "A_IPCM";
    start();
    assert(!Command(&context, OUTPUT_CLEAR, "audio"));
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
}

/* Audio stopped by the mute: the close gives the source back only. */
static void close_after_mute(void)
{
    start();
    assert(!Command(&context, OUTPUT_AUDIOMUTE, "1"));
    both(OUTPUT_CLOSE);
    assert(count(AUDIO_FD, AUDIO_STOP) == 1);
    assert(count(VIDEO_FD, VIDEO_STOP) == 1);
    assert(count_source(AUDIO_FD, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX) == 1);
}

/* A second stop or close does nothing. */
static void twice(void)
{
    int requests_after;
    start();
    both(OUTPUT_STOP);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    requests_after = request_count;
    both(OUTPUT_CLOSE);
    assert(request_count == requests_after);
    stopped_once(0);
}

/* A second playback in the same process starts from the open again. */
static void reopened(void)
{
    start();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    request_count = 0;
    start();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    stopped_once(0);
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
    fprintf(stderr, "decoder_stop: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("stop, then close", stop_then_close);
    failed += run("close without a stop", close_without_stop);
    failed += run("close without a play", close_without_play);
    failed += run("stop while paused", stop_while_paused);
    failed += run("stop in slow motion", stop_in_slow_motion);
    failed += run("stop after an audio switch", stop_after_audio_switch);
    failed += run("stop after a video switch", stop_after_video_switch);
    failed += run("stop after a failed DASH switch", failed_dash_switch);
    failed += run("stop after a PCM buffer clear", stop_after_pcm_clear);
    failed += run("close after the audio mute", close_after_mute);
    failed += run("stop and close twice", twice);
    failed += run("second playback", reopened);
    return failed ? 1 : 0;
}
