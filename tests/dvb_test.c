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
#define printf test_printf
int test_printf(const char *format, ...);
#include "../output/linuxdvb_mipsel.c"
#undef printf
#undef open
#undef usleep
#undef ioctl

static int opens, sleeps, failure = EBUSY, succeed_at, stop, resets;
static unsigned long requests[64];
static int arguments[64];
static int request_count;
static unsigned long failing_request;
static const char *encoding = "A_IPCM";
static int audio_flushes;

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
    va_list args;
    assert(request_count < 64);
    requests[request_count] = request;
    va_start(args, request);
    if (request == AUDIO_SET_AV_SYNC || request == AUDIO_SET_BYPASS_MODE ||
        request == VIDEO_SET_STREAMTYPE)
        arguments[request_count] = va_arg(args, int);
    va_end(args);
    request_count++;
    if (request == failing_request) {
        errno = EIO;
        return -1;
    }
    if (request == VIDEO_GET_PTS || request == AUDIO_GET_PTS) {
        errno = ENOTTY;
        return -1;
    }
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
    assert(!strcmp(encoding, "A_IPCM") || !strcmp(encoding, "A_AAC"));
    return &pcm_writer;
}

int32_t LinuxDvbBuffFlushAudio(Context_t *context)
{
    assert(context->playback->isAudioSwitching || context->playback->isSeeking);
    audio_flushes++;
    return 0;
}

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup(encoding);
    return 0;
}

/* Output follow-up: results of a switch, pause over a switch, reset per type. */

