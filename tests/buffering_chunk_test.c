#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#define printf test_printf
int test_printf(const char *format, ...);
#include "../output/linuxdvb_buffering.c"
#undef printf

static int8_t stop_requested(void);

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stop_requested();
}

static void blocked(int number)
{
    static const char text[] = "BufferingWriteV waits for room a single chunk can never get\n";
    (void)number;
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(2);
    _exit(1);
}

/* Output follow-up: the end of the buffering thread, an interrupted select(). */

static int stopped, stop_in_flush, interrupts;
static pthread_t command_thread;
static const char *hang;

static sem_t write_release;

/* With stop_in_flush the playback is terminated from another thread right
 * after the flush on the command thread has seen it alive; the write() that
 * sleeps in the driver returns then. */
static int8_t stop_requested(void)
{
    if (stop_in_flush && g_bDuringWrite && pthread_equal(pthread_self(), command_thread))
    {
        stop_in_flush = 0;
        stopped = 1;
        WriteWakeUp();
        sem_post(&write_release);
        return 0;
    }
    return stopped;
}

static void thread_has_started(void);

bool PlaybackDieNowRegisterCallback(PlaybackDieNowCallback callback)
{
    (void)callback;
    thread_has_started();
    return true;
}

/* The device of the Vu+ driver model: its poll callback sleeps in the driver
 * while the device has no room, up to 5 s when a pause comes meanwhile, and
 * neither the wake-up pipe, the continue nor a clear ends that sleep (box
 * test, Uno 4K SE). Here a select() with driver_fd in its write set sleeps
 * while that device is full until driver_wakes is posted, 2 s at most. */
static int driver_fd = -1, in_driver;
static sem_t driver_wakes;

/* The device whose write() sleeps in the driver until write_release is
 * posted: a write that runs. */
static int write_sleep_fd = -1, in_device_write;

ssize_t write(int fd, const void *data, size_t size)
{
    if (fd >= 0 && fd == __atomic_load_n(&write_sleep_fd, __ATOMIC_SEQ_CST))
    {
        __atomic_store_n(&in_device_write, 1, __ATOMIC_SEQ_CST);
        while (sem_wait(&write_release) && errno == EINTR)
            ;
        __atomic_store_n(&in_device_write, 0, __ATOMIC_SEQ_CST);
    }
    return syscall(SYS_write, fd, data, size);
}

/* Replaces select() of the C library: WriteWithRetry() can be interrupted. */
int select(int count, fd_set *readable, fd_set *writable, fd_set *failed, struct timeval *timeout)
{
    struct timespec limit;
    int device = __atomic_load_n(&driver_fd, __ATOMIC_SEQ_CST);
    if (interrupts)
    {
        interrupts--;
        errno = EINTR;
        return -1;
    }
    if (writable && device >= 0 && device < count && FD_ISSET(device, writable))
    {
        struct pollfd room = {device, POLLOUT, 0};
        if (poll(&room, 1, 0) == 0)
        {
            clock_gettime(CLOCK_REALTIME, &limit);
            limit.tv_sec += 2;
            __atomic_store_n(&in_driver, 1, __ATOMIC_SEQ_CST);
            while (sem_timedwait(&driver_wakes, &limit) && errno == EINTR)
                ;
            __atomic_store_n(&in_driver, 0, __ATOMIC_SEQ_CST);
            limit.tv_sec = 0;
            limit.tv_nsec = 0;
            return pselect(count, readable, writable, failed, &limit, NULL);
        }
    }
    if (timeout)
    {
        limit.tv_sec = timeout->tv_sec;
        limit.tv_nsec = timeout->tv_usec * 1000;
    }
    return pselect(count, readable, writable, failed, timeout ? &limit : NULL, NULL);
}

static void hung(int number)
{
    (void)number;
    if (write(2, hang, strlen(hang)) < 0)
        _exit(2);
    _exit(1);
}

static bool thread_writes(void)
{
    bool result;
    pthread_mutex_lock(&bufferingMtx);
    result = g_bDuringWrite;
    pthread_mutex_unlock(&bufferingMtx);
    return result;
}

static bool thread_ended(void)
{
    return !hasBufferingThreadStarted && g_pfd[1] == -1;
}

