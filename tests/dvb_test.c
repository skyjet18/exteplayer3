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

/* The video track report on a position request; its text is checked in video_info_test.c. */
void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

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

/* Set: the position requests are answered with decoder_pts. */
static int decoder_answers;
static unsigned long long decoder_pts;

int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list args;
    unsigned long long *pts = NULL;
    assert(request_count < 64);
    requests[request_count] = request;
    va_start(args, request);
    if (request == AUDIO_SET_AV_SYNC || request == AUDIO_SET_BYPASS_MODE ||
        request == VIDEO_SET_STREAMTYPE)
        arguments[request_count] = va_arg(args, int);
    if (request == VIDEO_GET_PTS || request == AUDIO_GET_PTS)
        pts = va_arg(args, unsigned long long *);
    va_end(args);
    request_count++;
    if (request == failing_request) {
        errno = EIO;
        return -1;
    }
    if (request == VIDEO_GET_PTS || request == AUDIO_GET_PTS) {
        if (decoder_answers) {
            *pts = decoder_pts;
            return 0;
        }
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
static stb_type_t stb_type = STB_VUPLUS;
stb_type_t GetSTBType(void) { return stb_type; }
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
void WriterWakeUp(void) {}
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }
/* Write() refers to these; getWriter() always has the writer stub, which
 * writes nothing. */
ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
Writer_t *getDefaultVideoWriter(void) { return NULL; }
Writer_t *getDefaultAudioWriter(void) { return NULL; }

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

/* The position of a decoder that runs out of data (a live playlist that does
 * not grow): the decoder clock goes on after the last frame and comes back
 * to the PTS of the frames when data comes again. */
#define TICKS(seconds) ((unsigned long long)((seconds) * 90000 + 0.5))

static unsigned long long reported_position(Context_t *context, unsigned long long decoder)
{
    unsigned long long reported = 0;
    decoder_pts = decoder;
    request_count = 0;
    assert(!LinuxDvbPts(context, &reported));
    return reported;
}

static void written(Context_t *context, const char *type, unsigned long long from,
                    unsigned long long to)
{
    uint8_t byte = 0;
    AudioVideoOut_t out = {.data = &byte, .len = 1, .width = 64, .height = 64};
    out.type = (char *)type;
    for (; from <= to; from += TICKS(0.04))
    {
        out.pts = out.dts = (int64_t)from;
        request_count = 0;
        assert(!Write(context, &out));
    }
}

static void cleared(Context_t *context, const char *type)
{
    request_count = 0;
    assert(!Command(context, OUTPUT_CLEAR, (void *)type));
}

