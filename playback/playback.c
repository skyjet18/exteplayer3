/*
 * GPL
 * duckbox 2010
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <libavutil/time.h>

#include "playback.h"
#include "debug.h"
#include "common.h"
#include "misc.h"

/* ***************************** */
/* Macros/Constants              */
/* ***************************** */
#define cERR_PLAYBACK_NO_ERROR      0
#define cERR_PLAYBACK_ERROR        -1

#define cMaxSpeed_ff   128 /* fixme: revise */
#define cMaxSpeed_fr   -320 /* fixme: revise */

#define MAX_PLAYBACK_DIE_NOW_CALLBACKS 10

static pthread_mutex_t continue_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t last_continue_us;
static uint32_t playback_stamp;

int64_t PlaybackLastContinue(void)
{
    int64_t value;
    pthread_mutex_lock(&continue_mutex);
    value = last_continue_us;
    pthread_mutex_unlock(&continue_mutex);
    return value;
}

/* A change of the play state (continue, stop, seek, audio switch) for the
 * threads that wait for one in PlaybackWaitChange() instead of polling. */
static pthread_mutex_t change_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t change_cond = PTHREAD_COND_INITIALIZER;
static uint32_t change_serial;

static void PlaybackChanged(void)
{
    pthread_mutex_lock(&change_mutex);
    change_serial++;
    pthread_cond_broadcast(&change_cond);
    pthread_mutex_unlock(&change_mutex);
}

uint32_t PlaybackChangeSerial(void)
{
    uint32_t serial;
    pthread_mutex_lock(&change_mutex);
    serial = change_serial;
    pthread_mutex_unlock(&change_mutex);
    return serial;
}

/* Returns after a change of the play state since PlaybackChangeSerial() gave
 * serial, or after timeoutMs. The caller takes the serial before it looks at
 * the state: a change in between ends the wait at once. */
void PlaybackWaitChange(uint32_t serial, int32_t timeoutMs)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeoutMs / 1000;
    until.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&change_mutex);
    while (change_serial == serial &&
           0 == pthread_cond_timedwait(&change_cond, &change_mutex, &until));
    pthread_mutex_unlock(&change_mutex);
}

/* ***************************** */
/* Variables                     */
/* ***************************** */

/* ***************************** */
/* Prototypes                    */
/* ***************************** */
extern void set_pause_timeout(uint8_t pause);
static int32_t PlaybackTerminate(Context_t  *context);

/* Set once by the first PlaybackDieNow(1), from the stop of main() or the end
 * of the play thread; the callbacks (wake-ups of threads that wait in a poll)
 * are registered by the threads of the output while it may already be set.
 * dieNow is a 32-bit int with __atomic access, the exchange decides which
 * caller runs the callbacks. The slots and dieNow are sequentially consistent:
 * a callback stored before the stop went through the slots is called by it,
 * and a thread that stores its callback later sees dieNow set at its next
 * PlaybackDieNow(0), which every waiting loop asks before it waits. The
 * registration does not call the callback itself: a wake-up may register on
 * its first call (pthread_once in the writer), it would run inside itself. */
static int32_t dieNow = 0;
static PlaybackDieNowCallback playbackDieNowCallbacks[MAX_PLAYBACK_DIE_NOW_CALLBACKS] = {NULL};

/* ***************************** */
/* MISC Functions                */
/* ***************************** */
int8_t PlaybackDieNow(int8_t val)
{
    if(val && 0 == __atomic_exchange_n(&dieNow, 1, __ATOMIC_SEQ_CST))
    {
        uint32_t i = 0;
        while (i < MAX_PLAYBACK_DIE_NOW_CALLBACKS)
        {
            PlaybackDieNowCallback callback = __atomic_load_n(&playbackDieNowCallbacks[i], __ATOMIC_SEQ_CST);
            if (callback == NULL)
            {
                break;
            }
            callback();
            i += 1;
        }
        PlaybackChanged();
    }
    return (int8_t)__atomic_load_n(&dieNow, __ATOMIC_SEQ_CST);
}

