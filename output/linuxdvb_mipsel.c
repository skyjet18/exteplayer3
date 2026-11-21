/*
 * LinuxDVB Output handling.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <time.h>

#include "bcm_ioctls.h"

#include "common.h"
#include "debug.h"
#include "output.h"
#include "writer.h"
#include "misc.h"
#include "pes.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */

#define cERR_LINUXDVB_NO_ERROR      0
#define cERR_LINUXDVB_ERROR        -1

static const char VIDEODEV[] = "/dev/dvb/adapter0/video0";
#ifdef HAVE_DREAMNEXTGEN
/* the audio decoder of the dreamaudiod service */
static const char AUDIODEV[] = "/dev/dvb/adapter0/dreamaudio0";
#else
static const char AUDIODEV[] = "/dev/dvb/adapter0/audio0";
#endif

static int videofd 	= -1;
static int audiofd 	= -1;

/*
 * What LinuxDvbStop() still has to do for a device: stop the decoder, which
 * runs (or is held) since the open or its last PLAY, and give the source back
 * to the demux, which the open set to memory. A playback stopped with "q" is
 * stopped by OUTPUT_STOP and then closed by OUTPUT_CLOSE, and the close called
 * LinuxDvbStop() again on the stopped decoders. Some Vu+ drivers are reported
 * to crash on that second stop; now every step happens once.
 * Every PLAY sets the running flag again, every STOP clears it.
 */
static bool videoRunning, audioRunning;
static bool videoOnMemory, audioOnMemory;

struct DVBApiVideoInfo_s
{
    int aspect_ratio;
    int progressive;
    int frame_rate;
    int width, height;
};
static struct DVBApiVideoInfo_s videoInfo = {-1,-1,-1,-1,-1};

#ifndef VIDEO_GET_FRAME_RATE
#define VIDEO_GET_FRAME_RATE _IOR('o', 56, unsigned int)
#endif

static const char VIDEO_PROGRESSIVE_FILE[] = "/proc/stb/vmpeg/0/progressive";

/*
 * Completing a video track FFmpeg has no picture size of, and the
 * progressive flag of every video track. videoInfo holds
 * what the video events delivered, videoInfoAsked what the driver answered
 * for the values no event came for.
 */
static struct DVBApiVideoInfo_s videoInfoAsked = {-1,-1,-1,-1,-1};
enum
{
    VIDEO_INFO_START,   /* no video data written yet */
    VIDEO_INFO_QUIET,   /* no event so far, the driver has not been asked */
    VIDEO_INFO_HOLD,    /* asked; the answer is kept back until no event follows */
    VIDEO_INFO_EVENTS,  /* events arrive; waiting for the last one */
    VIDEO_INFO_DONE     /* reported */
};
static int videoInfoPhase = VIDEO_INFO_START;
static bool videoInfoReported;
/* the current video track has no picture size, or no progressive flag yet */
static bool videoInfoWanted;
static bool videoInfoNoSize;
static int videoInfoRetries;
#define VIDEO_INFO_MAX_RETRIES 10
/* video data and the position requests both read the video events */
static pthread_mutex_t videoInfoMutex = PTHREAD_MUTEX_INITIALIZER;
static void LinuxDvbVideoInfoPoll(Context_t *context, unsigned long long pts);
/* the periods are measured with the decoder PTS (90 kHz) */
#define VIDEO_INFO_QUIET_PTS   9000
#define VIDEO_INFO_SETTLE_PTS 45000
#define VIDEO_INFO_MAX_STEP  900000
static unsigned long long videoInfoPts;
static unsigned long long videoInfoProgress;
static int videoInfoPtsSamples;

unsigned long long int sCURRENT_PTS = 0;
bool isBufferedOutput = false;
/* Set once video with a PTS other than 0 went to the decoder since the last
 * play, stop, flush or clear: from then on a VIDEO_GET_PTS of 0 is no position. */
static volatile int videoPtsWritten;

/* The position of a decoder that has run out of data. Vu+ decoders keep
 * counting VIDEO_GET_PTS / AUDIO_GET_PTS with their clock after the last
 * frame: a live stream whose playlist does not grow for 18 s reports a
 * position 12 s after the last frame, and when data comes again the decoder
 * goes back to the PTS of the frames it shows. So the position is kept
 * within the data written to the decoder: once the decoder reports more than
 * POS_BOUND_TOLERANCE after the highest PTS written, that PTS plus the
 * tolerance is reported, until the decoder shows data again and has come up
 * to that value. Only after the decoder was seen within the data: right
 * after a clear it can still report the PTS of an earlier playback, which
 * is left as it is. After a start (OUTPUT_PLAY) a decoder that has not yet
 * shown any of the data may still count on with the clock of the earlier
 * playback: an audio-only DTS file on VTi (Uno 4K SE, AV receiver) reported
 * 9.5 s two seconds after the start with 3.9 s of data written, and its
 * clock ran on for good. A value more than POS_BOUND_TOLERANCE after all
 * data written since the start is no position of it, so until the decoder
 * is seen within the data the lowest PTS written since the start is
 * reported instead. PTS are compared as 33 bit values.
 * A decoder restarted by OUTPUT_SWITCH (the video output after a long gap,
 * an audio track switch) reports stale or unrelated values until it shows
 * what is written after the restart: until then, at most for
 * POS_BOUND_RESTART_MS, the position stays at the last one reported.
 * Neither the standing position nor that wait outlast data that comes
 * again: when the data written has gone POS_BOUND_FLOWING beyond the
 * position reported, the position follows the data (a decoder that runs
 * just after the live edge, a decoder restarted again and again). */
#define POS_BOUND_MASK       0x1FFFFFFFFULL
#define POS_BOUND_TOLERANCE  27000ULL         /* 300 ms */
#define POS_BOUND_CATCH_UP   90000ULL         /* 1 s */
#define POS_BOUND_LIMIT      (600ULL * 90000) /* 10 min: further is no stall */
#define POS_BOUND_RESTART_MS 5000
#define POS_BOUND_FLOWING    360000ULL        /* 4 s */
typedef struct
{
    bool have_max;              /* max holds a PTS written since the last reset */
    bool inside;                /* the decoder was seen within the written data */
    bool latched;               /* latch is reported */
    bool restarting;            /* restarted, not seen within the new data yet */
    bool have_min;              /* min holds a PTS written since the restart */
    bool have_reported;
    bool started;               /* reset by OUTPUT_PLAY, not by a clear */
    bool have_first;
    unsigned long long max;     /* highest PTS written, 33 bit */
    unsigned long long latch;
    unsigned long long min;     /* lowest PTS written since the restart */
    unsigned long long first;   /* lowest PTS written since the reset */
    unsigned long long reported;
    struct timespec restart_time;
} PosBound_t;
static PosBound_t videoPosBound, audioPosBound;
/* written by the play thread, read by the position requests */
static pthread_mutex_t posBoundMutex = PTHREAD_MUTEX_INITIALIZER;

/* how far a is after b, modulo 2^33 */
static unsigned long long PosBoundAhead(unsigned long long a, unsigned long long b)
{
    return (a - b) & POS_BOUND_MASK;
}

/* a is b or after b */
static bool PosBoundNotBefore(unsigned long long a, unsigned long long b)
{
    return PosBoundAhead(a, b) < (1ULL << 32);
}

static void PosBoundWritten(PosBound_t *bound, unsigned long long pts)
{
    if (pts == INVALID_PTS_VALUE)
        return;
    pts &= POS_BOUND_MASK;
    pthread_mutex_lock(&posBoundMutex);
    /* B-frames and backward jumps leave the maximum as it is */
    if (!bound->have_max || (pts != bound->max && PosBoundNotBefore(pts, bound->max)))
    {
        bound->max = pts;
        bound->have_max = true;
    }
    if (!bound->have_first || !PosBoundNotBefore(pts, bound->first))
    {
        bound->first = pts;
        bound->have_first = true;
    }
    if (bound->restarting && (!bound->have_min || !PosBoundNotBefore(pts, bound->min)))
    {
        bound->min = pts;
        bound->have_min = true;
    }
    pthread_mutex_unlock(&posBoundMutex);
}

static PosBound_t *PosBoundOf(const char *type, int i)
{
    if (i == 0 && (!type || !strcmp(type, "video")))
        return &videoPosBound;
    if (i == 1 && (!type || !strcmp(type, "audio")))
        return &audioPosBound;
    return NULL;
}

static void PosBoundReset(const char *type)
{
    int i;

    pthread_mutex_lock(&posBoundMutex);
    for (i = 0; i < 2; i++)
    {
        PosBound_t *bound = PosBoundOf(type, i);
        if (bound)
            memset(bound, 0, sizeof(*bound));
    }
    pthread_mutex_unlock(&posBoundMutex);
}

/* A start: the reset, and a value after all the data written since is no
 * position of this playback until the decoder was seen within the data. */
static void PosBoundStart(const char *type)
{
    int i;

    PosBoundReset(type);
    pthread_mutex_lock(&posBoundMutex);
    for (i = 0; i < 2; i++)
    {
        PosBound_t *bound = PosBoundOf(type, i);
        if (bound)
            bound->started = true;
    }
    pthread_mutex_unlock(&posBoundMutex);
}

/* The decoder starts again with the data written from now on, which goes
 * on from the data before. */
static void PosBoundRestart(const char *type)
{
    int i;

    pthread_mutex_lock(&posBoundMutex);
    for (i = 0; i < 2; i++)
    {
        PosBound_t *bound = PosBoundOf(type, i);
        if (!bound)
            continue;
        if (!bound->have_reported || !bound->inside)
        {
            bool started = bound->started;
            memset(bound, 0, sizeof(*bound));
            bound->started = started;
            continue;
        }
        /* one more restart before the decoder is back does not extend the wait */
        if (!bound->restarting)
            clock_gettime(CLOCK_MONOTONIC, &bound->restart_time);
        bound->restarting = true;
        bound->have_min = false;
    }
    pthread_mutex_unlock(&posBoundMutex);
}