static void position_within_the_written_data(Context_t *context, ManagerHandler_t *managers)
{
    const unsigned long long wrap = 1ULL << 33;
    int video = videofd;

    decoder_answers = 1;
    videofd = -1;
    cleared(context, "audio");
    written(context, "audio", 0, TICKS(24.84));
    assert(reported_position(context, TICKS(20)) == TICKS(20));
    assert(reported_position(context, TICKS(24.9)) == TICKS(24.9));
    /* no more data: the decoder clock goes on, the position stands */
    assert(reported_position(context, TICKS(26.9)) == TICKS(25.14));
    assert(reported_position(context, TICKS(36.9)) == TICKS(25.14));
    /* data again, the decoder still shows nothing of it */
    written(context, "audio", TICKS(24.88), TICKS(26.84));
    assert(reported_position(context, TICKS(37.5)) == TICKS(25.14));
    /* the decoder shows the new data: no step back */
    assert(reported_position(context, TICKS(24.88)) == TICKS(25.14));
    assert(reported_position(context, TICKS(25.5)) == TICKS(25.5));
    assert(reported_position(context, TICKS(26.5)) == TICKS(26.5));
    /* after a clear: the PTS of an earlier playback is left as it is */
    cleared(context, "audio");
    assert(reported_position(context, TICKS(40)) == TICKS(40));
    written(context, "audio", TICKS(10), TICKS(12));
    assert(reported_position(context, TICKS(40)) == TICKS(40));
    assert(reported_position(context, TICKS(10.5)) == TICKS(10.5));
    assert(reported_position(context, TICKS(13)) == TICKS(12.3));
    /* a jump back of more than a second is reported */
    assert(reported_position(context, TICKS(5)) == TICKS(5));
    /* far after the data (10 minutes and more) is no stall */
    assert(reported_position(context, TICKS(12 + 1200)) == TICKS(12 + 1200));
    /* the PTS wrap after 2^33 */
    cleared(context, "audio");
    written(context, "audio", wrap - TICKS(1), wrap + TICKS(0.48));
    assert(reported_position(context, wrap - TICKS(0.5)) == wrap - TICKS(0.5));
    assert(reported_position(context, TICKS(0.3)) == TICKS(0.3));
    assert(reported_position(context, TICKS(3)) == TICKS(0.78));
    /* after a start: the decoder has shown nothing of the data yet and still
     * counts on with the clock of an earlier playback (audio-only DTS on
     * VTi: 9.5 s reported two seconds after the start, data written up to
     * 3.92 s, the clock running on) */
    request_count = 0;
    assert(!Command(context, OUTPUT_PLAY, "audio"));
    /* nothing written since the start: 0, not the earlier clock */
    assert(reported_position(context, TICKS(7.5)) == 0);
    written(context, "audio", 0, TICKS(3.92));
    assert(reported_position(context, TICKS(9.5)) == 0);
    assert(reported_position(context, TICKS(12.5)) == 0);
    /* within the data, as far as the time since the first write allows: the
     * decoder is taken, and from then on its clock after the data stands at
     * the data as before */
    assert(reported_position(context, TICKS(0.8)) == TICKS(0.8));
    assert(reported_position(context, TICKS(9.5)) == TICKS(4.22));
    /* a start at a later PTS, the earlier clock far after the data */
    request_count = 0;
    assert(!Command(context, OUTPUT_PLAY, "audio"));
    written(context, "audio", TICKS(3600), TICKS(3602));
    assert(reported_position(context, TICKS(3700)) == TICKS(3600));
    /* a track switch before the decoder showed the data: the data written
     * after it */
    request_count = 0;
    context->playback->isAudioSwitching = 1;
    assert(!Command(context, OUTPUT_SWITCH, "audio"));
    context->playback->isAudioSwitching = 0;
    written(context, "audio", TICKS(3602.04), TICKS(3603));
    assert(reported_position(context, TICKS(3700)) == TICKS(3602.04));
    assert(reported_position(context, TICKS(3602.5)) == TICKS(3602.5));
    /* the clock of the earlier playback far before the data: the first PTS
     * written (see video_pts_test.c for the end of that), and a clear keeps
     * the earlier-playback value as before */
    request_count = 0;
    assert(!Command(context, OUTPUT_PLAY, "audio"));
    written(context, "audio", TICKS(50), TICKS(52));
    assert(reported_position(context, TICKS(20)) == TICKS(50));
    cleared(context, "audio");
    written(context, "audio", TICKS(10), TICKS(12));
    assert(reported_position(context, TICKS(40)) == TICKS(40));
    /* video: the video data bounds the position of the video decoder */
    videofd = 2;
    managers->video = managers->audio;
    cleared(context, "video");
    written(context, "video", TICKS(100), TICKS(110));
    assert(reported_position(context, TICKS(105)) == TICKS(105));
    assert(reported_position(context, TICKS(118)) == TICKS(110.3));
    written(context, "video", TICKS(110.04), TICKS(112));
    assert(reported_position(context, TICKS(110.04)) == TICKS(110.3));
    assert(reported_position(context, TICKS(111)) == TICKS(111));
    /* the video output restarted after a long gap (OUTPUT_SWITCH): the
     * decoder reports unrelated values until it shows the new data */
    assert(reported_position(context, TICKS(113)) == TICKS(112.3));
    request_count = 0;
    assert(!Command(context, OUTPUT_SWITCH, "video"));
    assert(reported_position(context, TICKS(1.55)) == TICKS(112.3));
    written(context, "video", TICKS(112.04), TICKS(114));
    assert(reported_position(context, TICKS(1.55)) == TICKS(112.3));
    assert(reported_position(context, TICKS(120)) == TICKS(112.3));
    assert(reported_position(context, TICKS(112.1)) == TICKS(112.3));
    assert(reported_position(context, TICKS(113)) == TICKS(113));
    /* data comes again but the decoder stays after it: the position stands
     * until the data has gone 4 s beyond it, then follows the data */
    assert(reported_position(context, TICKS(115)) == TICKS(114.3));
    written(context, "video", TICKS(114.04), TICKS(117.96));
    assert(reported_position(context, TICKS(119)) == TICKS(114.3));
    written(context, "video", TICKS(118), TICKS(118));
    assert(reported_position(context, TICKS(119)) == TICKS(118.3));
    assert(reported_position(context, TICKS(118.2)) == TICKS(118.3));
    written(context, "video", TICKS(118.04), TICKS(119));
    assert(reported_position(context, TICKS(118.5)) == TICKS(118.5));
    managers->video = NULL;
    videofd = video;
    decoder_answers = 0;
}

