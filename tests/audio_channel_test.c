#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Left, stereo or right of dual mono audio: AUDIO_CHANNEL_SELECT on the audio
 * decoder. The driver is a list of the ioctls it was given; a selection made
 * before the decoder runs waits for AUDIO_PLAY, every later AUDIO_PLAY (track
 * switch, DTS-HD restart, PCM restart after a clear) sets it again, and a stop
 * leaves the decoder at stereo. Every case runs in a child of its own. */

static unsigned long requests[64];
static int arguments[64];
static int request_count;
static int refuse_select;

int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    (void)fd;
    assert(request_count < 64);
    requests[request_count] = request;
    arguments[request_count] = -1;
    if (request == AUDIO_CHANNEL_SELECT || request == AUDIO_SET_BYPASS_MODE)
    {
        va_start(args, request);
        arguments[request_count] = va_arg(args, int);
        va_end(args);
    }
    request_count++;
    if (request == AUDIO_CHANNEL_SELECT && refuse_select)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int write_frame(void *call) { (void)call; return 100; }
static int reset_writer(void) { return 0; }
static WriterCaps_t mp2_caps = {.dvbStreamType = AUDIOTYPE_MPEG};
static WriterCaps_t dts_caps = {.dvbStreamType = AUDIOTYPE_DTS};
static WriterCaps_t pcm_caps = {.dvbStreamType = AUDIOTYPE_RAW};
static Writer_t mp2_writer = {.reset = reset_writer, .writeData = write_frame, .caps = &mp2_caps};
static Writer_t dts_writer = {.reset = reset_writer, .writeData = write_frame, .caps = &dts_caps};
static Writer_t pcm_writer = {.reset = reset_writer, .writeData = write_frame, .caps = &pcm_caps};
static const char *encoding = "A_MPEG_L2";
Writer_t *getWriter(char *name)
{
    if (!strcmp(name, "A_DTS"))
        return &dts_writer;
    if (!strcmp(name, "A_IPCM"))
        return &pcm_writer;
    return &mp2_writer;
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
void WriterWakeUp(void) {}
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup(encoding);
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1, .isAudio = 1};
static Manager_t audio = {.Command = audio_command};
static ManagerHandler_t managers = {.audio = &audio};
static Context_t context = {.playback = &playback, .manager = &managers};

static int select_channel(int channel)
{
    return Command(&context, OUTPUT_AUDIO_CHANNEL, &channel);
}

/* the selections given to the driver, in order, from the request "first" on */
static int selections(int first, int *out, int size)
{
    int i, n = 0;
    for (i = first; i < request_count; i++)
        if (requests[i] == AUDIO_CHANNEL_SELECT)
        {
            assert(n < size);
            out[n++] = arguments[i];
        }
    return n;
}

static int last_index(unsigned long request)
{
    int i;
    for (i = request_count - 1; i >= 0; i--)
        if (requests[i] == request)
            return i;
    return -1;
}

static void start(void)
{
    static int mutex_ready;
    if (!mutex_ready)
    {
        /* once: initialising a mutex that is already initialised is undefined */
        assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
        mutex_ready = 1;
    }
    audiofd = 1;
    assert(!Command(&context, OUTPUT_PLAY, "audio"));
}

/* Stereo, the default: not a single AUDIO_CHANNEL_SELECT, not even at a stop. */
static void stereo_untouched(void)
{
    int got[4];
    start();
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(!Command(&context, OUTPUT_STOP, "audio"));
    assert(selections(0, got, 4) == 0);
}

/* Before the device is open: kept, set right after AUDIO_PLAY. */
static void before_the_start(void)
{
    int got[4];
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_RIGHT));
    assert(request_count == 0);
    start();
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_RIGHT);
    assert(last_index(AUDIO_CHANNEL_SELECT) > last_index(AUDIO_PLAY));
}

/* While playing: at once; a track switch sets it again after its AUDIO_PLAY. */
static void while_playing_and_switch(void)
{
    int got[4];
    start();
    request_count = 0;
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_LEFT));
    assert(request_count == 1 && requests[0] == AUDIO_CHANNEL_SELECT && arguments[0] == AUDIO_MONO_LEFT);
    request_count = 0;
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_LEFT);
    assert(last_index(AUDIO_CHANNEL_SELECT) > last_index(AUDIO_PLAY));
    /* back to stereo is given to the driver as well */
    request_count = 0;
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_STEREO));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_STEREO);
    request_count = 0;
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(selections(0, got, 4) == 0);
}