static unsigned long long PosBoundApply(PosBound_t *bound, unsigned long long pts)
{
    unsigned long long ahead, reported = pts;

    pthread_mutex_lock(&posBoundMutex);
    if (!bound->have_max)
        goto out;
    ahead = PosBoundAhead(pts, bound->max);
    if (bound->restarting)
    {
        struct timespec now;
        int64_t ms;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = (int64_t)(now.tv_sec - bound->restart_time.tv_sec) * 1000 +
             (now.tv_nsec - bound->restart_time.tv_nsec) / 1000000;
        if (bound->have_min && PosBoundNotBefore(pts + POS_BOUND_TOLERANCE, bound->min) &&
            (ahead <= POS_BOUND_TOLERANCE || ahead >= (1ULL << 32)))
        {
            /* the decoder shows the data written since the restart */
            bound->restarting = false;
        }
        else if (ms < POS_BOUND_RESTART_MS &&
                 !PosBoundNotBefore(bound->max, bound->reported + POS_BOUND_FLOWING))
        {
            reported = bound->reported;
            goto out;
        }
        else
        {
            bound->restarting = false;
        }
    }
    if (!bound->inside && bound->have_first && pts != bound->first &&
        PosBoundAhead(bound->first, pts) <= POS_BOUND_CATCH_UP)
    {
        /* just before the first PTS written since the reset: the clock of
         * a decoder that shows nothing of the data yet, which goes past 0
         * when the data starts at 0 (2^33 - 600 was reported as 95443711
         * ms). The first PTS until the decoder is within the data. An older
         * clock further before the data is left as it is. */
        reported = bound->first;
        goto out;
    }
    if (ahead > POS_BOUND_TOLERANCE && ahead < POS_BOUND_LIMIT)
    {
        /* past the data */
        if (!bound->latched)
        {
            /* not seen within the data since the start: nothing of it shown */
            if (!bound->inside && bound->started && bound->have_first)
                reported = bound->first;
            if (!bound->inside)
                goto out;
            bound->latched = true;
            bound->latch = pts - ahead + POS_BOUND_TOLERANCE;
            if (pts <= POS_BOUND_MASK)
                bound->latch &= POS_BOUND_MASK;
        }
        else if (PosBoundNotBefore(bound->max + POS_BOUND_TOLERANCE, bound->latch + POS_BOUND_FLOWING))
        {
            /* data comes again, the decoder stays after it: follow the data */
            bound->latch = (bound->max + POS_BOUND_TOLERANCE) & POS_BOUND_MASK;
        }
        reported = bound->latch;
        goto out;
    }
    if (ahead <= POS_BOUND_TOLERANCE || ahead >= (1ULL << 32))
        bound->inside = true;
    if (bound->latched)
    {
        /* data again: no step back below what was reported */
        unsigned long long behind = PosBoundAhead(bound->latch, pts);
        if (behind && behind < POS_BOUND_CATCH_UP)
        {
            reported = bound->latch;
            goto out;
        }
        bound->latched = false;
    }
out:
    bound->reported = reported;
    bound->have_reported = true;
    pthread_mutex_unlock(&posBoundMutex);
    return reported;
}

/* Vu+ Ultimo 4K / Duo 4K SE: the first DD+ passthrough start through the
 * playpump stays silent, while an audio track switch made after it gives
 * sound. So on these two models the start is followed once by the restart
 * a track switch does, see LinuxDvbEac3RestartState().
 * 0 = none (always, on every other box), 1 = waiting for the first DD+
 * packet, 2 = packet written. */
#define EAC3_RESTART_DELAY_MS 100
#define EAC3_RESTART_FAST_POLL_MS 5000
static volatile int eac3Restart = 0;
static int eac3RestartChecked = 0;
static struct timespec eac3PlayTime;   /* set before the state becomes 1 */
static struct timespec eac3PacketTime; /* set before the state becomes 2 */

/* Same two models: setting the DD+ bypass mode again while the decoder runs
 * DD+ passthrough reconfigures the HDMI output and leaves it silent. Set when
 * an audio switch has started the decoder in DD+ bypass mode on a box where
 * LinuxDvbEac3NeedsRestart() holds, cleared by every other start or bypass
 * mode: the next switch to a DD+ track then only flushes the decoder. */
static int eac3BypassKept = 0;

/* Left, stereo or right of dual mono audio (two languages on L and R), an
 * OutputAudioChannel_t: the one asked for, and the one the decoder was last
 * set to. Drivers may reset it when the decoder starts, so a selection other
 * than stereo is set again after every AUDIO_PLAY. */
static int audioChannel = OUTPUT_AUDIO_CHANNEL_STEREO;
static int audioChannelApplied = OUTPUT_AUDIO_CHANNEL_STEREO;

pthread_mutex_t LinuxDVBmutex;

/* ***************************** */
/* Prototypes                    */
/* ***************************** */
int32_t LinuxDvbBuffOpen(Context_t *context, char *type, int outfd, void *mtx);
int32_t LinuxDvbBuffClose(Context_t *context);
int32_t LinuxDvbBuffFlush(Context_t *context);
int32_t LinuxDvbBuffFlushAudio(Context_t *context);
int32_t LinuxDvbBuffResume(Context_t *context);

ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic);
int32_t LinuxDvbBuffSetSize(const uint32_t bufferSize);
uint32_t LinuxDvbBuffGetSize();

int LinuxDvbStop(Context_t  *context, char * type);

static int applied_avsync = -1;
static int timing_after_switch;

static void LinuxDvbTiming(const char *phase)
{
    unsigned long long pts;
    struct timeval now;
    int error;
    if (LINUXDVB_DEBUG_LEVEL < 10)
        return;
    gettimeofday(&now, NULL);
    if (videofd >= 0 && !ioctl(videofd, VIDEO_GET_PTS, &pts))
        linuxdvb_printf(10, "hardware timing: phase=%s video_pts90=%llu wall_us=%"PRId64
                       " applied_avsync=%d\n", phase, pts,
                       (int64_t)now.tv_sec * 1000000 + now.tv_usec, applied_avsync);
    else
    {
        error = videofd >= 0 ? errno : EBADF;
        linuxdvb_printf(10, "hardware timing: phase=%s video_pts90=unavailable errno=%d\n",
                       phase, error);
    }
    gettimeofday(&now, NULL);
    if (audiofd >= 0 && !ioctl(audiofd, AUDIO_GET_PTS, &pts))
        linuxdvb_printf(10, "hardware timing: phase=%s audio_pts90=%llu wall_us=%"PRId64
                       " applied_avsync=%d\n", phase, pts,
                       (int64_t)now.tv_sec * 1000000 + now.tv_usec, applied_avsync);
    else
    {
        error = audiofd >= 0 ? errno : EBADF;
        linuxdvb_printf(10, "hardware timing: phase=%s audio_pts90=unavailable errno=%d\n",
                       phase, error);
    }
}

/* The caller owns LinuxDVBmutex. */
static int LinuxDvbApplyAVSync(Context_t *context)
{
    int requested = context && context->playback ? context->playback->AVSync : 0;
    int dash = context && context->playback && context->playback->isDash;
    int enabled = dash ? requested : 0;
#ifdef HAVE_DREAMNEXTGEN
    /* The Dreambox kernel audio backend also needs sync for non-DASH streams. */
    enabled = 1;
#endif
    if (ioctl(audiofd, AUDIO_SET_AV_SYNC, enabled) == -1)
    {
        applied_avsync = -1;
        linuxdvb_err("AUDIO_SET_AV_SYNC: ERROR %d, %s\n", errno, strerror(errno));
        return cERR_LINUXDVB_ERROR;
    }
    applied_avsync = enabled;
    linuxdvb_printf(10, "hardware sync: dash=%d requested=%d applied=%d\n",
                   dash, requested, enabled);
    return 0;
}

/* The caller owns LinuxDVBmutex. AUDIO_CHANNEL_SELECT acts on decoded audio;
 * a passthrough bitstream (bypass to HDMI/S/PDIF) is not changed by it. */
static int LinuxDvbSetAudioChannel(int channel)
{
    audio_channel_select_t selection = AUDIO_STEREO;
    if (OUTPUT_AUDIO_CHANNEL_LEFT == channel)
        selection = AUDIO_MONO_LEFT;
    else if (OUTPUT_AUDIO_CHANNEL_RIGHT == channel)
        selection = AUDIO_MONO_RIGHT;
    if (audiofd < 0)
        return cERR_LINUXDVB_ERROR;
    if (ioctl(audiofd, AUDIO_CHANNEL_SELECT, selection) < 0)
    {
        linuxdvb_err("AUDIO_CHANNEL_SELECT %d: ERROR %d, %s\n", (int)selection, errno, strerror(errno));
        return cERR_LINUXDVB_ERROR;
    }
    audioChannelApplied = channel;
    linuxdvb_printf(10, "audio channel %d\n", channel);
    return cERR_LINUXDVB_NO_ERROR;
}

/* After AUDIO_PLAY: the selection again, as the decoder start may reset it. */
static void LinuxDvbRestoreAudioChannel(void)
{
    if (OUTPUT_AUDIO_CHANNEL_STEREO != audioChannel)
        LinuxDvbSetAudioChannel(audioChannel);
}

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

#define getLinuxDVBMutex() pthread_mutex_lock(&LinuxDVBmutex)
#define releaseLinuxDVBMutex() pthread_mutex_unlock(&LinuxDVBmutex)

static bool LinuxDvbReadProcLine(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    bool ok = false;

    if (f)
    {
        ok = (NULL != fgets(buf, size, f));
        fclose(f);
        if (ok)
            buf[strcspn(buf, "\r\n")] = '\0';
    }
    return ok;
}

/* Read once, at the first audio start: one of the two models, and the box
 * is set to hand DD+ on as a bitstream. EP3_EAC3_RESTART=1 / EP3_EAC3_RESTART=0
 * in the environment forces it on / off, e.g. to try it on another model. */
static bool LinuxDvbEac3NeedsRestart(void)
{
    const char *env = getenv("EP3_EAC3_RESTART");
    char model[32] = "";
    char mode[32] = "";

    if (env && (env[0] == '0' || env[0] == '1') && env[1] == '\0')
        return env[0] == '1';
    if (STB_VUPLUS != GetSTBType())
        return false;
    if (!LinuxDvbReadProcLine("/proc/stb/info/vumodel", model, sizeof(model)) ||
        !LinuxDvbReadProcLine("/proc/stb/audio/ac3", mode, sizeof(mode)))
        return false;
    return (!strcmp(model, "ultimo4k") || !strcmp(model, "duo4kse")) &&
           !strcmp(mode, "passthrough");
}