static void wait_for(bool (*event)(void), const char *otherwise)
{
    hang = otherwise;
    alarm(5);
    while (!event())
        usleep(1000);
    alarm(0);
}

static void interrupted_select_is_repeated(Context_t *context)
{
    int wake[2], output[2];
    char data = 0;
    assert(!pipe(wake) && !pipe(output));
    assert(fcntl(output[0], F_SETFL, O_NONBLOCK) >= 0);
    interrupts = 1;
    assert(!WriteWithRetry(context, wake[0], output[1], NULL, "v", 1, 1));
    assert(!interrupts && read(output[0], &data, 1) == 1 && data == 'v');
    close(wake[0]);
    close(wake[1]);
    close(output[0]);
    close(output[1]);
}

static void thread_end(void)
{
    static char block[4096];
    uint8_t frame[256] = {1, 2, 3};
    struct iovec vector = {frame, sizeof(frame)};
    PlaybackHandler_t playback = {0};
    Context_t context = {.playback = &playback};
    int decoder[2], room[2];

    interrupted_select_is_repeated(&context);
    /* A decoder that takes no data: the write of the thread does not return. */
    assert(!pipe(decoder) && fcntl(decoder[1], F_SETFL, O_NONBLOCK) >= 0);
    while (write(decoder[1], block, sizeof(block)) > 0)
        ;
    assert(errno == EAGAIN);
    signal(SIGALRM, hung);
    bufferingQueueHead = bufferingQueueTail = NULL;
    bufferingDataSize = 0;
    videofd = -1;

    /* A close without a stop before it, as after a failed OUTPUT_PLAY. */
    assert(!LinuxDvbBuffOpen(&context, "video", decoder[1], NULL));
    assert(!LinuxDvbBuffClose(&context));
    wait_for(thread_ended, "LinuxDvbBuffClose has not ended the buffering thread\n");

    /* A flush that waits for the write while the playback is terminated:
     * a device with room whose write() sleeps in the driver. */
    assert(!pipe(room) && fcntl(room[1], F_SETFL, O_NONBLOCK) >= 0);
    assert(!sem_init(&write_release, 0, 0));
    __atomic_store_n(&write_sleep_fd, room[1], __ATOMIC_SEQ_CST);
    assert(!LinuxDvbBuffOpen(&context, "video", room[1], NULL));
    assert(BufferingWriteV(room[1], &vector, 1) == sizeof(frame));
    wait_for(thread_writes, "the buffering thread does not write the chunk\n");
    command_thread = pthread_self();
    stop_in_flush = 1;
    hang = "LinuxDvbBuffFlush still waits for a thread that has ended\n";
    alarm(5);
    assert(!LinuxDvbBuffFlush(&context));
    alarm(0);
    assert(stopped && !stop_in_flush);
    wait_for(thread_ended, "the buffering thread has not ended with the playback\n");
    __atomic_store_n(&write_sleep_fd, -1, __ATOMIC_SEQ_CST);

    /* Not queued any more: the result is the size of the data, as for a queued chunk. */
    assert(BufferingWriteV(room[1], &vector, 1) == sizeof(frame));
    assert(!bufferingQueueHead && !bufferingDataSize);
    stopped = 0;
    assert(!LinuxDvbBuffClose(&context));
    close(room[0]);
    close(room[1]);
    sem_destroy(&write_release);
    close(decoder[0]);
    close(decoder[1]);
}

/* Output follow-up: the end of the thread as the close and the next open see it. */

static sem_t held, resume;
static int hold_exit, hold_created, thread_runs;

/* Called by the thread once it has made its wake-up pipe. */
static void thread_has_started(void)
{
    thread_runs = 1;
}

static bool thread_gone(void)
{
    return thread_runs && g_pfd[1] == -1;
}

/* The messages of linuxdvb_buffering.c: its thread prints "EXIT" as the last
 * thing it does, the open prints "Created thread" after pthread_create(). */
