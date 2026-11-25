#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
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
#define open test_open
int test_open(const char *path, int flags, ...);
#define close test_close
int test_close(int fd);
#define dup2 test_dup2
int test_dup2(int from, int to);
#define poll test_poll
int test_poll(struct pollfd *fds, nfds_t count, int timeout);
#define dup test_dup
int test_dup(int fd);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef open
#undef close
#undef dup2
#undef poll
#undef dup

/* The stop before the _exit() of the fallbacks of the stop signals (a second
 * signal, a stop that takes longer than STOP_SIGNAL_TIMEOUT):
 * LinuxDvbFallbackStop() stops the decoders still running and closes their
 * devices, without waiting for a thread of the player longer than
 * LINUXDVB_FALLBACK_STOP_MS, and touches nothing that the normal stop has
 * closed already. The driver records the requests of each device, the close
 * and the dup2() of /dev/null onto it; every case runs in a child. */

#define VIDEO_FD 10
#define AUDIO_FD 11
#define NULL_FD 12
/* the copy of videofd the event thread polls (-b) */
#define EVENT_FD (VIDEO_FD + 100)
#define CLOSED 0xc105edUL
#define RELEASED 0xd02UL

struct request { int fd; unsigned long request; int arg; };
static struct request requests[256];
static volatile int request_count;
static volatile int null_opened, null_closed, event_fd_closed;
/* AUDIO_STOP of a driver that does not return */
static volatile int audio_stop_blocks;
/* the poll of the event thread: sleeps this long */
static volatile int poll_sleep_ms, in_poll, stopped_during_poll;

static long ms_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static void record(int fd, unsigned long request, int arg)
{
    assert(request_count < 256);
    requests[request_count] = (struct request){fd, request, arg};
    request_count++;
}

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
    if (fd == VIDEO_FD && (request == VIDEO_STOP || request == VIDEO_CLEAR_BUFFER) && in_poll)
        stopped_during_poll++;
    record(fd, request, arg);
    if (fd == AUDIO_FD && request == AUDIO_STOP && audio_stop_blocks)
        for (;;)
            pause();
    return 0;
}

int test_open(const char *path, int flags, ...)
{
    (void)flags;
    if (!strcmp(path, "/dev/null"))
    {
        null_opened++;
        return NULL_FD;
    }
    if (!strcmp(path, VIDEODEV))
        return VIDEO_FD;
    assert(!strcmp(path, AUDIODEV));
    return AUDIO_FD;
}

int test_close(int fd)
{
    if (fd == NULL_FD)
    {
        null_closed++;
        return 0;
    }
    if (fd == EVENT_FD)
    {
        event_fd_closed = 1;
        return 0;
    }
    assert(fd == VIDEO_FD || fd == AUDIO_FD);
    record(fd, CLOSED, -1);
    return 0;
}

/* the device is released, the number stays taken by /dev/null */
int test_dup2(int from, int to)
{
    assert(from == NULL_FD);
    assert(to == VIDEO_FD || to == AUDIO_FD);
    record(to, RELEASED, -1);
    return to;
}

int test_dup(int fd)
{
    assert(fd == VIDEO_FD);
    return EVENT_FD;
}

/* The poll of the Vu+ driver: it sleeps, whatever happens meanwhile. */
int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == EVENT_FD && !timeout);
    in_poll = 1;
    if (poll_sleep_ms)
        usleep(poll_sleep_ms * 1000);
    in_poll = 0;
    fds[0].revents = 0;
    return 0;
}

stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int reset_writer(void) { return 0; }
static WriterCaps_t ac3_caps = {.name = "ac3", .dvbStreamType = AUDIOTYPE_AC3};
static WriterCaps_t mpeg2_caps = {.name = "mpeg2", .dvbStreamType = STREAMTYPE_MPEG2};
static Writer_t ac3_writer = {.reset = reset_writer, .caps = &ac3_caps};
static Writer_t mpeg2_writer = {.reset = reset_writer, .caps = &mpeg2_caps};
Writer_t *getWriter(char *name)
{
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
void WriterWakeUp(void) {}
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup("A_AC3");
    return 0;
}

static int video_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
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

static int index_of(int fd, unsigned long request)
{
    int i;
    for (i = 0; i < request_count; i++)
        if (requests[i].fd == fd && requests[i].request == request)
            return i;
    return -1;
}