static long LinuxDvbElapsedMs(const struct timespec *since)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000;
}

/* Polled by the command loop: 2 (once) when the restart is due, 1 while it
 * is expected within milliseconds, 0 otherwise. */
int LinuxDvbEac3RestartState(void)
{
    int state = eac3Restart;

    if (0 == state)
        return 0;
    if (1 == state)
    {
        /* a stream that takes long to deliver its first DD+ packet is not
         * polled fast for ever, the restart then comes with the next poll */
        return LinuxDvbElapsedMs(&eac3PlayTime) < EAC3_RESTART_FAST_POLL_MS ? 1 : 0;
    }
    if (LinuxDvbElapsedMs(&eac3PacketTime) < EAC3_RESTART_DELAY_MS)
        return 1;

    return __sync_bool_compare_and_swap(&eac3Restart, 2, 0) ? 2 : 0;
}

static int LinuxDvbMapBypassMode(int bypass, bool primary)
{
    if (STB_DREAMBOX == GetSTBType()) {
        primary = !primary;
    }

    switch(bypass) {
        case AUDIOTYPE_RAW:
            bypass = primary ? 0x30 : 0xf;
            break;
        case AUDIOTYPE_AC3_PLUS:
            bypass = primary ? 0x22 : 7;
            break;
        case AUDIOTYPE_WMA:
            bypass = primary ? 0x20 : 0xd;
            break;
        case AUDIOTYPE_WMA_PRO:
            bypass = primary ? 0x21 : 0xe;
            break;
        default:
            break;
    }

    return bypass;
}

/*
 * Some drivers pass DTS-HD MA and DTS:X through in full only in their DTS-HD
 * mode (bypass mode 0x10); in plain DTS mode (2) they send the core alone.
 * The track does not say whether the stream carries the HD extension, so the
 * decoder is started in plain DTS mode as always and the first frame written
 * after that decides: with an extension the decoder is restarted in DTS-HD
 * mode before the frame goes out, see LinuxDvbDtsHdStart().
 *
 * Only where the driver's DTS-HD mode is known to cope with a receiver that
 * has no DTS-HD: the drivers with /proc/stb/audio/dtshd.  Others may give
 * such a receiver no sound at all, so everything else stays on plain DTS.
 * EP3_DTS_HD_MODE=1 / EP3_DTS_HD_MODE=0 in the environment forces it on / off.
 */
static int LinuxDvbDtsHdModeUsable(void)
{
    static int usable = -1;
    if (usable < 0)
    {
        const char *env = getenv("EP3_DTS_HD_MODE");
        usable = STB_DREAMBOX != GetSTBType() && access("/proc/stb/audio/dtshd", F_OK) == 0;
        if (env && (env[0] == '0' || env[0] == '1') && env[1] == '\0')
            usable = env[0] == '1';
    }
    return usable;
}

/*
 * Whether a framed DTS buffer carries a DTS-HD extension substream (DTS-HD
 * HRA, DTS-HD MA, DTS:X, DTS Express): it starts with one, or one follows a
 * 16 bit big endian core frame.
 *
 * This function and its comment are dvb_dts_has_hd_extension() of dtsframe.h
 * in the dvbaudiosink of gstreamer1.0-plugin-dvbmediasink as patched by
 * oe-alliance-core (0004-dvbaudiosink-dts-driver-fixes.patch), renamed and
 * reindented.  SPDX-License-Identifier: MIT (the licence of that file).
 */
static int LinuxDvbDtsHasHdExtension(const uint8_t *input, size_t size)
{
    static const uint8_t exss[4] = { 0x64, 0x58, 0x20, 0x25 };
    size_t frame_size;

    if (!input || size < 8)
        return 0;
    if (!memcmp(input, exss, 4))
        return 1;
    if (input[0] != 0x7f || input[1] != 0xfe || input[2] != 0x80 || input[3] != 0x01)
        return 0;
    frame_size = (((size_t)input[5] & 0x03) << 12 | (size_t)input[6] << 4 | input[7] >> 4) + 1;
    return size >= frame_size + 4 && !memcmp(input + frame_size, exss, 4);
}

/* Set where the audio decoder is started for a DTS track in plain DTS mode
 * (LinuxDvbPlay, LinuxDvbSwitch), cleared by the first frame after that. */
static int dts_hd_pending;

/*
 * Called with the first DTS frame after a start of the decoder.  A frame
 * with an HD extension restarts the decoder in DTS-HD mode; nothing has been
 * written to it yet.  Any other frame leaves the decoder as it is.
 *
 * A paused decoder is left alone and a later frame decides: restarting it
 * here could not be told apart from a continue command arriving at the same
 * time, and the decoder would stay paused or start too early.
 */
static void LinuxDvbDtsHdStart(Context_t *context, const uint8_t *data, size_t size)
{
    getLinuxDVBMutex();
    if (dts_hd_pending && audiofd != -1 &&
        context->playback->isPlaying && !context->playback->isPaused)
    {
        dts_hd_pending = 0;
        if (LinuxDvbDtsHasHdExtension(data, size))
        {
            linuxdvb_printf(10, "DTS with HD extension, set bypass 0x%02x\n", AUDIOTYPE_DTS_HD);
            eac3BypassKept = 0;
            if (ioctl(audiofd, AUDIO_STOP, NULL) == -1)
            {
                linuxdvb_err("AUDIO_STOP: ERROR %d, %s\n", errno, strerror(errno));
            }
            if (ioctl(audiofd, AUDIO_CLEAR_BUFFER) == -1)
            {
                linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
            }
            if (ioctl(audiofd, AUDIO_SET_BYPASS_MODE, AUDIOTYPE_DTS_HD) < 0)
            {
                linuxdvb_err("AUDIO_SET_BYPASS_MODE: ERROR %d, %s\n", errno, strerror(errno));
                /* back to the mode the decoder was started in */
                if (ioctl(audiofd, AUDIO_SET_BYPASS_MODE, AUDIOTYPE_DTS) < 0)
                {
                    linuxdvb_err("AUDIO_SET_BYPASS_MODE: ERROR %d, %s\n", errno, strerror(errno));
                }
            }
            if (context->playback->isDash)
                LinuxDvbApplyAVSync(context);
            audioRunning = true;
            if (ioctl(audiofd, AUDIO_PLAY) == -1)
            {
                linuxdvb_err("AUDIO_PLAY: ERROR %d, %s\n", errno, strerror(errno));
            }
            if (ioctl(audiofd, AUDIO_CONTINUE) == -1)
            {
                linuxdvb_err("AUDIO_CONTINUE: ERROR %d, %s\n", errno, strerror(errno));
            }
            LinuxDvbRestoreAudioChannel();
        }
    }
    releaseLinuxDVBMutex();
}

static int LinuxDvbMapStreamType(int streamtype, bool primary)
{
    if (STB_DREAMBOX == GetSTBType()) {
        primary = !primary;
    }

    switch(streamtype) {
        case STREAMTYPE_MPEG4_H265:
            streamtype = primary ? 7 : 22;
            break;
        case STREAMTYPE_VC1:
            streamtype = primary ? 3 : 16;
            break;
        case STREAMTYPE_VC1_SM:
            streamtype = primary ? 5 : 17;
            break;
        case STREAMTYPE_VB8:
            streamtype = primary ? 8 : 20;
            break;
        case STREAMTYPE_VB9:
            streamtype = primary ? 9 : 23;
            break;
        default:
            break;
    }

    return streamtype;
}

static int LinuxDvbOpenRetryOnBusy(const char *path, int flags)
{
    int waited = 0, fd;
    do
    {
        fd = open(path, flags);
        if (fd >= 0 || errno != EBUSY || waited == 250000)
            return fd;
        if (PlaybackDieNow(0))
        {
            errno = ECANCELED;
            return -1;
        }
        usleep(10000);
        waited += 10000;
    } while (1);
}