/* EP3_HISI_AV_SYNC=1: on a HiSilicon receiver the hardware A/V sync of the
 * playback also for streams that are not DASH; off (the default), on other
 * receivers or with another value nothing changes. */
static void hisilicon_av_sync(Context_t *context, PlaybackHandler_t *playback)
{
    playback->isDash = 0;
    playback->AVSync = 1;
    failing_request = 0;
    audiofd = 1;
    unsetenv("EP3_HISI_AV_SYNC");
    stb_type = STB_HISILICON;
    request_count = 0;
    /* the timing log behind it asks the PTS of the devices: only the first
     * request is the sync */
    assert(!LinuxDvbAVSync(context, "audio"));
    assert(request_count >= 1 && requests[0] == AUDIO_SET_AV_SYNC && arguments[0] == 0);
    setenv("EP3_HISI_AV_SYNC", "1", 1);
    request_count = 0;
    assert(!LinuxDvbAVSync(context, "audio") && arguments[0] == 1);
    playback->AVSync = 0;
    request_count = 0;
    assert(!LinuxDvbAVSync(context, "audio") && arguments[0] == 0);
    playback->AVSync = 1;
    setenv("EP3_HISI_AV_SYNC", "yes", 1);
    request_count = 0;
    assert(!LinuxDvbAVSync(context, "audio") && arguments[0] == 0);
    setenv("EP3_HISI_AV_SYNC", "1", 1);
    stb_type = STB_VUPLUS;
    request_count = 0;
    assert(!LinuxDvbAVSync(context, "audio") && arguments[0] == 0);
    unsetenv("EP3_HISI_AV_SYNC");
}

/* EP3_HISI_NO_CLEAR=1: on a HiSilicon receiver no VIDEO/AUDIO_CLEAR_BUFFER
 * when the devices are opened; off or on other receivers the clears stay. */
static void hisilicon_no_clear(Context_t *context)
{
    int saved_video = videofd, saved_audio = audiofd;
    bool saved_buffered = isBufferedOutput;
    int on;

    isBufferedOutput = false;
    failing_request = 0;
    stb_type = STB_HISILICON;
    for (on = 0; on <= 1; on++)
    {
        if (on)
            setenv("EP3_HISI_NO_CLEAR", "1", 1);
        else
            unsetenv("EP3_HISI_NO_CLEAR");
        videofd = audiofd = -1;
        opens = 0;
        succeed_at = 1;
        request_count = 0;
        assert(!LinuxDvbOpen(context, "video"));
        assert((requests[0] == VIDEO_CLEAR_BUFFER) == !on);
        assert(requests[on ? 0 : 1] == VIDEO_SELECT_SOURCE);
        request_count = 0;
        assert(!LinuxDvbOpen(context, "audio"));
        assert((requests[0] == AUDIO_CLEAR_BUFFER) == !on);
        assert(requests[on ? 0 : 1] == AUDIO_SELECT_SOURCE);
    }
    stb_type = STB_VUPLUS;
    videofd = -1;
    opens = 0;
    request_count = 0;
    assert(!LinuxDvbOpen(context, "video") && requests[0] == VIDEO_CLEAR_BUFFER);
    unsetenv("EP3_HISI_NO_CLEAR");
    succeed_at = 0;
    videofd = saved_video;
    audiofd = saved_audio;
    isBufferedOutput = saved_buffered;
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
    position_within_the_written_data(&context, &managers);
    hisilicon_av_sync(&context, &playback);
    hisilicon_no_clear(&context);
    pthread_mutex_destroy(&LinuxDVBmutex);
    return 0;
}
