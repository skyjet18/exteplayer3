#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>

/* The stall limit of http inputs: the clock of the container moves only when
 * the test moves it, and the calls the limit is put around are the test's.
 * They ask the interrupt callback of the context, as the protocols do while
 * they wait, before and after the clock has moved. */
static int64_t test_gettime_relative(void);
static int test_open_input(AVFormatContext **ps, const char *url, const AVInputFormat *format,
                           AVDictionary **options);
static int test_find_stream_info(AVFormatContext *ic, AVDictionary **options);
static int test_read_frame(AVFormatContext *s, AVPacket *packet);
#define av_gettime_relative test_gettime_relative
#define avformat_open_input test_open_input
#define avformat_find_stream_info test_find_stream_info
#define av_read_frame test_read_frame
#include "../container/container_ffmpeg.c"
#undef av_gettime_relative
#undef avformat_open_input
#undef avformat_find_stream_info
#undef av_read_frame

#define DAY_MS 86400000u

enum { OPEN_PASSES, OPEN_FAILS, OPEN_SWALLOWS };

static PlaybackHandler_t playback;
static int64_t clock_us = 1000000000;
static uint32_t limit_ms; /* the limit the case expects, 0: none */
static int open_mode, info_stalls;
static int opens, infos, reads, outside;
static char notification[4096];
static char url[] = "http://127.0.0.1:9/stall.ts";
static char wav[] = "/tmp/exteplayer3-stall-XXXXXX";

static int64_t test_gettime_relative(void)
{
    return clock_us;
}

static void advance(uint32_t ms)
{
    clock_us += (int64_t)ms * 1000;
}

static int interrupted(AVFormatContext *s)
{
    return s->interrupt_callback.callback(s->interrupt_callback.opaque);
}

/* The limit itself is still allowed; without one a day is. */
static void within_the_limit(AVFormatContext *s)
{
    advance(limit_ms ? limit_ms : DAY_MS);
    assert(!interrupted(s));
}

static void ended_after_the_limit(AVFormatContext *s)
{
    within_the_limit(s);
    assert(limit_ms);
    advance(1);
    assert(interrupted(s));
}

static int test_open_input(AVFormatContext **ps, const char *name, const AVInputFormat *format,
                           AVDictionary **options)
{
    int ret;
    (void)name;
    (void)format;
    (void)options;
    opens++;
    assert(*ps && !interrupted(*ps));
    if (open_mode == OPEN_FAILS) {
        ended_after_the_limit(*ps);
        /* a failed open frees the context */
        avformat_free_context(*ps);
        *ps = NULL;
        return AVERROR_EXIT;
    }
    /* the local file stands in for what the server sends */
    ret = avformat_open_input(ps, wav, NULL, NULL);
    assert(!ret);
    if (open_mode == OPEN_SWALLOWS)
        ended_after_the_limit(*ps);
    else
        within_the_limit(*ps);
    return ret;
}

static int test_find_stream_info(AVFormatContext *ic, AVDictionary **options)
{
    int ret = avformat_find_stream_info(ic, options);
    infos++;
    assert(ret >= 0);
    /* a limit of its own, not what the open has left of it */
    within_the_limit(ic);
    if (info_stalls) {
        /* every byte read starts the clock again */
        for (int i = 0; i < 4; i++) {
            advance(limit_ms);
            ic->pb->bytes_read++;
            assert(!interrupted(ic));
        }
        ended_after_the_limit(ic);
    }
    return ret;
}

/* Two reads, each answered with a packet. The second is paused for ten times
 * the limit first and ends the playback. */
static int test_read_frame(AVFormatContext *s, AVPacket *packet)
{
    reads++;
    /* the time since the last read is not counted */
    assert(!interrupted(s));
    if (reads == 1) {
        within_the_limit(s);
    } else {
        playback.isPaused = 1;
        advance(limit_ms ? 10 * limit_ms : DAY_MS);
        assert(!interrupted(s));
        playback.isPaused = 0;
        if (limit_ms)
            ended_after_the_limit(s);
        else
            within_the_limit(s);
        playback.isPlaying = 0;
    }
    memset(packet, 0, sizeof(*packet));
    packet->pts = packet->dts = AV_NOPTS_VALUE;
    return 0;
}

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(notification, sizeof(notification), format, args);
    va_end(args);
}

int64_t PlaybackLastContinue(void)
{
    return 0;
}

uint32_t PlaybackChangeSerial(void)
{
    return 0;
}

void PlaybackWaitChange(uint32_t serial, int32_t timeoutMs)
{
    (void)serial;
    (void)timeoutMs;
}

void LinuxDvbBuffSetStamp(void *stamp)
{
    (void)stamp;
}

static int32_t output_command(void *opaque, OutputCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == OUTPUT_GET_BUFFER_SIZE);
    *(uint32_t *)argument = 0;
    return 0;
}