/* A switch while paused: the selection before the decoder is held again. */
static void switch_while_paused(void)
{
    start();
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_RIGHT));
    playback.isPaused = 1;
    request_count = 0;
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(last_index(AUDIO_PLAY) < last_index(AUDIO_CHANNEL_SELECT));
    assert(last_index(AUDIO_CHANNEL_SELECT) < last_index(AUDIO_PAUSE));
}

/* A stop leaves the decoder at stereo before AUDIO_STOP; a new start sets the
 * selection again. */
static void stop_resets_to_stereo(void)
{
    int got[4];
    start();
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_RIGHT));
    request_count = 0;
    assert(!Command(&context, OUTPUT_STOP, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_STEREO);
    assert(last_index(AUDIO_CHANNEL_SELECT) < last_index(AUDIO_STOP));
    /* stereo already: a second stop does not ask again */
    request_count = 0;
    assert(!Command(&context, OUTPUT_STOP, "audio"));
    assert(selections(0, got, 4) == 0);
    request_count = 0;
    assert(!Command(&context, OUTPUT_PLAY, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_RIGHT);
}

/* Not a selection: refused, nothing changes. */
static void invalid_value(void)
{
    int got[4];
    start();
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_LEFT));
    request_count = 0;
    assert(select_channel(3) < 0);
    assert(select_channel(-1) < 0);
    assert(request_count == 0);
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_LEFT);
}

/* A driver without the ioctl: the error is reported, the selection is kept
 * (software decoded audio takes it) and the next start tries again. */
static void driver_refuses(void)
{
    int got[4];
    start();
    refuse_select = 1;
    assert(select_channel(OUTPUT_AUDIO_CHANNEL_RIGHT) < 0);
    request_count = 0;
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_RIGHT);
    /* never applied: the stop has nothing to undo */
    request_count = 0;
    assert(!Command(&context, OUTPUT_STOP, "audio"));
    assert(selections(0, got, 4) == 0);
}

/* The DTS-HD restart before the first frame starts the decoder again. */
static void dts_hd_restart(void)
{
    static uint8_t core_hd[24] = {0x7f, 0xfe, 0x80, 0x01, 0xfc, 0x00, 0x00, 0xf0,
                                  [16] = 0x64, 0x58, 0x20, 0x25};
    AudioVideoOut_t out = {.data = core_hd, .len = sizeof(core_hd), .type = "audio"};
    int got[4];
    assert(!setenv("EP3_DTS_HD_MODE", "1", 1));
    encoding = "A_DTS";
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_LEFT));
    start();
    request_count = 0;
    assert(!LinuxDvbOutput.Write(&context, &out));
    assert(last_index(AUDIO_PLAY) >= 0);
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_LEFT);
    assert(last_index(AUDIO_CHANNEL_SELECT) > last_index(AUDIO_PLAY));
}

/* PCM: the clear of a seek restarts the decoder for the next PCM header. */
static void pcm_clear(void)
{
    int got[4];
    encoding = "A_IPCM";
    start();
    assert(!select_channel(OUTPUT_AUDIO_CHANNEL_RIGHT));
    request_count = 0;
    assert(!Command(&context, OUTPUT_CLEAR, "audio"));
    assert(selections(0, got, 4) == 1 && got[0] == AUDIO_MONO_RIGHT);
    assert(last_index(AUDIO_CHANNEL_SELECT) > last_index(AUDIO_PLAY));
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
    fprintf(stderr, "audio_channel: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_DTS_HD_MODE");
    failed += run("stereo untouched", stereo_untouched);
    failed += run("selected before the start", before_the_start);
    failed += run("while playing and after a switch", while_playing_and_switch);
    failed += run("switch while paused", switch_while_paused);
    failed += run("stop resets to stereo", stop_resets_to_stereo);
    failed += run("invalid value", invalid_value);
    failed += run("driver refuses", driver_refuses);
    failed += run("DTS-HD restart", dts_hd_restart);
    failed += run("PCM clear", pcm_clear);
    return failed ? 1 : 0;
}