int test_printf(const char *format, ...)
{
    va_list arguments;
    int result;
    if (hold_exit && strstr(format, "] EXIT\n"))
    {
        hold_exit = 0;
        sem_post(&held);
        sem_wait(&resume);
    }
    if (hold_created && strstr(format, "Created thread"))
    {
        hold_created = 0;
        wait_for(thread_gone, "the buffering thread has not ended with the playback\n");
    }
    va_start(arguments, format);
    result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

static void chunk_arrives(Context_t *context, int decoder[2], uint8_t value)
{
    uint8_t frame[256] = {value}, arrived[256];
    struct iovec vector = {frame, sizeof(frame)};
    assert(BufferingWriteV(decoder[1], &vector, 1) == sizeof(frame));
    hang = "the buffering thread does not write the chunk\n";
    alarm(5);
    assert(read(decoder[0], arrived, sizeof(arrived)) == sizeof(arrived));
    alarm(0);
    assert(!memcmp(arrived, frame, sizeof(frame)));
    assert(!LinuxDvbBuffClose(context));
}

/* The thread is still at its last message when the close returns and the
 * output is opened again: the close has seen its end, the open starts a
 * thread, and that thread keeps its wake-up pipe. */
static void open_while_the_thread_ends(Context_t *context, int decoder[2])
{
    hold_exit = 1;
    assert(!LinuxDvbBuffOpen(context, "video", decoder[1], NULL));
    assert(!LinuxDvbBuffClose(context));
    assert(!LinuxDvbBuffOpen(context, "video", decoder[1], NULL));
    hang = "the buffering thread has not come to its end\n";
    alarm(5);
    assert(!sem_wait(&held));
    alarm(0);
    sem_post(&resume);
    chunk_arrives(context, decoder, 4);
}

/* The playback is being stopped: the thread has ended before the open goes
 * on behind pthread_create(). No thread is left to close, and the next open
 * starts one. */
static void open_while_the_playback_stops(Context_t *context, int decoder[2])
{
    stopped = 1;
    thread_runs = 0;
    hold_created = 1;
    assert(!LinuxDvbBuffOpen(context, "video", decoder[1], NULL));
    assert(!hold_created);
    stopped = 0;
    assert(!LinuxDvbBuffClose(context));
    assert(!LinuxDvbBuffOpen(context, "video", decoder[1], NULL));
    chunk_arrives(context, decoder, 5);
}

/* A PES header and its payload given as segments reach the decoder in a write()
 * of their own each, as writev() hands them to a driver: the decoder is a
 * packet socket here, every write() of the thread is one packet. An empty
 * segment is not written. */
static void segments_written_one_by_one(void)
{
    PlaybackHandler_t playback = {0};
    Context_t context = {.playback = &playback};
    uint8_t header[14] = {0, 0, 1, 0xc0}, payload[300], arrived[512];
    struct iovec vector[3] = {{header, sizeof(header)}, {payload, 0}, {payload, sizeof(payload)}};
    int decoder[2];
    size_t i;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, decoder));
    signal(SIGALRM, hung);
    stopped = 0;
    assert(audiofd == -1);
    assert(!LinuxDvbBuffOpen(&context, "audio", decoder[1], NULL));
    assert(BufferingWriteV(decoder[1], vector, 3) == sizeof(header) + sizeof(payload));
    hang = "the buffering thread does not write the segments\n";
    alarm(5);
    assert(read(decoder[0], arrived, sizeof(arrived)) == sizeof(header));
    assert(!memcmp(arrived, header, sizeof(header)));
    assert(read(decoder[0], arrived, sizeof(arrived)) == sizeof(payload));
    assert(!memcmp(arrived, payload, sizeof(payload)));
    alarm(0);
    assert(!LinuxDvbBuffClose(&context));
    close(decoder[0]);
    close(decoder[1]);
}

/* An audio switch without a seek (live, paused) sets isSeeking and keeps the
 * stamp: the video queued meanwhile reaches the decoder after the switch,
 * the queued audio of the old track does not. A seek changes the stamp:
 * the video queued before it is not written. */
