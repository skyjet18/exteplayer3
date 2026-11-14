#include <assert.h>
#include <errno.h>
#include <pthread.h>

/* The container starts its threads through this. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument);
#define pthread_create test_pthread_create
#include "../container/container_ffmpeg.c"
#undef pthread_create

static int create_fails, threads_started, threads_ended;

/* A real thread that has run to its end when the caller goes on. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument)
{
    (void)attributes;
    if (create_fails)
        return EAGAIN;
    threads_started++;
    assert(!pthread_create(thread, NULL, start, argument));
    assert(!pthread_join(*thread, NULL));
    return 0;
}

/* The last call of the play thread but for clearing its flag. */
int8_t PlaybackDieNow(int8_t value)
{
    threads_ended += value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

int64_t PlaybackLastContinue(void)
{
    return 0;
}

void LinuxDvbBuffSetStamp(void *stamp)
{
    (void)stamp;
}

/* The play thread picks the live sync default of the box. */
stb_type_t GetSTBType(void)
{
    return STB_UNKNOWN;
}

static int32_t output_command(void *opaque, OutputCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == OUTPUT_GET_BUFFER_SIZE);
    *(uint32_t *)argument = 0;
    return 0;
}

static PlaybackHandler_t playback;
static OutputHandler_t output = {.Command = output_command};
static Context_t context = {.playback = &playback, .output = &output};

/* The playback is not playing any more: the thread ends before
 * container_ffmpeg_play() goes on behind pthread_create(). No thread is
 * left, so the stop has nothing to wait for and the next play starts one. */
static void thread_ends_at_once(void)
{
    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 1 && threads_ended == 1);
    assert(!hasPlayThreadStarted);

    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 2 && threads_ended == 2);
    assert(!hasPlayThreadStarted);

    isContainerRunning = 1;
    assert(!container_ffmpeg_stop(&context));
    assert(!isContainerRunning);
}

/* No thread could be created: none is reported, the next play starts one. */
static void thread_is_not_created(void)
{
    create_fails = 1;
    assert(container_ffmpeg_play(&context) == cERR_CONTAINER_FFMPEG_ERR);
    assert(threads_started == 2 && !hasPlayThreadStarted);
    create_fails = 0;
    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 3 && threads_ended == 3 && !hasPlayThreadStarted);
}

int main(void)
{
    thread_ends_at_once();
    thread_is_not_created();
    return 0;
}
