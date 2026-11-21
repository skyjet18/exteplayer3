/*
 * Optional software playback clock, included by container_ffmpeg.c.
 *
 * The player pushes data as fast as the device takes it and relies on the
 * write blocking (or reporting "not writable") while the decoder is full.
 * The devices of some HiSilicon receivers take everything; picture and
 * sound then run too fast. With EP3_OUTPUT_PACING_MS=<ms> in the environment
 * a chunk is held back until it is at most <ms> ahead of real time (audio
 * 500 ms more, so that its buffer does not run dry before the picture).
 * Unset, empty, 0 or negative: off, nothing here runs.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "misc.h"

#define PACING_STREAM_VIDEO 0
#define PACING_STREAM_AUDIO 1
#define PACING_STREAM_MAX   2

static pthread_mutex_t g_pacing_mtx = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_pacing_lead_ms = -1;   /* -1: environment not read yet */
static int64_t g_pacing_wall_us = -1;   /* anchor: monotonic time ...   */
static int64_t g_pacing_pts = -1;       /* ... at which this PTS is due */
static int64_t g_pacing_last_pts[PACING_STREAM_MAX] = { -1, -1 };

static int64_t PacingNowUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

static int32_t OutputPacingLeadMs(void)
{
    if (g_pacing_lead_ms < 0)
    {
        const char *env = getenv("EP3_OUTPUT_PACING_MS");
        int32_t lead = env ? atoi(env) : 0;
        g_pacing_lead_ms = lead > 0 ? lead : 0;
    }
    return g_pacing_lead_ms;
}

/* forget the anchor, the next chunk sets a new one (seek, trick play) */
static void OutputPacingReset(void)
{
    int32_t i;
    pthread_mutex_lock(&g_pacing_mtx);
    g_pacing_wall_us = -1;
    g_pacing_pts = -1;
    for (i = 0; i < PACING_STREAM_MAX; ++i)
    {
        g_pacing_last_pts[i] = -1;
    }
    pthread_mutex_unlock(&g_pacing_mtx);
}

/* pts in 90 kHz units; returns when the chunk may be written */
static void OutputPacingWait(Context_t *context, int32_t stream, int64_t pts)
{
    int32_t leadMs = OutputPacingLeadMs();
    int64_t now, last, target, ahead, maxAhead;

    if (leadMs <= 0 || pts < 0 || pts == (int64_t)INVALID_PTS_VALUE || NULL == context ||
        stream < 0 || stream >= PACING_STREAM_MAX)
    {
        return;
    }

    /* trick play steps through key frames, real time means nothing there */
    if (context->playback->isForwarding || context->playback->BackWard || context->playback->SlowMotion)
    {
        OutputPacingReset();
        return;
    }

    /* the clock stands during a pause, or the delay bursts out after it */
    if (context->playback->isPaused)
    {
        int64_t pauseStart = PacingNowUs();
        /* taken before the state is looked at: no continue is missed */
        uint32_t serial = PlaybackChangeSerial();
        while (context->playback->isPaused && context->playback->isPlaying &&
               !context->playback->isSeeking && 0 == PlaybackDieNow(0))
        {
            /* continue, stop and seek wake it; the timeout is a safety net
             * for a change of the state that does not signal */
            PlaybackWaitChange(serial, 250);
            serial = PlaybackChangeSerial();
        }
        pthread_mutex_lock(&g_pacing_mtx);
        if (g_pacing_wall_us >= 0)
        {
            g_pacing_wall_us += PacingNowUs() - pauseStart;
        }
        pthread_mutex_unlock(&g_pacing_mtx);
    }

    now = PacingNowUs();
    maxAhead = (int64_t)leadMs * 1000LL;
    if (stream == PACING_STREAM_AUDIO)
    {
        maxAhead += 500000LL;
    }

    pthread_mutex_lock(&g_pacing_mtx);
    if (g_pacing_pts < 0)
    {
        g_pacing_pts = pts;
        g_pacing_wall_us = now;
    }
    last = g_pacing_last_pts[stream];
    g_pacing_last_pts[stream] = pts;

    if (last >= 0 && (pts - last < -90000LL * 2LL || pts - last > 90000LL * 10LL))
    {
        /* this stream jumped: anchor again. What was written before the
         * jump is still being played, so the new position is due a lead later */
        g_pacing_pts = pts;
        g_pacing_wall_us = now + (int64_t)leadMs * 1000LL;
        pthread_mutex_unlock(&g_pacing_mtx);
        return;
    }

    target = g_pacing_wall_us + ((pts - g_pacing_pts) * 100LL) / 9LL;
    ahead = target - now;
    if (ahead > maxAhead + 2000000LL || ahead < -10000000LL)
    {
        /* far off: the other stream jumped and moved the anchor, or the
         * source stalled for long. Anchor here instead of sleeping it out */
        g_pacing_pts = pts;
        g_pacing_wall_us = now;
        pthread_mutex_unlock(&g_pacing_mtx);
        return;
    }
    pthread_mutex_unlock(&g_pacing_mtx);

    while (ahead > maxAhead)
    {
        int64_t sleepUs = ahead - maxAhead;

        if (!context->playback->isPlaying || context->playback->isSeeking || 0 != PlaybackDieNow(0))
        {
            break;
        }
        usleep(sleepUs > 5000LL ? 5000 : (useconds_t)sleepUs);
        ahead = target - PacingNowUs();
    }
}
