#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "common.h"
#include "misc.h"
#include "writer.h"

/* writev_with_retry() on a device that takes nothing, as a paused decoder:
 * it waits in poll() for room, for WriterWakeUp() or for a stop instead of
 * retrying every millisecond. A non-blocking pipe stands in for the device. */

static volatile int stopped;
static PlaybackDieNowCallback dieNowCallback;

int8_t PlaybackDieNow(int8_t value)
{
    if (value && !stopped)
    {
        stopped = 1;
        if (dieNowCallback)
            dieNowCallback();
    }
    return stopped;
}

bool PlaybackDieNowRegisterCallback(PlaybackDieNowCallback callback)
{
    dieNowCallback = callback;
    return true;
}

typedef struct {
    ssize_t ret;
    int err;
    long switches;   /* voluntary context switches of the writing thread */
    long cpu_us;
    int64_t end_us;
} Result_t;

static int device[2];
static uint8_t chunk[3000];

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long usage_us(const struct rusage *usage)
{
    return (long)(usage->ru_utime.tv_sec + usage->ru_stime.tv_sec) * 1000000L +
           usage->ru_utime.tv_usec + usage->ru_stime.tv_usec;
}

static void *write_chunk(void *arg)
{
    Result_t *result = arg;
    struct iovec iov = {chunk, sizeof(chunk)};
    struct rusage before, after;
    getrusage(RUSAGE_THREAD, &before);
    result->ret = writev_with_retry(device[1], &iov, 1);
    result->err = errno;
    getrusage(RUSAGE_THREAD, &after);
    result->switches = after.ru_nvcsw - before.ru_nvcsw;
    result->cpu_us = usage_us(&after) - usage_us(&before);
    result->end_us = now_us();
    return NULL;
}

/* the device is full: a chunk does not fit any more */
static void fill(void)
{
    while (write(device[1], chunk, sizeof(chunk)) > 0);
    assert(errno == EAGAIN);
}

static void drain(void)
{
    uint8_t buf[65536];
    while (read(device[0], buf, sizeof(buf)) > 0);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR);
}

int main(void)
{
    pthread_t thread;
    Result_t result;
    int64_t start;

    assert(!pipe(device));
    assert(fcntl(device[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(device[1], F_SETFL, O_NONBLOCK) == 0);
    memset(chunk, 0x47, sizeof(chunk));

    /* 1 s on a full device, then room: hardly a wake-up meanwhile (it was a
     * thousand), and the chunk goes as soon as there is room */
    memset(&result, 0, sizeof(result));
    fill();
    assert(!pthread_create(&thread, NULL, write_chunk, &result));
    sleep_ms(1000);
    assert(result.end_us == 0);
    start = now_us();
    drain();
    assert(!pthread_join(thread, NULL));
    printf("writer_wait: 1 s on a full device: %ld voluntary context switches, %ld us CPU, "
           "written %lld us after the room\n", result.switches, result.cpu_us,
           (long long)(result.end_us - start));
    assert(result.ret == (ssize_t)sizeof(chunk));
    assert(result.end_us - start < 50000);
    assert(result.switches <= 30);

    /* WriterWakeUp() (a resume) makes it look again, still full: it waits on */
    memset(&result, 0, sizeof(result));
    drain();
    fill();
    assert(!pthread_create(&thread, NULL, write_chunk, &result));
    sleep_ms(200);
    WriterWakeUp();
    WriterWakeUp();
    sleep_ms(200);
    assert(result.end_us == 0);

    /* a stop ends the wait at once */
    start = now_us();
    PlaybackDieNow(1);
    assert(!pthread_join(thread, NULL));
    printf("writer_wait: stop while waiting: returned %lld us after it\n",
           (long long)(result.end_us - start));
    assert(result.ret == -1 && result.err == ECANCELED);
    assert(result.end_us - start < 50000);
    assert(result.switches <= 30);
    return 0;
}
