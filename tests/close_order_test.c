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
#define poll test_poll
int test_poll(struct pollfd *fds, nfds_t count, int timeout);
#define dup test_dup
int test_dup(int fd);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef open
#undef close
#undef poll
#undef dup
#ifndef VIDEO_EVENT_END_MS
#define VIDEO_EVENT_END_MS 500
#endif

/* The order of the stop and close of the DVB output against threads of the
 * player that sleep in a poll of the driver (the Vu+ driver sleeps there up
 * to 200 ms while the decoder is full, up to 5 s when a pause comes
 * meanwhile, and cannot be woken). The decoder must not be stopped and its
 * device not closed while such a poll runs; a poll that outlasts the wait
 * leaves the device open. Every case runs in a child of its own. */

#define VIDEO_FD 10
#define AUDIO_FD 11
/* the copy of videofd the event thread polls (-b) */
#define EVENT_FD (VIDEO_FD + 100)
#define CLOSED 0xc105edUL
/* LinuxDvbBuffClose(), recorded without a device */
#define BUFFERING_CLOSE 0xbcUL

struct request { int fd; unsigned long request; };
static struct request requests[256];
static int request_count;

/* the driver poll of the event thread */
static volatile int poll_sleep_ms;
static volatile int in_poll, polls, event_fd_closed, video_stopped;
/* a decoder stop, a close or a poll at the wrong time */
static volatile int stopped_during_poll, closed_during_poll, polls_after_stop;

static long ms_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static void record(int fd, unsigned long request)
{
    assert(request_count < 256);
    requests[request_count++] = (struct request){fd, request};
}

int test_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == VIDEO_FD || fd == AUDIO_FD);
    if (fd == VIDEO_FD && (request == VIDEO_STOP || request == VIDEO_CLEAR_BUFFER))
    {
        if (in_poll)
            stopped_during_poll++;
        if (request == VIDEO_STOP)
            video_stopped = 1;
    }
    record(fd, request);
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
    if (fd == EVENT_FD)
    {
        assert(!event_fd_closed);
        event_fd_closed = 1;
        return 0;
    }
    assert(fd == VIDEO_FD || fd == AUDIO_FD);
    if (fd == VIDEO_FD && in_poll)
        closed_during_poll++;
    record(fd, CLOSED);
    return 0;
}

int test_dup(int fd)
{
    assert(fd == VIDEO_FD);
    return EVENT_FD;
}

/* The poll of the Vu+ driver: it sleeps, whatever happens meanwhile. */
int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == EVENT_FD && fds[0].events == POLLPRI && !timeout);
    if (event_fd_closed || video_stopped)
        polls_after_stop++;
    in_poll = 1;
    __sync_fetch_and_add(&polls, 1);
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
/* -1: the buffering thread has not ended within the wait (it sleeps in the driver) */
static int buffering_close_result;
int32_t LinuxDvbBuffClose(Context_t *context)
{
    (void)context;
    record(-1, BUFFERING_CLOSE);
    return buffering_close_result;
}
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

/* -b, playing, the event thread started by the first video frame */
static void start_buffered(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    isBufferedOutput = true;
    both(OUTPUT_OPEN);
    assert(videofd == VIDEO_FD && audiofd == AUDIO_FD);
    both(OUTPUT_PLAY);
    assert(LinuxDvbEventThreadPolls(&context));
}

static void wait_for_poll(void)
{
    int i;
    for (i = 0; i < 2000 && !in_poll; i++)
        usleep(1000);
    assert(in_poll);
}

/* "q" while the event thread sleeps in the poll (300 ms): the stop waits for
 * the end of that poll, then stops the decoder and closes the device; the
 * thread polls no more and has closed its copy by then. */
static void event_thread_ends_before_the_decoder_stops(void)
{
    struct timespec start;
    long ms;
    poll_sleep_ms = 300;
    start_buffered();
    wait_for_poll();
    clock_gettime(CLOCK_MONOTONIC, &start);
    both(OUTPUT_STOP);
    assert(event_fd_closed);
    both(OUTPUT_CLOSE);
    ms = ms_since(&start);
    fprintf(stderr, "close_order: stop and close %ld ms after the poll began to sleep\n", ms);
    assert(!stopped_during_poll && !closed_during_poll && !polls_after_stop);
    assert(count(VIDEO_FD, VIDEO_STOP) == 1 && count(VIDEO_FD, CLOSED) == 1);
    assert(count(AUDIO_FD, AUDIO_STOP) == 1 && count(AUDIO_FD, CLOSED) == 1);
    assert(ms < 450);
}

/* "q" between two polls: the stop ends the wait of the thread at once. */
static void stop_ends_the_wait_between_two_polls(void)
{
    struct timespec start;
    long ms;
    int i, before;
    start_buffered();
    for (i = 0; i < 2000 && !polls; i++)
        usleep(1000);
    assert(polls);
    usleep(50000);
    clock_gettime(CLOCK_MONOTONIC, &start);
    both(OUTPUT_STOP);
    ms = ms_since(&start);
    fprintf(stderr, "close_order: stop %ld ms, the thread was between two polls\n", ms);
    assert(event_fd_closed);
    assert(ms < 100);
    before = polls;
    usleep(300000);
    assert(polls == before && !polls_after_stop);
    both(OUTPUT_CLOSE);
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
}