int LinuxDvbOpen(Context_t  *context __attribute__((unused)), char *type)
{
    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    if (video && videofd < 0) 
    {
        videofd = LinuxDvbOpenRetryOnBusy(VIDEODEV, O_RDWR | O_NONBLOCK);

        if (videofd < 0)
        {
            linuxdvb_err("failed to open %s - errno %d, %s\n", VIDEODEV, errno, strerror(errno));
            return cERR_LINUXDVB_ERROR;
        }
 
        if (ioctl( videofd, VIDEO_CLEAR_BUFFER) == -1)
        {
            linuxdvb_err("VIDEO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
        }

        if (ioctl( videofd, VIDEO_SELECT_SOURCE, (void*)VIDEO_SOURCE_MEMORY) == -1)
        {
            linuxdvb_err("VIDEO_SELECT_SOURCE: ERROR %d, %s\n", errno, strerror(errno));
        }
        
        if (ioctl(videofd, VIDEO_FREEZE) == -1)
        {
            linuxdvb_err("VIDEO_FREEZE: ERROR %d, %s\n", errno, strerror(errno));
        }
        videoRunning = true;
        videoOnMemory = true;

        if (isBufferedOutput)
            LinuxDvbBuffOpen(context, type, videofd, &LinuxDVBmutex);
    }
    if (audio && audiofd < 0) 
    {
        audiofd = LinuxDvbOpenRetryOnBusy(AUDIODEV, O_RDWR | O_NONBLOCK);

        if (audiofd < 0)
        {
            linuxdvb_err("failed to open %s - errno %d, %s\n", AUDIODEV, errno, strerror(errno));
            return cERR_LINUXDVB_ERROR;
        }

        if (ioctl( audiofd, AUDIO_CLEAR_BUFFER) == -1)
        {
            linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
        }

        if (ioctl( audiofd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY) == -1)
        {
            linuxdvb_err("AUDIO_SELECT_SOURCE: ERROR %d, %s\n", errno, strerror(errno));
        }

#ifdef HAVE_DREAMNEXTGEN
        /* the kernel syncs the audio of the dreamaudio device to the video */
        if (ioctl(audiofd, AUDIO_SET_AV_SYNC, 1) == -1)
        {
            linuxdvb_err("AUDIO_SET_AV_SYNC: ERROR %d, %s\n", errno, strerror(errno));
        }
#endif
        
        if (ioctl( audiofd, AUDIO_PAUSE) == -1)
        {
            linuxdvb_err("AUDIO_PAUSE: ERROR %d, %s\n", errno, strerror(errno));
        }
        audioRunning = true;
        audioOnMemory = true;

        if (isBufferedOutput)
            LinuxDvbBuffOpen(context, type, audiofd, &LinuxDVBmutex);
    }

    return cERR_LINUXDVB_NO_ERROR;
}

int LinuxDvbClose(Context_t  *context, char *type)
{
    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    /* closing stand alone is not allowed, so prevent
     * user from closing and don't call stop. stop will
     * set default values for us (speed and so on).
     * After OUTPUT_STOP there is nothing left for it to do.
     */
    LinuxDvbStop(context, type);

    getLinuxDVBMutex();
    
    if (isBufferedOutput)
        LinuxDvbBuffClose(context);

    if (video && videofd != -1)
    {
        close(videofd);
        videofd = -1;
    }
    if (audio && audiofd != -1) 
    {
        close(audiofd);
        audiofd = -1;
    }

    releaseLinuxDVBMutex();
    return cERR_LINUXDVB_NO_ERROR;
}

/* Broadcom drivers take the frame rate from this node when the stream itself
 * carries none (H.264 without VUI timing, MPEG-4 part 2, some VC-1) and report
 * it to the front end. Written before VIDEO_SET_STREAMTYPE with the frame rate
 * of the container, the nearest the driver accepts, as gstdvbvideosink and
 * libstbplayer do; a box without the node is left alone. */
static void LinuxDvbSetFallbackFramerate(Context_t *context)
{
    static const uint32_t rates[] = {23976, 24000, 25000, 29970, 30000, 50000, 59940, 60000};
    Track_t *track = NULL;
    uint32_t best = rates[0], i;
    FILE *f;

    if (context->manager->video->Command(context, MANAGER_GET_TRACK, &track) < 0 ||
        track == NULL || track->frame_rate == 0)
    {
        return;
    }
    for (i = 1; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        uint32_t diff = track->frame_rate > rates[i] ? track->frame_rate - rates[i] : rates[i] - track->frame_rate;
        uint32_t bestDiff = track->frame_rate > best ? track->frame_rate - best : best - track->frame_rate;
        if (diff < bestDiff)
            best = rates[i];
    }
    f = fopen("/proc/stb/vmpeg/0/fallback_framerate", "w");
    if (f == NULL)
    {
        linuxdvb_printf(20, "no fallback_framerate: %s\n", strerror(errno));
        return;
    }
    fprintf(f, "%u", best);
    fclose(f);
    linuxdvb_printf(10, "fallback_framerate %u for %u\n", best, track->frame_rate);
}

int LinuxDvbPlay(Context_t  *context, char *type) {
    int32_t ret = cERR_LINUXDVB_NO_ERROR;
    Writer_t *writer;

    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    if (video && videofd != -1) {
        char *Encoding = NULL;
        context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);

        linuxdvb_printf(10, "V %s\n", Encoding);

        writer = getWriter(Encoding);
        if (writer == NULL)
        {
            linuxdvb_err("cannot found writer for encoding %s using default\n", Encoding);
            ret = cERR_LINUXDVB_ERROR;
        }
        else
        {
            linuxdvb_printf(20, "found writer %s for encoding %s\n", writer->caps->name, Encoding);
            if (writer->reset)
                writer->reset();
            LinuxDvbSetFallbackFramerate(context);
            ret = ioctl( videofd, VIDEO_SET_STREAMTYPE, LinuxDvbMapStreamType(writer->caps->dvbStreamType, true));
            if (ret < 0) ret = ioctl( videofd, VIDEO_SET_STREAMTYPE, LinuxDvbMapStreamType(writer->caps->dvbStreamType, false));
            if (ret < 0)
            {
                linuxdvb_err("VIDEO_SET_STREAMTYPE: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }
        free(Encoding);

        videoRunning = true;
        if (0 != ioctl(videofd, VIDEO_PLAY))
        {
            linuxdvb_err("VIDEO_PLAY: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
        
        if (ioctl(videofd, VIDEO_CONTINUE) == -1)
        {
            linuxdvb_err("VIDEO_CONTINUE: ERROR %d, %s\n", errno, strerror(errno));
        }
        
        if (ioctl( videofd, VIDEO_CLEAR_BUFFER) == -1)
        {
            linuxdvb_err("VIDEO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
        }
    }
    if (audio && audiofd != -1) {
        char * Encoding = NULL;
        context->manager->audio->Command(context, MANAGER_GETENCODING, &Encoding);

        linuxdvb_printf(20, "0 A %s\n", Encoding);

        writer = getWriter(Encoding);
        dts_hd_pending = writer && AUDIOTYPE_DTS == writer->caps->dvbStreamType && LinuxDvbDtsHdModeUsable();
        /* the first switch after a start always restarts the decoder */
        eac3BypassKept = 0;
        
        if (writer == NULL)
        {
            linuxdvb_err("cannot found writer for encoding %s using default\n", Encoding);
            ret = cERR_LINUXDVB_ERROR;
        } 
        else
        {
            linuxdvb_printf(20, "found writer %s for encoding %s\n", writer->caps->name, Encoding);
            ret = ioctl( audiofd, AUDIO_SET_BYPASS_MODE, LinuxDvbMapBypassMode(writer->caps->dvbStreamType, true));
            if (ret < 0) ret = ioctl( audiofd, AUDIO_SET_BYPASS_MODE, LinuxDvbMapBypassMode(writer->caps->dvbStreamType, false));
            if (ret < 0)
            {
                linuxdvb_err("AUDIO_SET_BYPASS_MODE: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }

        if (context->playback->isDash)
        {
            if (ret < 0)
            {
                free(Encoding);
                return ret;
            }
            getLinuxDVBMutex();
            ret = LinuxDvbApplyAVSync(context);
            releaseLinuxDVBMutex();
            if (ret < 0)
            {
                free(Encoding);
                return ret;
            }
            timing_after_switch = 1;
        }
        /* the first audio start only, a loop restart plays on */
        if (!eac3RestartChecked)
        {
            eac3RestartChecked = 1;
            if (writer && AUDIOTYPE_AC3_PLUS == writer->caps->dvbStreamType &&
                !context->playback->isDash && LinuxDvbEac3NeedsRestart())
            {
                linuxdvb_printf(10, "DD+ passthrough start, audio restart armed\n");
                clock_gettime(CLOCK_MONOTONIC, &eac3PlayTime);
                eac3Restart = 1;
            }
        }
        audioRunning = true;
        if (ioctl(audiofd, AUDIO_PLAY) < 0)
        {
            linuxdvb_err("AUDIO_PLAY: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }

        if (ioctl(audiofd, AUDIO_CONTINUE) < 0)
        {
            linuxdvb_err("AUDIO_CONTINUE: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
        getLinuxDVBMutex();
        LinuxDvbRestoreAudioChannel();
        releaseLinuxDVBMutex();
        free(Encoding);
    }

    //return ret;
    return context->playback->isDash ? ret : 0;
}

int LinuxDvbStop(Context_t  *context __attribute__((unused)), char * type) 
{
    int ret = cERR_LINUXDVB_NO_ERROR;
    unsigned char video = !strcmp("video", type);
    unsigned char audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    getLinuxDVBMutex();

    /* a decoder that is stopped already is left alone, see videoRunning */
    if (video && videofd != -1)
    {
        if (videoRunning)
        {
            videoRunning = false;
            if (ioctl(videofd, VIDEO_CLEAR_BUFFER) == -1)
            {
                linuxdvb_err("VIDEO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
            }

            if (ioctl(videofd, VIDEO_STOP) == -1)
            {
                linuxdvb_err("VIDEO_STOP: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }

            ioctl(videofd, VIDEO_SLOWMOTION, 0);
            ioctl(videofd, VIDEO_FAST_FORWARD, 0);
        }
        if (videoOnMemory)
        {
            videoOnMemory = false;
            ioctl(videofd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
        }
    }
    if (audio && audiofd != -1) {
        /* not left or right for what plays next, DVB services included;
         * the selection itself stays for a start that follows */
        if (OUTPUT_AUDIO_CHANNEL_STEREO != audioChannelApplied)
            LinuxDvbSetAudioChannel(OUTPUT_AUDIO_CHANNEL_STEREO);

        if (audioRunning)
        {
            audioRunning = false;
            if (ioctl(audiofd, AUDIO_CLEAR_BUFFER) == -1)
            {
                linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
            }

            if (ioctl(audiofd, AUDIO_STOP) == -1)
            {
                linuxdvb_err("AUDIO_STOP: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }
        if (audioOnMemory)
        {
            audioOnMemory = false;
            ioctl(audiofd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX);
        }
        eac3Restart = 0;
        eac3BypassKept = 0;
    }

    releaseLinuxDVBMutex();

    /* a write that waits for room tries again now, and ends at a stop */
    if (!isBufferedOutput)
        WriterWakeUp();

    return ret;
}

int LinuxDvbPause(Context_t  *context __attribute__((unused)), char *type) {
    int32_t ret = cERR_LINUXDVB_NO_ERROR;
    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    getLinuxDVBMutex();

    if (video && videofd != -1) 
    {
        if (ioctl(videofd, VIDEO_FREEZE, NULL) == -1)
        {
            linuxdvb_err("VIDEO_FREEZE: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
    }
    
    if (audio && audiofd != -1)
    {
        if (ioctl(audiofd, AUDIO_PAUSE, NULL) == -1)
        {
            linuxdvb_err("AUDIO_PAUSE: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
    }

    releaseLinuxDVBMutex();

    return ret;
}

int LinuxDvbContinue(Context_t  *context __attribute__((unused)), char * type) {
    int32_t ret = cERR_LINUXDVB_NO_ERROR;
    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    if (video && videofd != -1)
    {
        if (ioctl(videofd, VIDEO_CONTINUE, NULL) == -1)
        {
            linuxdvb_err("VIDEO_CONTINUE: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
    }
    
    if (audio && audiofd != -1)
    {
        if (ioctl(audiofd, AUDIO_CONTINUE, NULL) == -1)
        {
            linuxdvb_err("AUDIO_CONTINUE: ERROR %d, %s\n", errno, strerror(errno));
            ret = cERR_LINUXDVB_ERROR;
        }
    }
    
    if (isBufferedOutput)
        LinuxDvbBuffResume(context);
    else
        WriterWakeUp();

    linuxdvb_printf(10, "exiting\n");

    return ret;
}

int LinuxDvbAudioMute(Context_t  *context __attribute__((unused)), char *flag) {
    int ret = cERR_LINUXDVB_NO_ERROR;

    linuxdvb_printf(10, "\n");

    if (audiofd != -1) {
        if(*flag == '1')
        {
            audioRunning = false;
            if (ioctl(audiofd, AUDIO_STOP, NULL) == -1)
            {
                linuxdvb_err("AUDIO_STOP: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }
        else
        {
            audioRunning = true;
            if (ioctl(audiofd, AUDIO_PLAY) == -1)
            {
                linuxdvb_err("AUDIO_PLAY: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
            else
            {
                getLinuxDVBMutex();
                LinuxDvbRestoreAudioChannel();
                releaseLinuxDVBMutex();
            }
        }
    }

    linuxdvb_printf(10, "exiting\n");

    return ret;
}

int LinuxDvbFlush(Context_t  *context __attribute__((unused)), char * type) 
{
    return cERR_LINUXDVB_NO_ERROR;
}

int LinuxDvbSlowMotion(Context_t  *context, char * type) {
    int32_t ret = cERR_LINUXDVB_NO_ERROR;

    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    if ( (video && videofd != -1) || (audio && audiofd != -1) ) {
        getLinuxDVBMutex();

        if (video && videofd != -1) {
            if (ioctl(videofd, VIDEO_SLOWMOTION, context->playback->SlowMotion) == -1)
            {
                linuxdvb_err("VIDEO_SLOWMOTION: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }

        releaseLinuxDVBMutex();
    }

    linuxdvb_printf(10, "exiting with value %d\n", ret);

    return ret;
}

int LinuxDvbAVSync(Context_t  *context, char *type __attribute__((unused))) {
    int32_t ret = cERR_LINUXDVB_NO_ERROR;
    /* konfetti: this one is dedicated to audiofd so we
     * are ignoring what is given by type! I think we should
     * remove this param. Therefor we should add a variable
     * setOn or something like that instead, this would remove
     * using a variable inside the structure.
     */
    if (audiofd != -1) {
        getLinuxDVBMutex();

        ret = LinuxDvbApplyAVSync(context);
        LinuxDvbTiming("startup");

        releaseLinuxDVBMutex();
    }

    return ret;
}

int LinuxDvbClear(Context_t  *context __attribute__((unused)), char *type) 
{
    int32_t ret = cERR_LINUXDVB_NO_ERROR;
    uint8_t video = !strcmp("video", type);
    uint8_t audio = !strcmp("audio", type);

    linuxdvb_printf(10, "LinuxDvbClear v%d a%d\n", video, audio);

    if ( (video && videofd != -1) || (audio && audiofd != -1) ) 
    {
        getLinuxDVBMutex();

        if (video && videofd != -1) 
        {
            if (ioctl(videofd, VIDEO_CLEAR_BUFFER) == -1)
            {
                linuxdvb_err("VIDEO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }
        else if (audio && audiofd != -1) 
        {
            if (ioctl(audiofd, AUDIO_CLEAR_BUFFER) == -1)
            {
                linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
            if (context->manager && context->manager->audio)
            {
                char *encoding = NULL;
                context->manager->audio->Command(context, MANAGER_GETENCODING, &encoding);
                Writer_t *writer = encoding ? getWriter(encoding) : NULL;
                if (!encoding)
                {
                    linuxdvb_err("failed to obtain audio encoding\n");
                    ret = cERR_LINUXDVB_ERROR;
                }
                if (writer && writer->caps->dvbStreamType == AUDIOTYPE_RAW)
                {
                    if (writer->reset)
                        writer->reset();
                    eac3BypassKept = 0;
                    if (ioctl(audiofd, AUDIO_SET_BYPASS_MODE,
                              LinuxDvbMapBypassMode(AUDIOTYPE_RAW, true)) < 0 &&
                        ioctl(audiofd, AUDIO_SET_BYPASS_MODE,
                              LinuxDvbMapBypassMode(AUDIOTYPE_RAW, false)) < 0)
                    {
                        linuxdvb_err("AUDIO_SET_BYPASS_MODE: %s\n", strerror(errno));
                        ret = cERR_LINUXDVB_ERROR;
                    }
                    /* The buffer is cleared at this point. Restarting a decoder
                     * that still runs is not an error of the clear. */
                    audioRunning = true;
                    if (ioctl(audiofd, AUDIO_PLAY) < 0)
                    {
                        linuxdvb_err("AUDIO_PLAY: %s\n", strerror(errno));
                    }
                    LinuxDvbRestoreAudioChannel();
                }
                free(encoding);
            }
            if (ioctl(audiofd, AUDIO_CONTINUE) < 0)
            {
                linuxdvb_err("AUDIO_CONTINUE: %s\n", strerror(errno));
            }
        }

        releaseLinuxDVBMutex();
    }

    /* the buffer is empty: a write that waits for room tries again now */
    if (!isBufferedOutput)
        WriterWakeUp();

    linuxdvb_printf(10, "exiting\n");

    return ret;
}

int LinuxDvbPts(Context_t  *context __attribute__((unused)), unsigned long long int* pts) {
    int32_t ret = cERR_LINUXDVB_ERROR;
    int zeroVideoPts = 0;
    /* not sCURRENT_PTS: the command, play and live sync threads ask at once */
    unsigned long long int current = 0;

    linuxdvb_printf(50, "\n");

    // GET_PTS is immutable call, so it can be done in parallel to other requests
    if (videofd > -1 && !ioctl(videofd, VIDEO_GET_PTS, (void*)&current))
    {
        ret = cERR_LINUXDVB_NO_ERROR;
        /* front ends ask for the position all the time, also when no more data is written */
        LinuxDvbVideoInfoPoll(context, current);
        /* Some Broadcom drivers answer VIDEO_GET_PTS with 0 for the whole
         * memory-source session, or until the decoder runs again. After video
         * with a PTS was written, 0 is no position: the audio decoder's PTS is
         * taken when it has one. */
        zeroVideoPts = current == 0 && videoPtsWritten;
        /* a real video position is kept within the written video */
        if (!zeroVideoPts)
            current = PosBoundApply(&videoPosBound, current);
    }
    else if (videofd > -1)
    {
        linuxdvb_err("VIDEO_GET_PTS: ERROR %d, %s\n", errno, strerror(errno));
    }

    if (zeroVideoPts)
    {
        unsigned long long int audioPts = 0;
        if (audiofd > -1 && !ioctl(audiofd, AUDIO_GET_PTS, (void*)&audioPts) && audioPts != 0)
        {
            /* the audio position, kept within the written audio */
            current = PosBoundApply(&audioPosBound, audioPts);
        }
        else
        {
            linuxdvb_printf(50, "VIDEO_GET_PTS 0 after written video, no audio PTS\n");
        }
    }
    else if (ret != cERR_LINUXDVB_NO_ERROR)
    {
        if (audiofd > -1 && !ioctl(audiofd, AUDIO_GET_PTS, (void*)&current))
        {
            ret = cERR_LINUXDVB_NO_ERROR;
            current = PosBoundApply(&audioPosBound, current);
        }
        else if (audiofd > -1)
        {
            linuxdvb_err("AUDIO_GET_PTS: ERROR %d, %s\n", errno, strerror(errno));
        }
    }

    if (ret != cERR_LINUXDVB_NO_ERROR)
    {
        current = 0;
    }

    *((unsigned long long int *)pts)=(unsigned long long int)current;
    return ret;
}

int LinuxDvbGetFrameCount(Context_t  *context __attribute__((unused)), unsigned long long int* frameCount)
{
    return cERR_LINUXDVB_NO_ERROR;
}

int LinuxDvbSwitch(Context_t  *context, char *type) 
{
    uint8_t audio = !strcmp("audio", type);
    uint8_t video = !strcmp("video", type);
    Writer_t *writer;
    int ret = 0;
    int keepEac3 = 0;
    bool eac3Box = false;

    linuxdvb_printf(10, "v%d a%d\n", video, audio);

    if ( (video && videofd != -1) || (audio && audiofd != -1) ) {
        if (audio && context->playback->isDash)
        {
            LinuxDvbTiming("before-switch");
            if (isBufferedOutput && LinuxDvbBuffFlushAudio(context) < 0)
            {
                linuxdvb_err("failed to flush buffered audio for DASH switch\n");
                return cERR_LINUXDVB_ERROR;
            }
        }
        /* reads /proc files: before the mutex is taken */
        if (audio && audiofd != -1 && !context->playback->isDash)
            eac3Box = LinuxDvbEac3NeedsRestart();
        getLinuxDVBMutex();

        if (audio && audiofd != -1) {
            char * Encoding = NULL;
            if (context && context->manager && context->manager->audio) {
                context->manager->audio->Command(context, MANAGER_GETENCODING, &Encoding);

                linuxdvb_printf(10, "A %s\n", Encoding);

                writer = getWriter(Encoding);
                dts_hd_pending = writer && AUDIOTYPE_DTS == writer->caps->dvbStreamType && LinuxDvbDtsHdModeUsable();

                /* DD+ -> DD+ (native E-AC-3, DTS or TrueHD transcoded to
                 * E-AC-3) on a Vu+ Ultimo 4K / Duo 4K SE whose decoder a switch
                 * has already started in DD+ bypass mode: no stop and no new
                 * bypass mode, which would silence the HDMI output, only the
                 * old track is flushed. A paused decoder stays paused. */
                if (eac3BypassKept && writer && AUDIOTYPE_AC3_PLUS == writer->caps->dvbStreamType &&
                    !context->playback->isDash)
                {
                    linuxdvb_printf(10, "DD+ -> DD+ switch, decoder kept running\n");
                    if (ioctl(audiofd, AUDIO_CLEAR_BUFFER) == -1)
                    {
                        linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
                    }
                    goto audio_switch_done;
                }
                eac3BypassKept = 0;

                /* a DASH switch that fails below leaves the decoder stopped */
                audioRunning = false;
                if (ioctl(audiofd, AUDIO_STOP ,NULL) == -1)
                {
                    linuxdvb_err("AUDIO_STOP: ERROR %d, %s\n", errno, strerror(errno));
                    if (context->playback->isDash)
                    {
                        ret = cERR_LINUXDVB_ERROR;
                        goto audio_switch_done;
                    }
                }

                if (ioctl(audiofd, AUDIO_CLEAR_BUFFER) == -1)
                {
                    linuxdvb_err("AUDIO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
                    if (context->playback->isDash)
                    {
                        ret = cERR_LINUXDVB_ERROR;
                        goto audio_switch_done;
                    }
                }

                if (writer == NULL)
                {
                    linuxdvb_err("cannot found writer for encoding %s using default\n", Encoding);
                    if (context->playback->isDash)
                    {
                        ret = cERR_LINUXDVB_ERROR;
                        goto audio_switch_done;
                    }
                } 
                else
                {
                    if (context->playback->isDash && writer->reset && writer->reset() < 0)
                    {
                        linuxdvb_err("failed to reset DASH audio writer\n");
                        ret = cERR_LINUXDVB_ERROR;
                        goto audio_switch_done;
                    }
                    linuxdvb_printf(10, "found writer %s for encoding %s\n", writer->caps->name, Encoding);
                    ret = ioctl( audiofd, AUDIO_SET_BYPASS_MODE, LinuxDvbMapBypassMode(writer->caps->dvbStreamType, true));
                    if (ret < 0) ret = ioctl( audiofd, AUDIO_SET_BYPASS_MODE, LinuxDvbMapBypassMode(writer->caps->dvbStreamType, false));
                    if (ret < 0)
                    {
                        linuxdvb_err("AUDIO_SET_BYPASS_MODE: ERROR %d, %s\n", errno, strerror(errno));
                        if (context->playback->isDash)
                            goto audio_switch_done;
                        /* only logged, the track is switched all the same */
                        ret = 0;
                    }
                    else
                    {
                        keepEac3 = AUDIOTYPE_AC3_PLUS == writer->caps->dvbStreamType && eac3Box;
                    }
                }

                if (context->playback->isDash)
                {
                    ret = LinuxDvbApplyAVSync(context);
                    if (ret < 0)
                    {
                        goto audio_switch_done;
                    }
                    timing_after_switch = 1;
                }
                audioRunning = true;
                if (ioctl(audiofd, AUDIO_PLAY) == -1)
                {
                    linuxdvb_err("AUDIO_PLAY: ERROR %d, %s\n", errno, strerror(errno));
                    if (context->playback->isDash)
                        ret = cERR_LINUXDVB_ERROR;
                }
                else
                {
                    eac3BypassKept = keepEac3;
                    /* the new codec can reset the left/right selection */
                    LinuxDvbRestoreAudioChannel();
                    /* AUDIO_PLAY starts the decoder: hold it again while the
                     * playback is paused, the continue command releases it */
                    if (context->playback->isPaused && ioctl(audiofd, AUDIO_PAUSE, NULL) == -1)
                    {
                        linuxdvb_err("AUDIO_PAUSE: ERROR %d, %s\n", errno, strerror(errno));
                    }
                }
audio_switch_done:
                free(Encoding);
            }
            else
                linuxdvb_printf(20, "no context for Audio\n");
        }

        if (video && videofd != -1) {
            char * Encoding = NULL;
            if (context && context->manager && context->manager->video) {
                context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);

                if (ioctl(videofd, VIDEO_STOP ,NULL) == -1)
                {
                    linuxdvb_err("VIDEO_STOP: ERROR %d, %s\n", errno, strerror(errno));
                }

                if (ioctl(videofd, VIDEO_CLEAR_BUFFER) == -1)
                {
                    linuxdvb_err("VIDEO_CLEAR_BUFFER: ERROR %d, %s\n", errno, strerror(errno));
                }

                linuxdvb_printf(10, "V %s\n", Encoding);

                writer = getWriter(Encoding);

                if (writer == NULL)
                {
                    linuxdvb_err("cannot found writer for encoding %s using default\n", Encoding);
                } 
                else
                {
                    linuxdvb_printf(10, "found writer %s for encoding %s\n", writer->caps->name, Encoding);
                    /* the restarted decoder needs the codec data again */
                    if (writer->reset)
                        writer->reset();
                    ret = ioctl( videofd, VIDEO_SET_STREAMTYPE, LinuxDvbMapStreamType(writer->caps->dvbStreamType, true));
                    if (ret < 0) ret = ioctl( videofd, VIDEO_SET_STREAMTYPE, LinuxDvbMapStreamType(writer->caps->dvbStreamType, false));
                    if (ret < 0)
                    {
                        linuxdvb_err("VIDEO_SET_STREAMTYPE: ERROR %d, %s\n", errno, strerror(errno));
                        /* only logged, the decoder is restarted all the same */
                        ret = 0;
                    }
                }

                videoRunning = true;
                if (ioctl(videofd, VIDEO_PLAY) == -1)
                {
                    /* konfetti: fixme: think on this, I think we should
                     * return an error here and stop the playback mode
                     */
                    linuxdvb_err("VIDEO_PLAY:ERROR %d, %s\n", errno, strerror(errno));
                }
                free(Encoding);
            }
            else
                linuxdvb_printf(20, "no context for Video\n");
        }

        releaseLinuxDVBMutex();

    }

    linuxdvb_printf(10, "exiting\n");

    return ret;
}

/*
 * Tells whether the driver has a video event queued. Not under
 * videoInfoMutex: the Vu+ (Broadcom) driver sleeps in its poll for up to
 * 200 ms while the decoder buffer is full, also with a timeout of 0, and the
 * position requests of the command thread wait for that mutex.
 */
static bool LinuxDvbVideoEventPending(void)
{
    struct pollfd pfd[1];
    pfd[0].fd = videofd;
    pfd[0].events = POLLPRI;
    pfd[0].revents = 0;
    return poll(pfd, 1, 0) > 0 && (pfd[0].revents & POLLPRI);
}

/*
 * Reads one video event the driver has queued (LinuxDvbVideoEventPending());
 * the caller holds videoInfoMutex. progressive is set for a progressive
 * event, values when the events have delivered all values ("v_e"). Returns
 * true when an event was read.
 */
static bool LinuxDvbReadVideoEvent(bool *progressive, bool *values)
{
    struct video_event evt;
    bool changed = false;

    if (ioctl(videofd, VIDEO_GET_EVENT, &evt) == -1)
    {
        /* the other thread has read it since the poll */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return false;
        linuxdvb_err("ioctl failed with errno %d\n", errno);
        linuxdvb_err("VIDEO_GET_EVENT: %s\n", strerror(errno));
        return false;
    }

    if (evt.type == VIDEO_EVENT_SIZE_CHANGED)
    {
        linuxdvb_printf(10, "VIDEO_EVENT_SIZE_CHANGED type: 0x%x\n", evt.type);
        linuxdvb_printf(10, "width  : %d\n", evt.u.size.w);
        linuxdvb_printf(10, "height : %d\n", evt.u.size.h);
        linuxdvb_printf(10, "aspect : %d\n", evt.u.size.aspect_ratio);
        videoInfo.width = evt.u.size.w;
        videoInfo.height = evt.u.size.h;
        videoInfo.aspect_ratio = evt.u.size.aspect_ratio;
        changed = true;
    }
    else if (evt.type == VIDEO_EVENT_FRAME_RATE_CHANGED)
    {
        linuxdvb_printf(10, "VIDEO_EVENT_FRAME_RATE_CHANGED type: 0x%x\n", evt.type);
        linuxdvb_printf(10, "framerate : %d\n", evt.u.frame_rate);
        videoInfo.frame_rate = evt.u.frame_rate;
        changed = true;
    }
    else if (evt.type == 16 /*VIDEO_EVENT_PROGRESSIVE_CHANGED*/)
    {
        linuxdvb_printf(10, "VIDEO_EVENT_PROGRESSIVE_CHANGED type: 0x%x\n", evt.type);
        linuxdvb_printf(10, "progressive : %d\n", evt.u.frame_rate);
        videoInfo.progressive = evt.u.frame_rate;
        *progressive = true;
        changed = true;
    }
    else
    {
        linuxdvb_err("unhandled DVBAPI Video Event %d\n", evt.type);
    }

    if (changed &&
        videoInfo.width != -1 &&
        videoInfo.height != -1 &&
        videoInfo.aspect_ratio != -1 &&
        videoInfo.frame_rate != -1 &&
        videoInfo.progressive != -1)
    {
        *values = true;
    }
    return true;
}

static void LinuxDvbSendVideoEvent(const struct DVBApiVideoInfo_s *info)
{
    E2iSendMsg("{\"v_e\":{\"w\":%d,\"h\":%d,\"a\":%d,\"f\":%d,\"p\":%d}}\n",
        info->width, info->height, info->aspect_ratio, info->frame_rate, info->progressive);
}

/*
 * Asks the driver for the values no video event has delivered, the way
 * Enigma2 does it. There is no ioctl for the progressive flag.
 */
static void LinuxDvbAskVideoInfo(void)
{
    static const struct DVBApiVideoInfo_s unknown = {-1,-1,-1,-1,-1};

    videoInfoAsked = unknown;
    if (videoInfo.width <= 0 || videoInfo.height <= 0)
    {
        video_size_t size;
        memset(&size, 0, sizeof(size));
        if (ioctl(videofd, VIDEO_GET_SIZE, &size) == -1)
        {
            linuxdvb_printf(10, "video info: VIDEO_GET_SIZE: %s\n", strerror(errno));
        }
        else if (size.w > 0 && size.h > 0)
        {
            videoInfoAsked.width = size.w;
            videoInfoAsked.height = size.h;
        }
        else
        {
            linuxdvb_printf(10, "video info: VIDEO_GET_SIZE: %dx%d\n", size.w, size.h);
        }
    }

    if (videoInfo.frame_rate <= 0)
    {
        unsigned int frame_rate = 0;
        if (ioctl(videofd, VIDEO_GET_FRAME_RATE, &frame_rate) == -1)
        {
            linuxdvb_printf(10, "video info: VIDEO_GET_FRAME_RATE: %s\n", strerror(errno));
        }
        else if (frame_rate > 0 && frame_rate <= INT32_MAX)
        {
            videoInfoAsked.frame_rate = frame_rate;
        }
    }

    if (videoInfo.progressive == -1)
    {
        FILE *file = fopen(VIDEO_PROGRESSIVE_FILE, "r");
        if (file)
        {
            unsigned int progressive = 0;
            if (fscanf(file, "%x", &progressive) == 1)
            {
                videoInfoAsked.progressive = progressive ? 1 : 0;
            }
            fclose(file);
        }
    }

    /* -1: known from an event, so not asked, or no answer */
    linuxdvb_printf(10, "video info asked from the decoder: %dx%d framerate %d progressive %d\n",
                    videoInfoAsked.width, videoInfoAsked.height, videoInfoAsked.frame_rate, videoInfoAsked.progressive);
}

/*
 * Called for every video frame of a track FFmpeg has no picture size of, or
 * whose progressive flag no event has brought yet, and for every position
 * request while it plays; the caller holds
 * videoInfoMutex. event tells that a video event has been read just now,
 * pts is the decoder PTS when the caller has read it. Returns true when the
 * track is to be reported with the values of the decoder.
 *
 * The driver sends VIDEO_EVENT_SIZE_CHANGED, VIDEO_EVENT_FRAME_RATE_CHANGED
 * and the progressive event only for a value that differs from the one of
 * the previous playback; a decoder that showed 1280x720 before reports
 * nothing for the next 1280x720 stream. What no event delivers has to be
 * asked from the driver, but right after the start it still answers with the
 * values of the previous playback, and the events of this stream follow a
 * moment after it has been asked. So:
 * - without an event the driver is asked after a short quiet period; that
 *   answer is never used;
 * - once no event has come for VIDEO_INFO_SETTLE_PTS, the values the events
 *   left open are asked again and the track is reported once, with the final
 *   values. Without a picture size the driver is asked again later. For a
 *   picture FFmpeg knows only the progressive flag is used, and the track is
 *   reported only when the driver has one.
 * The periods are measured with the decoder PTS; the first step of it, from
 * the value the previous playback left, does not count. A file is often
 * written to the end before that, so the position requests of the front end
 * go on with it.
 */
static bool LinuxDvbUpdateVideoInfo(bool event, const unsigned long long *knownPts)
{
    unsigned long long pts = 0, step = 0;

    if (videoInfoPhase == VIDEO_INFO_DONE)
    {
        return false;
    }

    if (knownPts)
    {
        pts = *knownPts;
    }
    else if (ioctl(videofd, VIDEO_GET_PTS, (void*)&pts) == -1)
    {
        linuxdvb_printf(10, "video info: VIDEO_GET_PTS: %s\n", strerror(errno));
        if (event)
        {
            videoInfoPhase = VIDEO_INFO_EVENTS;
        }
        else if (videoInfoPhase != VIDEO_INFO_EVENTS)
        {
            /* nothing tells when the decoder has left the previous playback */
            videoInfoPhase = VIDEO_INFO_DONE;
        }
        return false;
    }

    if (videoInfoPtsSamples == 0)
    {
        videoInfoPtsSamples = 1;
    }
    else if (pts != videoInfoPts)
    {
        if (videoInfoPtsSamples == 1)
        {
            /* the step from the PTS the previous playback has left */
            videoInfoPtsSamples = 2;
        }
        else if (pts > videoInfoPts && pts - videoInfoPts <= VIDEO_INFO_MAX_STEP)
        {
            step = pts - videoInfoPts;
        }
    }
    videoInfoPts = pts;

    if (event)
    {
        /* the period starts again with the last event */
        videoInfoPhase = VIDEO_INFO_EVENTS;
        videoInfoProgress = 0;
        return false;
    }
    videoInfoProgress += step;

    if (videoInfoPhase == VIDEO_INFO_START)
    {
        videoInfoPhase = VIDEO_INFO_QUIET;
    }

    if (videoInfoPhase == VIDEO_INFO_QUIET)
    {
        if (videoInfoProgress >= VIDEO_INFO_QUIET_PTS)
        {
            /* the answer can still be the previous playback; it makes the driver send its events */
            LinuxDvbAskVideoInfo();
            videoInfoPhase = VIDEO_INFO_HOLD;
            videoInfoProgress = 0;
        }
        return false;
    }

    if (videoInfoProgress < VIDEO_INFO_SETTLE_PTS)
    {
        return false;
    }

    LinuxDvbAskVideoInfo();
    videoInfoProgress = 0;
    if (!videoInfoNoSize)
    {
        /* FFmpeg knows the picture: only the progressive flag was missing */
        videoInfoPhase = VIDEO_INFO_DONE;
        if (videoInfo.progressive != -1 || videoInfoAsked.progressive == -1)
        {
            return false;
        }
        videoInfoReported = true;
        return true;
    }
    if ((videoInfo.width <= 0 || videoInfo.height <= 0) &&
        (videoInfoAsked.width <= 0 || videoInfoAsked.height <= 0))
    {
        if (++videoInfoRetries < VIDEO_INFO_MAX_RETRIES)
        {
            return false;
        }
        linuxdvb_printf(10, "video info: the decoder reports no picture size\n");
        videoInfoPhase = VIDEO_INFO_DONE;
        return false;
    }

    videoInfoPhase = VIDEO_INFO_DONE;
    videoInfoReported = true;
    return true;
}

/*
 * A position request: for a track without a picture size or progressive flag
 * the video events are read and the periods go on, also when no more data is
 * written.
 */
static void LinuxDvbVideoInfoPoll(Context_t *context, unsigned long long pts)
{
    bool progressive = false, values = false, report = false, event = false;
    struct DVBApiVideoInfo_s known, now;
    int i;

    pthread_mutex_lock(&videoInfoMutex);
    if (!videoInfoWanted || videoInfoPhase == VIDEO_INFO_DONE)
    {
        pthread_mutex_unlock(&videoInfoMutex);
        return;
    }
    known = videoInfo;
    for (i = 0; i < 8 && LinuxDvbVideoEventPending() && LinuxDvbReadVideoEvent(&progressive, &values); i++)
    {
    }
    now = videoInfo;
    event = memcmp(&known, &now, sizeof(known)) != 0;
    if (!videoInfoNoSize && progressive)
    {
        /* the event has brought the flag a known picture was waiting for */
        videoInfoWanted = false;
        report = true;
    }
    else
    {
        report = LinuxDvbUpdateVideoInfo(event, &pts);
    }
    pthread_mutex_unlock(&videoInfoMutex);

    if (report)
    {
        context->manager->video->Command(context, MANAGER_UPDATED_TRACK_INFO, NULL);
    }
    if (values)
    {
        LinuxDvbSendVideoEvent(&now);
    }
}

static int Write(void  *_context, void *_out)
{
    Context_t          *context  = (Context_t  *) _context;
    AudioVideoOut_t    *out      = (AudioVideoOut_t*) _out;
    int32_t            ret       = cERR_LINUXDVB_NO_ERROR;
    int32_t            res       = 0;
    uint8_t            video     = 0;
    uint8_t            audio     = 0;
    Writer_t           *writer   = NULL;
    WriterAVCallData_t call;

    if (out == NULL)
    {
       linuxdvb_err("null pointer passed\n");
       return cERR_LINUXDVB_ERROR;
    }
    
    video = !strcmp("video", out->type);
    audio = !strcmp("audio", out->type);
  
    linuxdvb_printf(20, "DataLength=%u PrivateLength=%u Pts=%"PRIu64" FrameRate=%d\n", 
                                                    out->len, out->extralen, out->pts, out->frameRate);
    linuxdvb_printf(20, "v%d a%d\n", video, audio);

    if (video) 
    {
        char *Encoding = NULL;
        context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);

        linuxdvb_printf(20, "Encoding = %s\n", Encoding);

        writer = getWriter(Encoding);

        if (writer == NULL)
        {
            linuxdvb_printf(20, "searching default writer ... %s\n", Encoding);
            writer = getDefaultVideoWriter();
        }

        if (writer == NULL)
        {
            linuxdvb_err("unknown video codec and no default writer %s\n",Encoding);
            ret = cERR_LINUXDVB_ERROR;
        } 
        else
        {
            /* FFmpeg has no picture size: the decoder's values complete the track */
            bool complete = out->width == 0 || out->height == 0;
            bool progressive = false, values = false, report = false;
            struct DVBApiVideoInfo_s known, now;
            /* before the mutex: the driver may sleep in its poll, see there */
            bool pending = LinuxDvbVideoEventPending();

            pthread_mutex_lock(&videoInfoMutex);
            known = videoInfo;
            if (pending)
                LinuxDvbReadVideoEvent(&progressive, &values);
            now = videoInfo;
            /* a known picture still waits for the progressive flag */
            videoInfoNoSize = complete;
            videoInfoWanted = complete || videoInfo.progressive == -1;
            if (videoInfoWanted)
            {
                report = LinuxDvbUpdateVideoInfo(memcmp(&known, &now, sizeof(known)) != 0, NULL);
            }
            pthread_mutex_unlock(&videoInfoMutex);

            /*
             * A track without a picture size is reported once, after the
             * last event, also for a progressive event. A track with one is
             * reported for a progressive event, or once with the flag the
             * decoder shows when no event brings it.
             */
            if ((progressive && !complete) || report)
            {
                context->manager->video->Command(context, MANAGER_UPDATED_TRACK_INFO, NULL);
            }
            if (values)
            {
                LinuxDvbSendVideoEvent(&now);
            }

            call.fd           = videofd;
            call.data         = out->data;
            call.len          = out->len;
            call.Pts          = out->pts;
            call.Dts          = out->dts;
            call.private_data = out->extradata;
            call.private_size = out->extralen;
            call.FrameRate    = out->frameRate;
            call.FrameScale   = out->timeScale;
            call.Width        = out->width;
            call.Height       = out->height;
            call.InfoFlags    = out->infoFlags;
            call.Version      = 0;
            call.WriteV       = isBufferedOutput ? BufferingWriteV : writev_with_retry;

            if (writer->writeData)
            {
                res = writer->writeData(&call);
            }

            if (res < 0)
            {
                linuxdvb_err("failed to write data %d - %d\n", res, errno);
                linuxdvb_err("%s\n", strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
            else
            {
                PosBoundWritten(&videoPosBound, out->pts);
                if (res > 0 && call.Pts != 0 && call.Pts != INVALID_PTS_VALUE)
                {
                    videoPtsWritten = 1;
                }
            }
        }

        free(Encoding);
    }
    else if (audio)
    {
        char *Encoding = NULL;
        context->manager->audio->Command(context, MANAGER_GETENCODING, &Encoding);

        linuxdvb_printf(20, "Encoding = %s\n", Encoding);

        writer = getWriter(Encoding);

        if (writer == NULL)
        {
            linuxdvb_printf(20, "searching default writer ... %s\n", Encoding);
            writer = getDefaultAudioWriter();
        }

        if (writer == NULL)
        {
            linuxdvb_err("unknown audio codec %s and no default writer\n",Encoding);
            ret = cERR_LINUXDVB_ERROR;
        } 
        else
        {
            call.fd             = audiofd;
            call.data           = out->data;
            call.len            = out->len;
            call.Pts            = out->pts;
            call.Dts            = out->dts;
            call.private_data   = out->extradata;
            call.private_size   = out->extralen;
            call.FrameRate      = out->frameRate;
            call.FrameScale     = out->timeScale;
            call.InfoFlags      = out->infoFlags;
            call.Version        = 0;
            call.WriteV         = isBufferedOutput ? BufferingWriteV : writev_with_retry;

            if (dts_hd_pending && AUDIOTYPE_DTS == writer->caps->dvbStreamType)
                LinuxDvbDtsHdStart(context, out->data, out->len);

            if (writer->writeData)
            {
                res = writer->writeData(&call);
            }

            if (res >= 0)
            {
                PosBoundWritten(&audioPosBound, out->pts);
            }

            if (1 == eac3Restart && res >= 0 && AUDIOTYPE_AC3_PLUS == writer->caps->dvbStreamType)
            {
                /* the delay counts from the first DD+ packet */
                clock_gettime(CLOCK_MONOTONIC, &eac3PacketTime);
                __sync_bool_compare_and_swap(&eac3Restart, 1, 2);
            }

            if (res < 0)
            {
                linuxdvb_err("failed to write data %d - %d\n", res, errno);
                linuxdvb_err("%s\n", strerror(errno));
                ret = cERR_LINUXDVB_ERROR;
            }
        }

        free(Encoding);
    }

    if (audio && timing_after_switch)
    {
        LinuxDvbTiming("first-selected-audio");
        timing_after_switch = 0;
    }
    return ret;
}

/* Every command arrives once per type ("video", "audio"): reset the writer
 * of that type only. */
static int reset(Context_t  *context, char *type)
{
    int ret = cERR_LINUXDVB_NO_ERROR;
    Writer_t*   writer;
    char * Encoding = NULL;
    uint8_t video = !strcmp("video", type);

    if (video)
        context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
    else
        context->manager->audio->Command(context, MANAGER_GETENCODING, &Encoding);

    writer = getWriter(Encoding);

    if (writer == NULL)
    {
        linuxdvb_err("unknown %s codec %s\n", video ? "video" : "audio", Encoding);
        ret = cERR_LINUXDVB_ERROR;
    } else
    {
        if (writer->reset)
            writer->reset();
    }

    free(Encoding);

    if (isBufferedOutput)
        LinuxDvbBuffFlush(context);
    
    return ret;
}

static int Command(void  *_context, OutputCmd_t command, void * argument) {
    Context_t* context = (Context_t*) _context;
    int ret = cERR_LINUXDVB_NO_ERROR;
    
    linuxdvb_printf(50, "Command %d\n", command);

    switch(command) {
    case OUTPUT_OPEN: {
        ret = LinuxDvbOpen(context, (char*)argument);
        break;
    }
    case OUTPUT_CLOSE: {
        ret = LinuxDvbClose(context, (char*)argument);
        reset(context, (char*)argument);
        PosBoundReset((char*)argument);
        sCURRENT_PTS = 0;
        videoPtsWritten = 0;
        break;
    }
    case OUTPUT_PLAY: {	// 4
        PosBoundStart((char*)argument);
        sCURRENT_PTS = 0;
        videoPtsWritten = 0;
        ret = LinuxDvbPlay(context, (char*)argument);
        break;
    }
    case OUTPUT_STOP: {
        reset(context, (char*)argument);
        ret = LinuxDvbStop(context, (char*)argument);
        PosBoundReset((char*)argument);
        sCURRENT_PTS = 0;
        videoPtsWritten = 0;
        break;
    }
    case OUTPUT_FLUSH: {
        ret = LinuxDvbFlush(context, (char*)argument);
        reset(context, (char*)argument);
        PosBoundReset((char*)argument);
        sCURRENT_PTS = 0;
        videoPtsWritten = 0;
        break;
    }
    case OUTPUT_PAUSE: {
        ret = LinuxDvbPause(context, (char*)argument);
        break;
    }
    case OUTPUT_CONTINUE: {
        ret = LinuxDvbContinue(context, (char*)argument);
        break;
    }
    case OUTPUT_AVSYNC: {
        ret = LinuxDvbAVSync(context, (char*)argument);
        break;
    }
    case OUTPUT_CLEAR: {
        reset(context, (char*)argument);
        ret = LinuxDvbClear(context, (char*)argument);
        reset(context, (char*)argument);
        PosBoundReset((char*)argument);
        sCURRENT_PTS = 0;
        videoPtsWritten = 0;
        break;
    }
    case OUTPUT_PTS: {
        unsigned long long int pts = 0;
        ret = LinuxDvbPts(context, &pts);
        *((unsigned long long int*)argument) = (unsigned long long int)pts;
        break;
    }
    case OUTPUT_SWITCH: {
        /* a switch is the restart the DD+ start was waiting for */
        if (!strcmp("audio", (char*)argument))
            eac3Restart = 0;
        ret = LinuxDvbSwitch(context, (char*)argument);
        /* the decoder starts again with the data written after the switch */
        PosBoundRestart((char*)argument);
        break;
    }
    case OUTPUT_SLOWMOTION: {
        return LinuxDvbSlowMotion(context, (char*)argument);
    }
    case OUTPUT_AUDIOMUTE: {
        return LinuxDvbAudioMute(context, (char*)argument);
    }
    case OUTPUT_GET_FRAME_COUNT: {
        unsigned long long int frameCount = 0;
        ret = LinuxDvbGetFrameCount(context, &frameCount);
        *((unsigned long long int*)argument) = (unsigned long long int)frameCount;
        break;
    }
    case OUTPUT_GET_PROGRESSIVE: {
        int progressive;
        ret = cERR_LINUXDVB_NO_ERROR;
        pthread_mutex_lock(&videoInfoMutex);
        progressive = videoInfo.progressive;
        /* no event brought it: the flag the decoder showed once it had settled */
        if (progressive == -1 && videoInfoReported)
            progressive = videoInfoAsked.progressive;
        pthread_mutex_unlock(&videoInfoMutex);
        *((int*)argument) = progressive;
        break;
    }
    case OUTPUT_GET_VIDEO_INFO: {
        TrackDescription_t *track = (TrackDescription_t*)argument;
        ret = cERR_LINUXDVB_NO_ERROR;
        pthread_mutex_lock(&videoInfoMutex);
        if ((track->width <= 0 || track->height <= 0) && videoInfoReported)
        {
            /* FFmpeg did not see a picture: its frame rate is a guess too */
            if (videoInfo.width > 0 && videoInfo.height > 0)
            {
                track->width  = videoInfo.width;
                track->height = videoInfo.height;
            }
            else if (videoInfoAsked.width > 0 && videoInfoAsked.height > 0)
            {
                track->width  = videoInfoAsked.width;
                track->height = videoInfoAsked.height;
            }
            if (videoInfo.frame_rate > 0)
                track->frame_rate = videoInfo.frame_rate;
            else if (videoInfoAsked.frame_rate > 0)
                track->frame_rate = videoInfoAsked.frame_rate;
            if (track->progressive == -1)
                track->progressive = videoInfoAsked.progressive;
        }
        pthread_mutex_unlock(&videoInfoMutex);
        break;
    }
    case OUTPUT_SET_BUFFER_SIZE: {
        ret = cERR_LINUXDVB_ERROR;
        if (!isBufferedOutput)
        {
            uint32_t bufferSize = *((uint32_t*)argument);
            ret = cERR_LINUXDVB_NO_ERROR;
            if (bufferSize > 0)
            {
                LinuxDvbBuffSetSize(bufferSize);
                isBufferedOutput = true;
            }
        }
        break;
    }
    case OUTPUT_GET_BUFFER_SIZE: {
        ret = cERR_LINUXDVB_NO_ERROR;
        *((uint32_t*)argument) = LinuxDvbBuffGetSize();
        break;
    }
    case OUTPUT_AUDIO_CHANNEL: {
        int channel = *((int*)argument);
        if (channel < OUTPUT_AUDIO_CHANNEL_LEFT || channel > OUTPUT_AUDIO_CHANNEL_RIGHT)
        {
            ret = cERR_LINUXDVB_ERROR;
            break;
        }
        getLinuxDVBMutex();
        /* kept also when the driver refuses it: the decoder starts try again */
        audioChannel = channel;
        /* before the audio device is open the start of the decoder sets it */
        ret = audiofd != -1 ? LinuxDvbSetAudioChannel(channel) : cERR_LINUXDVB_NO_ERROR;
        releaseLinuxDVBMutex();
        break;
    }
    default:
        linuxdvb_err("ContainerCmd %d not supported!\n", command);
        ret = cERR_LINUXDVB_ERROR;
        break;
    }

    linuxdvb_printf(50, "exiting with value %d\n", ret);

    return ret;
}

static char *LinuxDvbCapabilities[] = { "audio", "video", NULL };

struct Output_s LinuxDvbOutput = {
    "LinuxDvb",
    &Command,
    &Write,
    LinuxDvbCapabilities
};