static void switch_keeps_video(void)
{
    PlaybackHandler_t playback = {0};
    Context_t context = {.playback = &playback};
    uint8_t frame[64] = {0}, arrived[64];
    struct iovec vector = {frame, sizeof(frame)};
    int video[2], audio[2];

    assert(!pipe(video) && !pipe(audio));
    assert(fcntl(audio[0], F_SETFL, O_NONBLOCK) >= 0);
    signal(SIGALRM, hung);
    stopped = 0;
    maxBufferingDataSize = 65536;
    LinuxDvbBuffSetStamp(NULL);
    playback.isSeeking = 1;
    assert(!LinuxDvbBuffOpen(&context, "video", video[1], NULL));
    assert(!LinuxDvbBuffOpen(&context, "audio", audio[1], NULL));
    frame[0] = 1;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    frame[0] = 2;
    assert(BufferingWriteV(audio[1], &vector, 1) == sizeof(frame));
    frame[0] = 3;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    usleep(100000);
    playback.isSeeking = 0;
    hang = "the video queued during an audio switch is not written\n";
    alarm(5);
    assert(read(video[0], arrived, sizeof(arrived)) == sizeof(arrived) && arrived[0] == 1);
    assert(read(video[0], arrived, sizeof(arrived)) == sizeof(arrived) && arrived[0] == 3);
    alarm(0);
    assert(read(audio[0], arrived, sizeof(arrived)) == -1 && errno == EAGAIN);

    /* a seek: the stamp changes while the video waits */
    playback.isSeeking = 1;
    frame[0] = 4;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    usleep(50000);
    playback.stamp = (void *)1;
    usleep(50000);
    LinuxDvbBuffSetStamp((void *)1);
    playback.isSeeking = 0;
    frame[0] = 5;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    hang = "the video queued after a seek is not written\n";
    alarm(5);
    assert(read(video[0], arrived, sizeof(arrived)) == sizeof(arrived) && arrived[0] == 5);
    alarm(0);
    LinuxDvbBuffSetStamp(NULL);
    assert(!LinuxDvbBuffClose(&context));
    close(video[0]);
    close(video[1]);
    close(audio[0]);
    close(audio[1]);
}

static bool thread_in_driver(void)
{
    return __atomic_load_n(&in_driver, __ATOMIC_SEQ_CST);
}

static void fill(int fd)
{
    static char block[4096];
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) >= 0);
    while (write(fd, block, sizeof(block)) > 0)
        ;
    assert(errno == EAGAIN);
}

/* VIDEO_CLEAR_BUFFER / AUDIO_CLEAR_BUFFER */
static void clear(int fd)
{
    static char block[4096];
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) >= 0);
    while (read(fd, block, sizeof(block)) > 0)
        ;
    assert(errno == EAGAIN);
}

static double flush_ms(Context_t *context, int32_t (*flush)(Context_t *))
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(!flush(context));
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

static void quick(double ms, const char *what)
{
    if (ms > 200)
    {
        fprintf(stderr, "%s waited %.0f ms for the wait for room in the device\n", what, ms);
        exit(1);
    }
}

/* The first chunk that reaches the device: the one queued after the clear. */
static void next_chunk_is(int fd, uint8_t value)
{
    uint8_t arrived[256];
    struct pollfd data = {fd, POLLIN, 0};
    assert(poll(&data, 1, 3000) == 1);
    assert(read(fd, arrived, sizeof(arrived)) == sizeof(arrived));
    if (arrived[0] != value)
    {
        fprintf(stderr, "chunk %u reached the device after the flush and the clear, not chunk %u\n",
                arrived[0], value);
        exit(1);
    }
    usleep(50000);
    assert(read(fd, arrived, sizeof(arrived)) == -1 && errno == EAGAIN);
}

/* -b on the Vu+: the thread waits for room in a full decoder, a pause comes,
 * then a seek or an audio switch. The flush returns at once instead of
 * waiting for the end of the driver's sleep, and the chunk the thread was
 * waiting with does not reach the device after the flush and the clear that
 * follows it; the data queued after the clear does. */