/* A poll that sleeps longer than the wait (as after a pause): the stop goes
 * on after about half a second, and the video device is left open for the
 * end of the process; the thread closes its copy once its poll returns. */
static void long_poll_leaves_the_video_device_open(void)
{
    struct timespec start;
    long ms;
    poll_sleep_ms = 1500;
    start_buffered();
    wait_for_poll();
    clock_gettime(CLOCK_MONOTONIC, &start);
    both(OUTPUT_STOP);
    ms = ms_since(&start);
    both(OUTPUT_CLOSE);
    fprintf(stderr, "close_order: stop after %ld ms with the thread still in the poll\n", ms);
    assert(ms >= VIDEO_EVENT_END_MS - 50 && ms < VIDEO_EVENT_END_MS + 300);
    assert(in_poll && !event_fd_closed);
    assert(count(VIDEO_FD, CLOSED) == 0 && !closed_during_poll && videofd == -1);
    assert(count(AUDIO_FD, CLOSED) == 1);
    usleep(1300000);
    assert(!in_poll && event_fd_closed && !polls_after_stop);
}

/* Without -b no thread polls: nothing to wait for, closed as before. */
static void unbuffered_close_does_not_wait(void)
{
    struct timespec start;
    long ms;
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    both(OUTPUT_OPEN);
    both(OUTPUT_PLAY);
    assert(!LinuxDvbEventThreadPolls(&context));
    clock_gettime(CLOCK_MONOTONIC, &start);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    ms = ms_since(&start);
    assert(ms < 50 && !polls);
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
}

static int first(int fd, unsigned long request)
{
    int i;
    for (i = 0; i < request_count; i++)
        if (requests[i].fd == fd && requests[i].request == request)
            return i;
    return -1;
}

/* -b: the buffering thread ends once, before the first decoder is cleared
 * and stopped; then the devices are closed. */
static void buffering_ends_before_the_decoders_stop(void)
{
    int closed;
    start_buffered();
    /* the open clears the decoders as well: only what the stop sends */
    request_count = 0;
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    closed = first(-1, BUFFERING_CLOSE);
    assert(count(-1, BUFFERING_CLOSE) == 1);
    assert(closed >= 0);
    assert(closed < first(VIDEO_FD, VIDEO_CLEAR_BUFFER) && closed < first(VIDEO_FD, VIDEO_STOP));
    assert(closed < first(AUDIO_FD, AUDIO_CLEAR_BUFFER) && closed < first(AUDIO_FD, AUDIO_STOP));
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
}

/* The same with OUTPUT_CLOSE alone (end of file, termination). */
static void buffering_ends_before_the_decoders_stop_on_close(void)
{
    int closed;
    start_buffered();
    request_count = 0;
    both(OUTPUT_CLOSE);
    closed = first(-1, BUFFERING_CLOSE);
    assert(count(-1, BUFFERING_CLOSE) == 1);
    assert(closed >= 0 && closed < first(VIDEO_FD, VIDEO_STOP) && closed < first(AUDIO_FD, AUDIO_STOP));
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
}

/* The buffering thread has not ended within the wait of LinuxDvbBuffClose():
 * the decoders are stopped, but neither device is closed under it, and the
 * close does not wait for it a second time. */
static void buffering_thread_in_the_driver_leaves_the_devices_open(void)
{
    buffering_close_result = -1;
    start_buffered();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(count(-1, BUFFERING_CLOSE) == 1);
    assert(count(VIDEO_FD, VIDEO_STOP) == 1 && count(AUDIO_FD, AUDIO_STOP) == 1);
    assert(count(VIDEO_FD, CLOSED) == 0 && count(AUDIO_FD, CLOSED) == 0);
    assert(videofd == -1 && audiofd == -1);
}

/* A second playback in the same process: buffering and devices closed again. */
static void buffered_second_playback(void)
{
    start_buffered();
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    request_count = 0;
    both(OUTPUT_OPEN);
    both(OUTPUT_PLAY);
    both(OUTPUT_STOP);
    both(OUTPUT_CLOSE);
    assert(count(-1, BUFFERING_CLOSE) == 1);
    assert(count(VIDEO_FD, CLOSED) == 1 && count(AUDIO_FD, CLOSED) == 1);
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
    fprintf(stderr, "close_order: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("event thread ends before the decoder stops", event_thread_ends_before_the_decoder_stops);
    failed += run("stop ends the wait between two polls", stop_ends_the_wait_between_two_polls);
    failed += run("long poll leaves the video device open", long_poll_leaves_the_video_device_open);
    failed += run("unbuffered close does not wait", unbuffered_close_does_not_wait);
    failed += run("buffering ends before the decoders stop", buffering_ends_before_the_decoders_stop);
    failed += run("buffering ends before the decoders stop on close",
                  buffering_ends_before_the_decoders_stop_on_close);
    failed += run("buffering thread in the driver leaves the devices open",
                  buffering_thread_in_the_driver_leaves_the_devices_open);
    failed += run("buffered second playback", buffered_second_playback);
    return failed ? 1 : 0;
}