static void start(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    both(OUTPUT_OPEN);
    assert(videofd == VIDEO_FD && audiofd == AUDIO_FD);
    both(OUTPUT_PLAY);
}

/* the fallback, timed */
static int fallback(long *ms)
{
    struct timespec started;
    int result;
    clock_gettime(CLOCK_MONOTONIC, &started);
    result = LinuxDvbFallbackStop();
    *ms = ms_since(&started);
    return result;
}

/* Stopped as by LinuxDvbStop(), then released: CLEAR_BUFFER, STOP and the
 * source back to the demux before the dup2() of /dev/null, no close. */
static void stopped_and_released(int fd, unsigned long clear, unsigned long stop,
                                 unsigned long source)
{
    int released = index_of(fd, RELEASED);
    assert(count(fd, stop) == 1 && count(fd, RELEASED) == 1);
    assert(index_of(fd, clear) < index_of(fd, stop));
    assert(index_of(fd, stop) < released && index_of(fd, source) < released);
    assert(count(fd, CLOSED) == 0);
}

/* (a) A fallback while both decoders run: both are stopped, given back to
 * the demux and released before the return, within the wait. A close of
 * the normal stop that comes after it touches nothing. */
static void devices_open(void)
{
    long ms;
    int after;
    start();
    request_count = 0;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_DONE);
    assert(ms < LINUXDVB_FALLBACK_STOP_MS);
    stopped_and_released(AUDIO_FD, AUDIO_CLEAR_BUFFER, AUDIO_STOP, AUDIO_SELECT_SOURCE);
    stopped_and_released(VIDEO_FD, VIDEO_CLEAR_BUFFER, VIDEO_STOP, VIDEO_SELECT_SOURCE);
    assert(requests[index_of(VIDEO_FD, VIDEO_SELECT_SOURCE)].arg == VIDEO_SOURCE_DEMUX);
    assert(requests[index_of(AUDIO_FD, AUDIO_SELECT_SOURCE)].arg == AUDIO_SOURCE_DEMUX);
    assert(videofd == -1 && audiofd == -1);
    assert(null_opened == 2 && null_closed == 2);
    after = request_count;
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(request_count == after);
}

/* (b) AUDIO_STOP of the driver never returns: the fallback still returns
 * after the wait, and the caller ends the process. */
static void audio_stop_hangs(void)
{
    long ms;
    start();
    audio_stop_blocks = 1;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_TIMEOUT);
    fprintf(stderr, "fallback_stop: AUDIO_STOP that hangs, back after %ld ms\n", ms);
    assert(ms >= LINUXDVB_FALLBACK_STOP_MS - 50 && ms <= LINUXDVB_FALLBACK_STOP_MS + 300);
    assert(count(AUDIO_FD, AUDIO_STOP) == 1);
}

/* (c) The normal stop has closed both devices: nothing is touched. */
static void closed_already(void)
{
    long ms;
    int after;
    start();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
    after = request_count;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_NOTHING);
    assert(request_count == after);
    assert(!null_opened);
}

/* The normal stop has closed the audio device only (it hangs before the
 * close of the video device would come, say): the video device is stopped
 * and released, the audio device not touched again. */
static void audio_closed_already(void)
{
    long ms;
    start();
    assert(!Command(&context, OUTPUT_CLOSE, "audio"));
    request_count = 0;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_DONE);
    assert(count(AUDIO_FD, RELEASED) == 0 && count(AUDIO_FD, CLOSED) == 0);
    assert(count(AUDIO_FD, AUDIO_STOP) == 0);
    stopped_and_released(VIDEO_FD, VIDEO_CLEAR_BUFFER, VIDEO_STOP, VIDEO_SELECT_SOURCE);
}

/* Another thread keeps LinuxDVBmutex (a stop that hangs in the driver):
 * the fallback does not wait for it, and touches no device. */
static void lock_kept(void)
{
    long ms;
    start();
    request_count = 0;
    assert(!pthread_mutex_lock(&LinuxDVBmutex));
    assert(fallback(&ms) == LINUXDVB_FALLBACK_PARTIAL);
    assert(ms < LINUXDVB_FALLBACK_STOP_MS);
    assert(request_count == 0 && !null_opened);
    assert(videofd == VIDEO_FD && audiofd == AUDIO_FD);
}