/* Command() refers to the whole buffering interface. */
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
int32_t LinuxDvbBuffResume(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static void switch_results_and_reset(Context_t *context, PlaybackHandler_t *playback,
                                     ManagerHandler_t *managers)
{
    playback->isDash = 0;
    failing_request = AUDIO_SET_BYPASS_MODE;
    request_count = 0;
    /* Only logged outside DASH: the track is switched, so the switch has not failed. */
    assert(!LinuxDvbSwitch(context, "audio"));
    assert(request_count == 5 && requests[2] == AUDIO_SET_BYPASS_MODE);
    assert(requests[3] == AUDIO_SET_BYPASS_MODE && requests[4] == AUDIO_PLAY);
    failing_request = 0;
    playback->isPaused = 1;
    request_count = 0;
    assert(!LinuxDvbSwitch(context, "audio"));
    assert(request_count == 5 && requests[3] == AUDIO_PLAY && requests[4] == AUDIO_PAUSE);
    playback->isPaused = 0;
    /* The writer stub stands for the video writer as well. */
    managers->video = managers->audio;
    failing_request = VIDEO_SET_STREAMTYPE;
    request_count = 0;
    assert(!LinuxDvbSwitch(context, "video"));
    assert(request_count == 5 && requests[0] == VIDEO_STOP && requests[1] == VIDEO_CLEAR_BUFFER);
    assert(requests[3] == VIDEO_SET_STREAMTYPE && requests[4] == VIDEO_PLAY);
    failing_request = 0;
    resets = 0;
    assert(!Command(context, OUTPUT_FLUSH, "audio") && resets == 1);
    assert(!Command(context, OUTPUT_FLUSH, "video") && resets == 2);
    managers->video = NULL;
    playback->isDash = 1;
}

/* Output follow-up: messages of the position query, a writer without reset(). */

static const char *counted;
static int messages;

/* The messages of linuxdvb_mipsel.c. */
int test_printf(const char *format, ...)
{
    va_list arguments;
    int result;
    if (counted && strstr(format, counted))
        messages++;
    va_start(arguments, format);
    result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

/* Audio only: the video device is not asked, so no error is reported for it. */
static void no_message_for_the_closed_video_device(Context_t *context)
{
    unsigned long long position = 1;
    int video = videofd;
    videofd = -1;
    LINUXDVB_SILENT = 0;
    counted = "VIDEO_GET_PTS";
    request_count = 0;
    assert(LinuxDvbPts(context, &position) < 0 && !position);
    assert(request_count == 1 && requests[0] == AUDIO_GET_PTS && !messages);
    counted = NULL;
    LINUXDVB_SILENT = 1;
    videofd = video;
}

static void writer_without_reset(Context_t *context, ManagerHandler_t *managers)
{
    managers->video = managers->audio;
    pcm_writer.reset = NULL;
    assert(!Command(context, OUTPUT_FLUSH, "audio"));
    pcm_writer.reset = reset_pcm;
    managers->video = NULL;
}

int main(void)
{
    Context_t context = {0};
    PlaybackHandler_t playback = {.AVSync = 1};
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
    context.playback = &playback;
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    audiofd = 1;
    assert(!LinuxDvbClear(&context, "audio"));
    assert(resets == 1 && request_count == 4);
    assert(requests[0] == AUDIO_CLEAR_BUFFER);
    assert(requests[1] == AUDIO_SET_BYPASS_MODE);
    assert(requests[2] == AUDIO_PLAY && requests[3] == AUDIO_CONTINUE);
    request_count = 0;
    assert(!LinuxDvbAVSync(&context, "audio"));
    assert(request_count == 1 && requests[0] == AUDIO_SET_AV_SYNC && arguments[0] == 0);
    playback.isDash = 1;
    request_count = 0;
    assert(!LinuxDvbAVSync(&context, "audio"));
    assert(request_count == 1 && arguments[0] == 1);
    playback.AVSync = 0;
    request_count = 0;
    assert(!LinuxDvbAVSync(&context, "audio") && arguments[0] == 0);
    playback.AVSync = 1;
    encoding = "A_AAC";
    caps.dvbStreamType = AUDIOTYPE_AAC_PLUS;
    request_count = 0;
    playback.isAudioSwitching = 1;
    isBufferedOutput = true;
    videofd = 2;
    assert(!LinuxDvbSwitch(&context, "audio"));
    assert(audio_flushes == 1 && resets == 2 && request_count == 5);
    assert(requests[0] == AUDIO_STOP && requests[1] == AUDIO_CLEAR_BUFFER);
    assert(requests[2] == AUDIO_SET_BYPASS_MODE);
    assert(requests[3] == AUDIO_SET_AV_SYNC && arguments[3] == 1);
    assert(requests[4] == AUDIO_PLAY);
    request_count = 0;
    assert(!LinuxDvbPlay(&context, "audio"));
    assert(request_count == 4 && requests[0] == AUDIO_SET_BYPASS_MODE);
    assert(requests[1] == AUDIO_SET_AV_SYNC && arguments[1] == 1);
    assert(requests[2] == AUDIO_PLAY && requests[3] == AUDIO_CONTINUE);
    failing_request = AUDIO_SET_AV_SYNC;
    request_count = 0;
    assert(LinuxDvbAVSync(&context, "audio") < 0 && applied_avsync == -1);
    request_count = 0;
    assert(LinuxDvbSwitch(&context, "audio") < 0);
    assert(request_count == 4 && requests[3] == AUDIO_SET_AV_SYNC);
    request_count = 0;
    assert(LinuxDvbPlay(&context, "audio") < 0);
    assert(request_count == 2 && requests[1] == AUDIO_SET_AV_SYNC);
    failing_request = AUDIO_STOP;
    request_count = 0;
    assert(LinuxDvbSwitch(&context, "audio") < 0 && request_count == 1);
    failing_request = 0;
    playback.isDash = playback.isAudioSwitching = 0;
    request_count = 0;
    assert(!LinuxDvbSwitch(&context, "audio"));
    assert(request_count == 4 && requests[3] == AUDIO_PLAY);
    playback.isDash = playback.isSeeking = 1;
    request_count = 0;
    assert(!LinuxDvbSwitch(&context, "audio"));
    assert(request_count == 5 && requests[3] == AUDIO_SET_AV_SYNC && arguments[3] == 1);
    assert(requests[4] == AUDIO_PLAY);
    playback.isSeeking = 0;
    LINUXDVB_DEBUG_LEVEL = 10;
    request_count = 0;
    LinuxDvbTiming("test-unavailable");
    assert(request_count == 2 && requests[0] == VIDEO_GET_PTS && requests[1] == AUDIO_GET_PTS);
    switch_results_and_reset(&context, &playback, &managers);
    no_message_for_the_closed_video_device(&context);
    writer_without_reset(&context, &managers);
    pthread_mutex_destroy(&LinuxDVBmutex);
    return 0;
}