static int track_command(void *opaque, ManagerCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = NULL;
    return 0;
}

/* Asked for after av_read_frame() has returned a packet, where the packet
 * goes on to the output: a full output buffer can hold the thread here. */
static int video_command(void *opaque, ManagerCmd_t command, void *argument)
{
    outside++;
    advance(limit_ms ? 10 * limit_ms : DAY_MS);
    assert(!interrupted(avContextTab[0]));
    return track_command(opaque, command, argument);
}

static OutputHandler_t output = {.Command = output_command};
static Manager_t video = {.Command = video_command};
static Manager_t other = {.Command = track_command};
static ManagerHandler_t managers = {.video = &video, .audio = &other, .subtitle = &other};
static Context_t context = {.playback = &playback, .output = &output, .manager = &managers};

static int32_t open_input(char *name, uint32_t timeout)
{
    playback.httpTimeout = timeout;
    return container_ffmpeg_init_av_context(&context, name, 0, NULL, 0, 0);
}

static void assert_timed_out(void)
{
    char code[32];
    snprintf(code, sizeof(code), "\"code\":%d}", AVERROR(ETIMEDOUT));
    assert(!avContextTab[0] && strstr(notification, code));
}

/* Opened, then read in the play thread until the second read ends it. */
static void open_and_play(char *name, uint32_t timeout)
{
    assert(!open_input(name, timeout));
    assert(opens == 1 && infos == 1 && avContextTab[0]);
    playback.isPlaying = 1;
    FFMPEGThread(&context);
    assert(reads == 2 && outside == 2);
}

/* -T 19999: 3x is 59997 ms, below the floor of 60 s. */
static void open_ended_at_the_floor(void)
{
    limit_ms = 60000;
    open_mode = OPEN_FAILS;
    assert(open_input(url, 19999) == cERR_CONTAINER_FFMPEG_OPEN);
    assert_timed_out();
}

static void open_ended_above_the_floor(void)
{
    limit_ms = 60003;
    open_mode = OPEN_FAILS;
    assert(open_input(url, 20001) == cERR_CONTAINER_FFMPEG_OPEN);
    assert_timed_out();
}

/* The demuxer reports success after the interruption. */
static void open_swallowing_the_interruption(void)
{
    limit_ms = 60000;
    open_mode = OPEN_SWALLOWS;
    assert(open_input(url, 10000) == cERR_CONTAINER_FFMPEG_OPEN);
    assert(opens == 1 && infos == 0);
    assert_timed_out();
}

static void stream_info_without_a_byte(void)
{
    limit_ms = 60000;
    info_stalls = 1;
    assert(open_input(url, 10000) == cERR_CONTAINER_FFMPEG_OPEN);
    assert(opens == 1 && infos == 1);
    assert_timed_out();
}

static void read_ended_at_the_floor(void)
{
    limit_ms = 60000;
    open_and_play(url, 19999);
}

static void read_ended_above_the_floor(void)
{
    limit_ms = 60003;
    open_and_play(url, 20001);
}

/* -T 0 asks for no timeout: no limit for the open, the probing or a read. */
static void no_limit_without_a_timeout(void)
{
    limit_ms = 0;
    open_and_play(url, 0);
}

/* A local file reads through the custom I/O and may wait for a growing file. */
static void no_limit_for_a_local_file(void)
{
    limit_ms = 0;
    open_and_play(wav, 10000);
}

static int run(const char *name, void (*test)(void))
{
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(2); /* a play thread that does not end fails the case */
        test();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && !WEXITSTATUS(status))
        return 0;
    fprintf(stderr, "net_stall: %s failed\n", name);
    return 1;
}

int main(void)
{
    static const uint8_t header[44] = {
        'R', 'I', 'F', 'F', 100, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x80, 0x3e, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 64, 0, 0, 0};
    uint8_t samples[64] = {0};
    int failed = 0;
    int file = mkstemp(wav);
    assert(file >= 0 && write(file, header, sizeof(header)) == sizeof(header));
    assert(write(file, samples, sizeof(samples)) == sizeof(samples) && !close(file));
    ffmpeg_buf_size = 0; /* no read-ahead thread behind the http input */
    failed += run("open ended at the 60 s floor", open_ended_at_the_floor);
    failed += run("open ended at 3x the timeout", open_ended_above_the_floor);
    failed += run("open that reports success after the interruption", open_swallowing_the_interruption);
    failed += run("stream info without a byte", stream_info_without_a_byte);
    failed += run("read ended at the 60 s floor", read_ended_at_the_floor);
    failed += run("read ended at 3x the timeout", read_ended_above_the_floor);
    failed += run("no limit with -T 0", no_limit_without_a_timeout);
    failed += run("no limit for a local file", no_limit_for_a_local_file);
    unlink(wav);
    return failed ? 1 : 0;
}