/* A device the normal stop has left open (-b: the buffering thread still in
 * a write of the driver): stopped, not released. */
static void left_open(void)
{
    long ms;
    start();
    audioLeftOpen = true;
    request_count = 0;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_PARTIAL);
    assert(count(AUDIO_FD, AUDIO_STOP) == 1);
    assert(count(AUDIO_FD, RELEASED) == 0 && count(AUDIO_FD, CLOSED) == 0);
    assert(audiofd == AUDIO_FD);
    stopped_and_released(VIDEO_FD, VIDEO_CLEAR_BUFFER, VIDEO_STOP, VIDEO_SELECT_SOURCE);
}

/* -b, playing, the event thread started by the first video frame */
static void start_buffered(void)
{
    int i;
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    isBufferedOutput = true;
    both(OUTPUT_OPEN);
    both(OUTPUT_PLAY);
    assert(LinuxDvbEventThreadPolls(&context));
    for (i = 0; i < 2000 && !in_poll; i++)
        usleep(1000);
    assert(in_poll);
}

/* The event thread in a short poll of the driver: the video decoder is
 * stopped after its end, as by LinuxDvbStop(). */
static void event_thread_short_poll(void)
{
    long ms;
    poll_sleep_ms = 200;
    start_buffered();
    request_count = 0;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_DONE);
    assert(!stopped_during_poll && event_fd_closed);
    stopped_and_released(VIDEO_FD, VIDEO_CLEAR_BUFFER, VIDEO_STOP, VIDEO_SELECT_SOURCE);
    stopped_and_released(AUDIO_FD, AUDIO_CLEAR_BUFFER, AUDIO_STOP, AUDIO_SELECT_SOURCE);
}

/* The event thread sleeps in the poll longer than the wait for its end:
 * the video device is left alone, the audio device is done. */
static void event_thread_long_poll(void)
{
    long ms;
    poll_sleep_ms = 5000;
    start_buffered();
    request_count = 0;
    assert(fallback(&ms) == LINUXDVB_FALLBACK_PARTIAL);
    assert(ms < LINUXDVB_FALLBACK_STOP_MS);
    assert(!stopped_during_poll);
    assert(count(VIDEO_FD, VIDEO_STOP) == 0 && count(VIDEO_FD, RELEASED) == 0);
    assert(videofd == VIDEO_FD);
    stopped_and_released(AUDIO_FD, AUDIO_CLEAR_BUFFER, AUDIO_STOP, AUDIO_SELECT_SOURCE);
}

/* (d) The normal stop ("q", a single signal, the end of the file) is the one
 * of before: the same requests in the same order, closed with close(), no
 * /dev/null. */
static void normal_stop(void)
{
    static const struct request expected[] = {
        {VIDEO_FD, VIDEO_CLEAR_BUFFER, -1}, {VIDEO_FD, VIDEO_STOP, -1},
        {VIDEO_FD, VIDEO_SLOWMOTION, -1}, {VIDEO_FD, VIDEO_FAST_FORWARD, -1},
        {VIDEO_FD, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX},
        {AUDIO_FD, AUDIO_CLEAR_BUFFER, -1}, {AUDIO_FD, AUDIO_STOP, -1},
        {AUDIO_FD, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX},
        {VIDEO_FD, CLOSED, -1}, {AUDIO_FD, CLOSED, -1},
    };
    size_t i;
    start();
    request_count = 0;
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(request_count == (int)(sizeof(expected) / sizeof(expected[0])));
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        assert(requests[i].fd == expected[i].fd && requests[i].request == expected[i].request &&
               requests[i].arg == expected[i].arg);
    assert(!null_opened && videofd == -1 && audiofd == -1);
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
    fprintf(stderr, "fallback_stop: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("fallback with both devices open", devices_open);
    failed += run("fallback with an AUDIO_STOP that hangs", audio_stop_hangs);
    failed += run("fallback after the normal close", closed_already);
    failed += run("fallback after the close of the audio device", audio_closed_already);
    failed += run("fallback while another thread keeps the lock", lock_kept);
    failed += run("fallback with a device left open", left_open);
    failed += run("fallback while the event thread polls shortly", event_thread_short_poll);
    failed += run("fallback while the event thread polls long", event_thread_long_poll);
    failed += run("normal stop and close", normal_stop);
    return failed ? 1 : 0;
}