static void flush_while_the_driver_sleeps(void)
{
    PlaybackHandler_t playback = {0};
    Context_t context = {.playback = &playback};
    uint8_t frame[256] = {0};
    struct iovec vector = {frame, sizeof(frame)};
    int video[2], audio[2];

    assert(!pipe(video) && !pipe(audio) && !sem_init(&driver_wakes, 0, 0));
    fill(video[1]);
    fill(audio[1]);
    signal(SIGALRM, hung);
    stopped = 0;
    maxBufferingDataSize = 65536;
    playback.stamp = (void *)1;
    LinuxDvbBuffSetStamp((void *)1);
    assert(!LinuxDvbBuffOpen(&context, "video", video[1], NULL));
    assert(!LinuxDvbBuffOpen(&context, "audio", audio[1], NULL));

    /* a seek: new stamp, flush, clear, the data of the new position */
    __atomic_store_n(&driver_fd, video[1], __ATOMIC_SEQ_CST);
    frame[0] = 1;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    wait_for(thread_in_driver, "the buffering thread does not wait for room in the video device\n");
    playback.isSeeking = 1;
    playback.stamp = (void *)2;
    quick(flush_ms(&context, LinuxDvbBuffFlush), "LinuxDvbBuffFlush");
    clear(video[0]);
    LinuxDvbBuffSetStamp((void *)2);
    playback.isSeeking = 0;
    frame[0] = 2;
    assert(BufferingWriteV(video[1], &vector, 1) == sizeof(frame));
    sem_post(&driver_wakes);
    next_chunk_is(video[0], 2);

    /* a DASH audio switch: the audio is flushed and cleared */
    __atomic_store_n(&driver_fd, audio[1], __ATOMIC_SEQ_CST);
    frame[0] = 3;
    assert(BufferingWriteV(audio[1], &vector, 1) == sizeof(frame));
    wait_for(thread_in_driver, "the buffering thread does not wait for room in the audio device\n");
    quick(flush_ms(&context, LinuxDvbBuffFlushAudio), "LinuxDvbBuffFlushAudio");
    clear(audio[0]);
    frame[0] = 4;
    assert(BufferingWriteV(audio[1], &vector, 1) == sizeof(frame));
    sem_post(&driver_wakes);
    next_chunk_is(audio[0], 4);

    __atomic_store_n(&driver_fd, -1, __ATOMIC_SEQ_CST);
    LinuxDvbBuffSetStamp(NULL);
    assert(!LinuxDvbBuffClose(&context));
    close(video[0]);
    close(video[1]);
    close(audio[0]);
    close(audio[1]);
    sem_destroy(&driver_wakes);
}

static void close_and_open(void)
{
    PlaybackHandler_t playback = {0};
    Context_t context = {.playback = &playback};
    int decoder[2];

    assert(!pipe(decoder) && !sem_init(&held, 0, 0) && !sem_init(&resume, 0, 0));
    signal(SIGALRM, hung);
    BUFF_DEBUG_LEVEL = 20;
    open_while_the_thread_ends(&context, decoder);
    open_while_the_playback_stops(&context, decoder);
    BUFF_DEBUG_LEVEL = 0;
    close(decoder[0]);
    close(decoder[1]);
}

int main(void)
{
    uint8_t frame[256] = {1, 2, 3};
    struct iovec vector = {frame, sizeof(frame)};
    BufferingNode_t *node;

    assert(!pthread_mutex_init(&bufferingMtx, NULL));
    assert(!pthread_cond_init(&bufferingDataConsumedCond, NULL));
    assert(!pthread_cond_init(&bufferingdDataAddedCond, NULL));
    videofd = 7;
    maxBufferingDataSize = 128;
    signal(SIGALRM, blocked);
    alarm(3);
    /* One frame larger than the whole buffer, with nothing queued to be consumed. */
    assert(BufferingWriteV(videofd, &vector, 1) == sizeof(frame));
    alarm(0);
    node = bufferingQueueHead;
    assert(node && node == bufferingQueueTail && !node->next);
    assert(node->dataType == OUTPUT_VIDEO && node->dataSize == sizeof(frame));
    assert(bufferingDataSize == sizeof(*node) + sizeof(frame));
    free(node);
    pthread_mutex_destroy(&bufferingMtx);
    pthread_cond_destroy(&bufferingDataConsumedCond);
    pthread_cond_destroy(&bufferingdDataAddedCond);
    thread_end();
    close_and_open();
    segments_written_one_by_one();
    switch_keeps_video();
    flush_while_the_driver_sleeps();
    return 0;
}
