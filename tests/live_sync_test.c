#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* The container starts its threads through this. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument);
#define pthread_create test_pthread_create
#include "../container/container_ffmpeg.c"
#undef pthread_create

/* The live start hold of the container: who holds the output at the first
 * video packet of a stream without a duration. The play thread, which ends at
 * once as nothing is playing, takes the default of the box; the first video
 * packet then decides. Every case runs in a child of its own. */

static int threads_started, pauses, continues;
static stb_type_t stb = STB_OTHER;

/* A real thread that has run to its end when the caller goes on. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument)
{
    (void)attributes;
    threads_started++;
    assert(!pthread_create(thread, NULL, start, argument));
    assert(!pthread_join(*thread, NULL));
    return 0;
}

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
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

stb_type_t GetSTBType(void)
{
    return stb;
}

static int32_t output_command(void *opaque, OutputCmd_t command, void *argument)
{
    (void)opaque;
    if (command == OUTPUT_PAUSE)
        pauses++;
    else if (command == OUTPUT_CONTINUE)
        continues++;
    else {
        assert(command == OUTPUT_GET_BUFFER_SIZE);
        *(uint32_t *)argument = 0;
    }
    return 0;
}

static PlaybackHandler_t playback;
static OutputHandler_t output = {.Command = output_command};
static Context_t context = {.playback = &playback, .output = &output};

/* The first video packet of a live stream. */
static int first_video_packet_holds(void)
{
    AVFormatContext live = {.duration = AV_NOPTS_VALUE};
    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 1 && !pauses);
    avContextTab[0] = &live;
    ls_on_video(&context, 90000);
    avContextTab[0] = NULL;
    assert(ls_state == 2);
    if (!pauses)
        return 0;
    /* held: the watch thread was started and has released the hold */
    assert(pauses == 1 && threads_started == 2);
    return 1;
}

static void other_box(void)
{
    assert(!first_video_packet_holds());
}

static void dreambox(void)
{
    stb = STB_DREAMBOX;
    assert(first_video_packet_holds());
}

static void enabled_on_other_box(void)
{
    assert(!setenv("EP3_LIVE_SYNC", "1", 1));
    assert(first_video_packet_holds());
}

static void disabled_on_dreambox(void)
{
    stb = STB_DREAMBOX;
    assert(!setenv("EP3_LIVE_SYNC", "0", 1));
    assert(!first_video_packet_holds());
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
    fprintf(stderr, "live_sync: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_LIVE_SYNC");
    failed += run("no hold by default on another box", other_box);
    failed += run("hold by default on a Dreambox", dreambox);
    failed += run("EP3_LIVE_SYNC=1 on another box", enabled_on_other_box);
    failed += run("EP3_LIVE_SYNC=0 on a Dreambox", disabled_on_dreambox);
    return failed ? 1 : 0;
}
