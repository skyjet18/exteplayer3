#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define clock_gettime test_clock_gettime
int test_clock_gettime(clockid_t clock, struct timespec *now);
#define usleep test_usleep
int test_usleep(useconds_t delay);
#include "../container/output_pacing.c"
#undef clock_gettime
#undef usleep

/* The optional software playback clock (EP3_OUTPUT_PACING_MS) with a clock
 * that moves only by the sleeps of the code under test. The environment is
 * read once per process, so every case runs in a child of its own. */

static int64_t now_us = 1000000000LL;
static int64_t slept_us;
static int stopped;
/* called after every sleep: the "other thread" of a case */
static void (*after_sleep)(void);

int test_clock_gettime(clockid_t clock, struct timespec *now)
{
    assert(clock == CLOCK_MONOTONIC);
    now->tv_sec = now_us / 1000000;
    now->tv_nsec = (now_us % 1000000) * 1000;
    return 0;
}

int test_usleep(useconds_t delay)
{
    assert(delay > 0 && delay <= 10000);
    now_us += delay;
    slept_us += delay;
    if (after_sleep)
        after_sleep();
    return 0;
}

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stopped;
}

/* The pause waits for a change of the play state, 10 ms of the test clock
 * per wait; the "other thread" of a case runs after each. */
static uint32_t change_serial;
static int change_waits;

uint32_t PlaybackChangeSerial(void)
{
    return change_serial;
}

void PlaybackWaitChange(uint32_t serial, int32_t timeoutMs)
{
    assert(serial == change_serial && timeoutMs > 0 && timeoutMs <= 1000);
    change_waits++;
    now_us += 10000;
    slept_us += 10000;
    if (after_sleep)
        after_sleep();
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Context_t context = {.playback = &playback};

#define SECOND 90000LL
static const int64_t P = 900000; /* 10 s */

/* Returns the time the chunk was held. */
static int64_t held(int32_t stream, int64_t pts)
{
    int64_t before = slept_us;
    OutputPacingWait(&context, stream, pts);
    return slept_us - before;
}

static void lead(const char *ms)
{
    assert(!setenv("EP3_OUTPUT_PACING_MS", ms, 1));
}

/* Off: unset, 0, negative or not a number. */
static void off(void)
{
    assert(OutputPacingLeadMs() == 0);
    assert(!held(PACING_STREAM_VIDEO, P) && !held(PACING_STREAM_VIDEO, P + 30 * SECOND));
}

static void off_negative(void) { lead("-5"); off(); }
static void off_text(void) { lead("fast"); off(); }

static void video_ahead(void)
{
    lead("1000");
    assert(OutputPacingLeadMs() == 1000);
    assert(!held(PACING_STREAM_VIDEO, P));
    /* 3 s ahead of the anchor: held until 1 s ahead */
    assert(held(PACING_STREAM_VIDEO, P + 3 * SECOND) == 2000000);
    /* real time has moved on by those 2 s */
    assert(!held(PACING_STREAM_VIDEO, P + 3 * SECOND));
    assert(held(PACING_STREAM_VIDEO, P + 3 * SECOND + SECOND / 2) == 500000);
}

static void audio_has_more_lead(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    assert(held(PACING_STREAM_AUDIO, P + 3 * SECOND) == 1500000);
}

static void within_the_lead(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    assert(!held(PACING_STREAM_VIDEO, P + SECOND / 2));
    assert(!held(PACING_STREAM_AUDIO, P + SECOND));
}

/* The clock stands during a pause: after 3 s of pause a chunk 2.5 s after
 * the anchor is still 2.5 s ahead and held for 1.5 s. */
static int64_t pause_slept;
static void end_pause_after_3_s(void)
{
    if (playback.isPaused && slept_us - pause_slept >= 3000000)
    {
        playback.isPaused = 0;
        change_serial++;
    }
}

static void pause_stops_the_clock(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    playback.isPaused = 1;
    pause_slept = slept_us;
    after_sleep = end_pause_after_3_s;
    assert(held(PACING_STREAM_VIDEO, P + 5 * SECOND / 2) == 3000000 + 1500000);
    /* the pause waited for the continue, not in sleeps */
    assert(change_waits == 300);
}

/* A seek or a stop ends the wait. */
static void seek_after_half_a_second(void)
{
    if (slept_us >= 500000)
        playback.isSeeking = 1;
}

static void stop_after_half_a_second(void)
{
    if (slept_us >= 500000)
        stopped = 1;
}

static void seek_ends_the_wait(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    after_sleep = seek_after_half_a_second;
    assert(held(PACING_STREAM_VIDEO, P + 3 * SECOND) == 500000);
}

static void stop_ends_the_wait(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    after_sleep = stop_after_half_a_second;
    assert(held(PACING_STREAM_VIDEO, P + 3 * SECOND) == 500000);
}

/* Trick play is not paced and forgets the anchor. */
static void trick_play(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    playback.isForwarding = 1;
    assert(!held(PACING_STREAM_VIDEO, P + 3 * SECOND));
    playback.isForwarding = 0;
    playback.BackWard = 1;
    assert(!held(PACING_STREAM_VIDEO, P + 6 * SECOND));
    playback.BackWard = 0;
    assert(!held(PACING_STREAM_VIDEO, P + 60 * SECOND));
    assert(held(PACING_STREAM_VIDEO, P + 63 * SECOND) == 2000000);
}

/* After a seek the next chunk sets a new anchor. */
static void reset_by_a_seek(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    OutputPacingReset();
    assert(!held(PACING_STREAM_VIDEO, P + 3 * SECOND));
    assert(held(PACING_STREAM_VIDEO, P + 6 * SECOND) == 2000000);
}

/* A stream that jumps back is anchored again, due one lead later. */
static void jump_back(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    assert(!held(PACING_STREAM_VIDEO, P + SECOND));
    assert(!held(PACING_STREAM_VIDEO, P - 3 * SECOND));
    assert(held(PACING_STREAM_VIDEO, P - 3 * SECOND + 2 * SECOND) == 2000000);
}

/* Far beyond the lead (the other stream moved the anchor): anchored there. */
static void far_ahead(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    assert(!held(PACING_STREAM_AUDIO, P + 5 * SECOND));
    assert(held(PACING_STREAM_AUDIO, P + 8 * SECOND) == 1500000);
}

static void no_timestamp(void)
{
    lead("1000");
    assert(!held(PACING_STREAM_VIDEO, P));
    assert(!held(PACING_STREAM_VIDEO, (int64_t)INVALID_PTS_VALUE));
    assert(!held(PACING_STREAM_VIDEO, -1));
    assert(held(PACING_STREAM_VIDEO, P + 3 * SECOND) == 2000000);
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
    fprintf(stderr, "pacing: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_OUTPUT_PACING_MS");
    failed += run("off without EP3_OUTPUT_PACING_MS", off);
    failed += run("off with a negative value", off_negative);
    failed += run("off with text", off_text);
    failed += run("video ahead of the lead", video_ahead);
    failed += run("audio has 500 ms more", audio_has_more_lead);
    failed += run("within the lead", within_the_lead);
    failed += run("the clock stands during a pause", pause_stops_the_clock);
    failed += run("a seek ends the wait", seek_ends_the_wait);
    failed += run("a stop ends the wait", stop_ends_the_wait);
    failed += run("trick play", trick_play);
    failed += run("anchored again after a seek", reset_by_a_seek);
    failed += run("a stream jumps back", jump_back);
    failed += run("far beyond the lead", far_ahead);
    failed += run("no timestamp", no_timestamp);
    return failed ? 1 : 0;
}