bool PlaybackDieNowRegisterCallback(PlaybackDieNowCallback callback)
{
    bool ret = false;
    if (callback)
    {
        uint32_t i = 0;
        while (i < MAX_PLAYBACK_DIE_NOW_CALLBACKS)
        {
            PlaybackDieNowCallback slot = __atomic_load_n(&playbackDieNowCallbacks[i], __ATOMIC_SEQ_CST);
            if (slot == callback)
            {
                ret = true;
                break;
            }

            if (slot == NULL)
            {
                /* another thread may take the slot at the same time: then
                 * the same slot is looked at again */
                if (__atomic_compare_exchange_n(&playbackDieNowCallbacks[i], &slot, callback, false,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                {
                    ret = true;
                    break;
                }
                continue;
            }
            i += 1;
        }
    }
    return ret;
}

/* ***************************** */
/* Functions                     */
/* ***************************** */

static int PlaybackStop(Context_t  *context);

static int PlaybackOpen(Context_t  *context, PlayFiles_t *pFiles)
{
    if (context->playback->isPlaying)
    {
        PlaybackStop(context);
    }
    
    char *uri = pFiles->szFirstFile;
    
    playback_printf(10, "URI=%s\n", uri);

    if (context->playback->isPlaying)
    { // shouldn't happen
        playback_err("playback already running\n");
        return cERR_PLAYBACK_ERROR;
    }

    char * extension = NULL;

    context->playback->uri = strdup(uri);
    if (!context->playback->uri)
    {
        playback_err("cannot allocate the uri\n");
        return cERR_PLAYBACK_ERROR;
    }

    context->playback->isFile = 0;
    context->playback->isHttp = 0;

    if (!strncmp("file://", uri, 7) || !strncmp("myts://", uri, 7)) 
    {
        context->playback->isFile = 1;
        if (!strncmp("myts://", uri, 7)) 
        {
            memcpy(context->playback->uri, "file", 4);
            context->playback->noprobe = 1;
        } 
        else
        {
            context->playback->noprobe = 0;
        }

        extension = getExtension(context->playback->uri+7);
        if(!extension)
        {
            playback_err("Wrong extension (%s)\n", context->playback->uri+7);
            return cERR_PLAYBACK_ERROR;
        }
    } 
    else if (strstr(uri, "://")) 
    {
        context->playback->isHttp = 1;
        extension = "mp3";
        if (!strncmp("mms://", uri, 6)) 
        {
            // mms is in reality called rtsp, and ffmpeg expects this
            char * tUri = (char*)malloc(strlen(uri) + 2);
            if (!tUri)
            {
                playback_err("cannot allocate the uri\n");
                return cERR_PLAYBACK_ERROR;
            }
            strncpy(tUri+1, uri, strlen(uri)+1);
            strncpy(tUri, "rtsp", 4);
            free(context->playback->uri);
            context->playback->uri = tUri;
        }
    }
    else
    {
        playback_err("Unknown stream (%s)\n", uri);
        return cERR_PLAYBACK_ERROR;
    }

    pFiles->szFirstFile = context->playback->uri;
    if ((context->container->Command(context, CONTAINER_ADD, extension) < 0)
        ||  (!context->container->selectedContainer)
        ||  (context->container->selectedContainer->Command(context, CONTAINER_INIT, pFiles) < 0))
    {
        playback_err("CONTAINER_ADD failed\n");
        return cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value 0\n");

    return cERR_PLAYBACK_NO_ERROR;
}

static int PlaybackClose(Context_t  *context) 
{
    int ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");

    if (context->container->Command(context, CONTAINER_DEL, NULL) < 0)
    {
        playback_err("container delete failed\n");
    }

    context->manager->audio->Command(context, MANAGER_DEL, NULL);
    context->manager->video->Command(context, MANAGER_DEL, NULL);

    context->playback->isPaused     = 0;
    context->playback->isPlaying    = 0;
    context->playback->isForwarding = 0;
    context->playback->BackWard     = 0;
    context->playback->SlowMotion   = 0;
    context->playback->Speed        = 0;
    if(context->playback->uri) 
    {
        free(context->playback->uri);
        context->playback->uri = NULL;
    }

    playback_printf(10, "exiting with value %d\n", ret);

    return ret;
}

static int PlaybackPlay(Context_t  *context) 
{
    int ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");

    if (!context->playback->isPlaying) 
    {
        context->playback->AVSync = 1;
        ret = context->output->Command(context, OUTPUT_AVSYNC, NULL);
        if (ret != 0 && context->playback->isDash)
        {
            playback_err("DASH hardware A/V sync setup failed\n");
            return ret;
        }

        context->playback->isCreationPhase = 1;	// allows the created thread to go into wait mode
        ret = context->output->Command(context, OUTPUT_PLAY, NULL);

        if (ret != 0) 
        {
            playback_err("OUTPUT_PLAY failed!\n");
            playback_err("clearing isCreationPhase!\n");
            context->playback->isCreationPhase = 0;	// allow thread to go into next state
            context->playback->isPlaying       = 0;
            context->playback->isPaused        = 0;
            context->playback->isForwarding    = 0;
            context->playback->BackWard        = 0;
            context->playback->SlowMotion      = 0;
            context->playback->Speed           = 0;
            context->container->selectedContainer->Command(context, CONTAINER_STOP, NULL);
        }
        else 
        {
            context->playback->isPlaying    = 1;
            context->playback->isPaused     = 0;
            context->playback->isForwarding = 0;
            context->playback->BackWard     = 0;
            context->playback->SlowMotion   = 0;
            context->playback->Speed        = 1;

            playback_printf(10, "clearing isCreationPhase!\n");

            context->playback->isCreationPhase = 0;	// allow thread to go into next state

            ret = context->container->selectedContainer->Command(context, CONTAINER_PLAY, NULL);
            if (ret != 0) {
                playback_err("CONTAINER_PLAY failed!\n");
            }

        }
    }
    else
    {
        playback_err("playback already running\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);

    return ret;
}

#ifdef HAVE_DREAMNEXTGEN
static int32_t PlaybackContinue(Context_t  *context);
static int32_t PlaybackSeek(Context_t  *context, int64_t *pos, uint8_t absolute);

/* the stream PTS to play on from after the trick play */
static int64_t trickResumePts = INVALID_PTS_VALUE;

static void PlaybackTrickEnd(Context_t *context)
{
    int64_t pts = 0;

    if (context->container->selectedContainer->Command(context, CONTAINER_TRICK, &pts) == 0 && pts != INVALID_PTS_VALUE)
    {
        trickResumePts = pts;
    }
    context->playback->isForwarding = 0;
    context->playback->BackWard     = 0;
    context->playback->Speed        = 1;
}

/* Wind forwards (speed > 1) or backwards (speed < 0) on keyframes. */
static int32_t PlaybackTrick(Context_t *context, int32_t speed)
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;
    int64_t arg = speed;

    playback_printf(10, "speed %d\n", speed);

    if (speed == 0 || speed == 1)
    {
        return PlaybackContinue(context);
    }
    if (!context->playback->isPlaying || context->playback->isSeeking || context->playback->SlowMotion)
    {
        playback_err("trick play not possible\n");
        return cERR_PLAYBACK_ERROR;
    }
    if (context->container->selectedContainer->Command(context, CONTAINER_TRICK, &arg) != 0)
    {
        return cERR_PLAYBACK_ERROR;
    }
    if (context->playback->isPaused)
    {
        set_pause_timeout(0);
        context->output->Command(context, OUTPUT_CONTINUE, NULL);
        context->playback->isPaused = 0;
    }
    trickResumePts = INVALID_PTS_VALUE;
    context->playback->isForwarding = speed > 0;
    context->playback->BackWard     = speed < 0;
    context->playback->Speed        = speed;
    PlaybackChanged();

    playback_printf(10, "exiting with value %d\n", ret);
    return ret;
}
#endif

static int PlaybackPause(Context_t  *context) 
{
    int ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");

    if (context->playback->isPlaying && !context->playback->isPaused) 
    {
#ifdef HAVE_DREAMNEXTGEN
        /* the picture of the trick play stays, continue goes on from it */
        if (context->playback->isForwarding || context->playback->BackWard)
        {
            PlaybackTrickEnd(context);
        }
#endif
        set_pause_timeout(1);
        
        context->playback->isPaused     = 1;
        
        context->output->Command(context, OUTPUT_PAUSE, NULL);

        //context->playback->isPlaying  = 1;
        context->playback->isForwarding = 0;
        context->playback->BackWard     = 0;
        context->playback->SlowMotion   = 0;
        context->playback->Speed        = 1;
    }
    else
    {
        playback_err("playback not playing or already in pause mode\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackContinue(Context_t  *context) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");

    if (context->playback->isPlaying &&
       (context->playback->isPaused || context->playback->isForwarding || 
        context->playback->BackWard || context->playback->SlowMotion)) 
    {

        set_pause_timeout(0);

#ifdef HAVE_DREAMNEXTGEN
        if (context->playback->isForwarding || context->playback->BackWard)
        {
            PlaybackTrickEnd(context);
        }
#endif

        context->output->Command(context, OUTPUT_CONTINUE, NULL);

        context->playback->isPaused     = 0;
        pthread_mutex_lock(&continue_mutex);
        last_continue_us = av_gettime_relative();
        pthread_mutex_unlock(&continue_mutex);
        //context->playback->isPlaying  = 1;
        context->playback->isForwarding = 0;
        context->playback->BackWard     = 0;
        context->playback->SlowMotion   = 0;
        context->playback->Speed        = 1;
        PlaybackChanged();

#ifdef HAVE_DREAMNEXTGEN
        /* play on from the last picture of the trick play */
        if (trickResumePts != INVALID_PTS_VALUE)
        {
            int64_t sec = trickResumePts / 90000;
            trickResumePts = INVALID_PTS_VALUE;
            PlaybackSeek(context, &sec, 1);
        }
#endif
    }
    else
    {
        playback_err("continue not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackStop(Context_t  *context) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");
    
    PlaybackDieNow(1);
    if (context && context->playback)
        context->playback->stamp = (void *)-1;

    if (context && context->playback && context->playback->isPlaying) 
    {

        context->playback->isPaused     = 0;
        context->playback->isPlaying    = 0;
        context->playback->isForwarding = 0;
        context->playback->BackWard     = 0;
        context->playback->SlowMotion   = 0;
        context->playback->Speed        = 0;

        context->output->Command(context, OUTPUT_STOP, NULL);
        context->container->selectedContainer->Command(context, CONTAINER_STOP, NULL);

    }
    else
    {
        playback_err("stop not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackTerminate(Context_t  *context) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(20, "\n");
    
    PlaybackDieNow(1);

    if ( context && context->playback && context->playback->isPlaying ) 
    {
        //First Flush and than delete container, else e2 can't read length of file anymore

        if (context->output->Command(context, OUTPUT_FLUSH, NULL) < 0)
        {
            playback_err("failed to flush output.\n");
        }

        ret = context->container->selectedContainer->Command(context, CONTAINER_STOP, NULL);

        context->playback->isPaused     = 0;
        context->playback->isPlaying    = 0;
        context->playback->isForwarding = 0;
        context->playback->BackWard     = 0;
        context->playback->SlowMotion   = 0;
        context->playback->Speed        = 0;

    } 
    else
    {
        playback_err("%p %p %d\n", context, context ? context->playback : NULL,
                     context && context->playback ? context->playback->isPlaying : -1);

        /* fixme: konfetti: we should return an error here but this seems to be a condition which
         * can happen and is not a real error, which leads to a dead neutrino. should investigate
         * here later.
         */
    }

    playback_printf(20, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackSeekCmd(Context_t  *context, int64_t *pos, ContainerCmd_t containerCmd);

static int32_t PlaybackSeek(Context_t  *context, int64_t *pos, uint8_t absolute)
{
    return PlaybackSeekCmd(context, pos, absolute ? CONTAINER_SEEK_ABS : CONTAINER_SEEK);
}

/* containerCmd: CONTAINER_SEEK (pos in s from the position), CONTAINER_SEEK_ABS
 * (pos in s), CONTAINER_SEEK_ABS_MS or CONTAINER_SEEK_ABS_MS_AFTER (pos in ms) */
static int32_t PlaybackSeekCmd(Context_t  *context, int64_t *pos, ContainerCmd_t containerCmd)
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "pos: %"PRIu64"\n", *pos);

    if (context->playback->isPlaying && !context->playback->isForwarding && !context->playback->BackWard && !context->playback->SlowMotion && !context->playback->isPaused) 
    {
        uint8_t wasSeeking = context->playback->isSeeking;
        void *old_stamp = context->playback->stamp;
        context->playback->isSeeking = 1;
        PlaybackChanged();
        /* We changing current stamp, so all frames with old stamps will be skipped
         * Without this there could be situation when the ffmpeg thread is 
         * in the av_read_frame(), so, even if we flushed data,  still 
         * data from the old position were written
         */
        playback_stamp += 1;
        context->playback->stamp = (void *)(uintptr_t)playback_stamp;
        ret = context->container->selectedContainer->Command(context, containerCmd, pos);
        if (ret == 0)
        {
            ret = context->output->Command(context, OUTPUT_CLEAR, NULL);
            if (ret < 0)
                playback_err("failed to clear output for seek\n");
        }
        else
        {
            context->playback->stamp = old_stamp;
            playback_err("failed to queue seek\n");
        }
        context->playback->isSeeking = wasSeeking;
    } 
    else
    {
        playback_err("not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);

    return ret;
}

static int32_t PlaybackPts(Context_t  *context, int64_t *pts)
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(20, "\n");

    *pts = 0;

    if (context->playback->isPlaying)
    {
        ret = context->output->Command(context, OUTPUT_PTS, pts);
#ifdef HAVE_DREAMNEXTGEN
        /* the position in the stream, not the scaled PTS of the trick play */
        if (ret == 0 && (context->playback->isForwarding || context->playback->BackWard))
        {
            context->container->selectedContainer->Command(context, CONTAINER_TRICK_PTS, pts);
        }
        else if (ret == 0 && trickResumePts != INVALID_PTS_VALUE)
        {
            *pts = trickResumePts;
        }
#endif
    } 
    else
    {
        playback_err("not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(20, "exiting with value %d\n", ret);

    return ret;
}

static int32_t PlaybackGetFrameCount(Context_t  *context, int64_t *frameCount) 
{
    int ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(20, "\n");

    *frameCount = 0;

    if (context->playback->isPlaying) 
    {
        ret = context->output->Command(context, OUTPUT_GET_FRAME_COUNT, frameCount);
    } else
    {
        playback_err("not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(20, "exiting with value %d\n", ret);

    return ret;
}

static int32_t PlaybackLength(Context_t  *context, int64_t *length) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(20, "\n");

    *length = -1;

    if (context->playback->isPlaying) 
    {
        if (context->container && context->container->selectedContainer)
        {
            context->container->selectedContainer->Command(context, CONTAINER_LENGTH, length);
        }
    } 
    else
    {
        playback_err("not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(20, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackSwitchAudio(Context_t  *context, int32_t *track)
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;
    int32_t curtrackid = 0;
    int32_t nextrackid = 0;

    playback_printf(10, "\n");

    if (context && context->playback && context->playback->isPlaying)
    {
        uint8_t wasSeeking = context->playback->isSeeking;
        uint8_t wasSwitching = context->playback->isAudioSwitching;
        context->playback->isSeeking = 1;
        PlaybackChanged();
        if (context->manager && context->manager->audio) 
        {
            ret = context->manager->audio->Command(context, MANAGER_GET, &curtrackid);
            if (ret == 0)
                ret = context->manager->audio->Command(context, MANAGER_SET, track);
            if (ret == 0)
                ret = context->manager->audio->Command(context, MANAGER_GET, &nextrackid);
        }
        else
        {
            playback_err("switch audio not possible\n");
            ret = cERR_PLAYBACK_ERROR;
        }

        if(ret == 0 && nextrackid != curtrackid)
        {
            /* a seek of the container for the switch changes it */
            void *stamp = context->playback->stamp;
            if (context->container && context->container->selectedContainer)
            {
                ret = context->container->selectedContainer->Command(context, CONTAINER_SWITCH_AUDIO, &nextrackid);
            }
            else
                ret = cERR_PLAYBACK_ERROR;
            if (ret != 0)
            {
                if (context->manager->audio->Command(context, MANAGER_SET, &curtrackid) < 0)
                    playback_err("failed to restore audio selection\n");
                playback_err("failed to prepare audio switch\n");
            }
            else if (context->output && context->output->audio)
            {
                /* DASH without that seek (live): the new representation starts
                 * at the read position, nothing read before it may play on */
                if (context->playback->isDash && context->playback->stamp == stamp)
                {
                    playback_stamp += 1;
                    context->playback->stamp = (void *)(uintptr_t)playback_stamp;
                    ret = context->output->Command(context, OUTPUT_CLEAR, NULL);
                    if (ret < 0)
                        playback_err("failed to reset DASH A/V buffers\n");
                }
                if (ret == 0)
                    ret = context->output->audio->Command(context, OUTPUT_SWITCH, (void*)"audio");
                if (ret < 0)
                    playback_err("failed to switch audio output\n");
            }
        }
        context->playback->isSeeking = wasSeeking;
        context->playback->isAudioSwitching = wasSwitching;
    } 
    else
    {
        playback_err("switch audio not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t PlaybackSwitchSubtitle(Context_t  *context, int32_t *track) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;
    int32_t curtrackid = -1;
    int32_t nextrackid = -1;

    playback_printf(10, "Track: %d\n", *track);

    if (context && context->playback && context->playback->isPlaying )
    {
        if (context->manager && context->manager->subtitle) 
        {
            context->manager->subtitle->Command(context, MANAGER_GET, &curtrackid);
            context->manager->subtitle->Command(context, MANAGER_SET, track);
            context->manager->subtitle->Command(context, MANAGER_GET, &nextrackid);
          
            if (curtrackid != nextrackid)
            {
                if (context->output && context->output->subtitle)
                {
                    context->output->subtitle->Command(context, OUTPUT_SWITCH, (void*)"subtitle");
                }

                if (context->container && context->container->selectedContainer)
                {
                    context->container->selectedContainer->Command(context, CONTAINER_SWITCH_SUBTITLE, &nextrackid);
                }
            }
        } 
        else
        {
            ret = cERR_PLAYBACK_ERROR;
            playback_err("no subtitle\n");
        }
    } 
    else
    {
        playback_err("not possible\n");
        ret = cERR_PLAYBACK_ERROR;
    }

    playback_printf(10, "exiting with value %d\n", ret);

    return ret;
}

static int32_t PlaybackInfo(Context_t  *context, char **infoString) 
{
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(10, "\n");

    /* konfetti comment: 
     * removed if clause here (playback running) because its 
     * not necessary for all container. e.g. in case of ffmpeg 
     * container playback must not play to get the info.
     */
    if (context->container && context->container->selectedContainer)
    {
        context->container->selectedContainer->Command(context, CONTAINER_INFO, infoString);
    }

    playback_printf(10, "exiting with value %d\n", ret);

    return ret;
}

static int32_t Command(void* _context, PlaybackCmd_t command, void *argument) 
{
    Context_t* context = (Context_t*) _context; /* to satisfy compiler */
    int32_t ret = cERR_PLAYBACK_NO_ERROR;

    playback_printf(20, "Command %d\n", command);


    switch(command) 
    {
        case PLAYBACK_OPEN: 
        {
            ret = PlaybackOpen(context, (PlayFiles_t*)argument);
            break;
        }
        case PLAYBACK_CLOSE: 
        {
            ret = PlaybackClose(context);
            break;
        }
        case PLAYBACK_PLAY: 
        {
            ret = PlaybackPlay(context);
            break;
        }
        case PLAYBACK_STOP: 
        {
            ret = PlaybackStop(context);
            break;
        }
        case PLAYBACK_PAUSE: 
        {
            ret = PlaybackPause(context);
            break;
        }
        case PLAYBACK_CONTINUE: 
        {
            ret = PlaybackContinue(context);
            break;
        }
        case PLAYBACK_TERM: 
        {
            ret = PlaybackTerminate(context);
            break;
        }
        case PLAYBACK_SEEK: 
        {
            ret = PlaybackSeek(context, (int64_t*)argument, 0);
            break;
        }
        case PLAYBACK_SEEK_ABS: 
        {
            ret = PlaybackSeek(context, (int64_t*)argument, -1);
            break;
        }
        case PLAYBACK_SEEK_ABS_MS:
        {
            ret = PlaybackSeekCmd(context, (int64_t*)argument, CONTAINER_SEEK_ABS_MS);
            break;
        }
        case PLAYBACK_SEEK_ABS_MS_AFTER:
        {
            ret = PlaybackSeekCmd(context, (int64_t*)argument, CONTAINER_SEEK_ABS_MS_AFTER);
            break;
        }
        case PLAYBACK_PTS: 
        {
            ret = PlaybackPts(context, (int64_t*)argument);
            break;
        }
        case PLAYBACK_LENGTH: 
        {
            ret = PlaybackLength(context, (int64_t*)argument);
            break;
        }
        case PLAYBACK_SWITCH_AUDIO: 
        {
            ret = PlaybackSwitchAudio(context, (int*)argument);
            break;
        }
        case PLAYBACK_SWITCH_SUBTITLE: 
        {
            ret = PlaybackSwitchSubtitle(context, (int*)argument);
            break;
        }
        case PLAYBACK_INFO: 
        {
            ret = PlaybackInfo(context, (char**)argument);
            break;
        }
#ifdef HAVE_DREAMNEXTGEN
        case PLAYBACK_FASTFORWARD:
        {
            ret = PlaybackTrick(context, *(int32_t *)argument);
            break;
        }
        case PLAYBACK_FASTBACKWARD:
        {
            ret = PlaybackTrick(context, -*(int32_t *)argument);
            break;
        }
#endif
        case PLAYBACK_GET_FRAME_COUNT: 
        { 
            ret = PlaybackGetFrameCount(context, (uint64_t*)argument);
            break;
        }
        default:
            playback_err("PlaybackCmd %d not supported!\n", command);
            ret = cERR_PLAYBACK_ERROR;
            break;
    }

    playback_printf(20, "exiting with value %d\n", ret);

    return ret;
}

/*
 * This is very unreadable and must be changed
 */
PlaybackHandler_t PlaybackHandler = {
    "Playback", //name
    -1,         //fd
    0,          //isFile
    0,          //isHttp
    0,          //isPlaying
    0,          //isPaused
    0,          //isForwarding
    0,          //isSeeking
    0,          //isAudioSwitching
    0,          //isDash
    0,          //isCreationPhase
    0,          //BackWard
    0,          //SlowMotion
    0,          //Speed
    0,          //AVSync
    0,          //isVideo
    0,          //isAudio
    0,          //isSubtitle
    0,          //abortRequested
    &Command,   //Command
    "",         //uri
    0,          //size
    0,          //noprobe
    0,          //isLoopMode
    0,          //isTSLiveMode
	10000,      //httpTimeout
    NULL,       //stamp
};
