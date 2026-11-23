#ifndef EXTEPLAYER_PTS_CLOCK_H
#define EXTEPLAYER_PTS_CLOCK_H

#include <stdint.h>
#include <string.h>

#define PTS_CLOCK_INVALID INT64_MIN
#define PTS_CLOCK_WRAP (INT64_C(1) << 33)
#define PTS_CLOCK_JUMP (INT64_C(3) * 90000)
#define PTS_CLOCK_FRESH_US 700000
/* A jump in a stream whose packets came up to this long before continues
 * from its own timestamps: the track kept running (see pts_clock_update()). */
#define PTS_CLOCK_STALL_US 2000000
/* A track whose first timestamp is further than this from the other track
 * does not count from the same origin. Sixty seconds is where the demuxer
 * draws that line: libavformat takes a timestamp more than 60 s before the
 * first one of its program for one of the next 33-bit round. */
#define PTS_CLOCK_EPOCH (INT64_C(60) * 90000)

typedef struct {
    const void *stream;
    int64_t offset;
} PtsOffset;

typedef struct {
    PtsOffset cache[4];
    unsigned int next;
    int ready, trusted;
    int64_t raw, corrected, time_us, offset, step;
    int64_t first; /* corrected timestamp the track started with */
} PtsTrackClock;

typedef struct {
    PtsTrackClock track[2];
    int base_ready, delta_ready, delta_locked, stable;
    int preserve_timeline;
    int64_t base, delta, delta_min, delta_max;
} PtsClock;

static inline int64_t pts_clock_abs(int64_t value)
{
    return value < 0 ? -value : value;
}

static inline int pts_clock_video_gap(int64_t now, int64_t last, int64_t resumed, int paused)
{
    return !paused && last > 0 && now > last &&
           now - last > INT64_C(15) * 1000000 && resumed <= last;
}

static inline int64_t pts_clock_difference(int64_t a, int64_t b)
{
    return (int64_t)(((uint64_t)a - (uint64_t)b + PTS_CLOCK_WRAP / 2) &
                     (PTS_CLOCK_WRAP - 1)) - PTS_CLOCK_WRAP / 2;
}

/* A 33-bit DTS after the last one (none yet when last < 0), also across the
 * wrap of the PES field after 26.5 hours. */
static inline int pts_clock_after(int64_t dts, int64_t last)
{
    return last < 0 || pts_clock_difference(dts, last) > 0;
}

static inline void pts_clock_reset(PtsClock *clock, int keep_base)
{
    int ready = keep_base && clock->base_ready;
    int preserve_timeline = clock->preserve_timeline;
    int64_t base = clock->base;
    memset(clock, 0, sizeof(*clock));
    clock->base_ready = ready;
    clock->base = base;
    clock->preserve_timeline = preserve_timeline;
}

/* raw is a 90-kHz timestamp, not a PTS already shortened to a PES field. */
static inline int64_t pts_clock_update(PtsClock *clock, int kind,
                                       const void *stream, int64_t raw, int64_t now)
{
    PtsTrackClock *self = &clock->track[kind];
    PtsTrackClock *other = &clock->track[!kind];
    PtsOffset *entry = NULL;
    int64_t difference = 0, corrected;
    int fresh = other->ready && now >= other->time_us &&
                now - other->time_us < PTS_CLOCK_FRESH_US;
    int changed = 0, new_entry = 0;
    unsigned int i;

    if (raw == PTS_CLOCK_INVALID)
        return PTS_CLOCK_INVALID;
    if (!clock->base_ready)
    {
        clock->base = raw;
        clock->base_ready = 1;
    }
    for (i = 0; i < 4; i++)
        if (self->cache[i].stream == stream)
            entry = &self->cache[i];
    if (self->ready)
    {
        difference = pts_clock_difference(raw, self->raw);
        raw = self->raw + difference;
        changed = pts_clock_abs(difference) > PTS_CLOCK_JUMP;
    }
    if (!entry)
    {
        new_entry = 1;
        entry = &self->cache[self->next++ % 4];
        entry->stream = stream;
        if (clock->preserve_timeline)
            entry->offset = -clock->base;
        else
            entry->offset = self->ready ? self->offset : -clock->base;
    }
    if (!self->ready && other->ready && !clock->preserve_timeline)
    {
        /* The first timestamp of this track while the other one runs. */
        int64_t apart = pts_clock_difference(raw, other->raw);
        if (pts_clock_abs(apart) > PTS_CLOCK_EPOCH)
            /* Another origin: this distance is no A/V offset, nothing could
             * play it. The track starts where the other one started. */
            entry->offset = other->first - raw;
        else
            /* The same origin; whole 33-bit rounds between them are dropped. */
            entry->offset -= raw - other->raw - apart;
    }
    if (self->ready && pts_clock_abs(raw + entry->offset - self->corrected) > PTS_CLOCK_JUMP)
        changed = 1;
    if (clock->preserve_timeline)
        changed = 0;
    if (changed)
    {
        int64_t target = self->corrected + self->step;
        /* The same stream jumps while its packets keep coming (a new origin
         * after an ad break with separate audio and video renditions, whose
         * segments are read a second apart): it continues from its own
         * timestamps, where the other track may not have got to yet. A new
         * stream or one that stopped is anchored to the other track. */
        int running = !new_entry && now >= self->time_us &&
                      now - self->time_us < PTS_CLOCK_STALL_US;
        if (!running)
        {
            /* Never cross-anchor either track's very first timestamp. */
            self->trusted = fresh && clock->delta_ready;
            if (self->trusted)
                target = other->corrected + (kind ? clock->delta : -clock->delta);
        }
        entry->offset = target - raw;
        clock->stable = 0;
    }
    else if (!self->ready)
        self->trusted = 1;
    corrected = raw + entry->offset;
    if (self->ready && difference > 0 && difference < 9000)
        self->step = difference;
    self->raw = raw;
    self->corrected = corrected;
    self->offset = entry->offset;
    if (!self->ready)
        self->first = corrected;
    self->ready = 1;
    self->time_us = now;
    if (!changed && fresh && self->trusted && other->trusted && !clock->delta_locked)
    {
        int64_t delta = clock->track[1].corrected - clock->track[0].corrected;
        if (pts_clock_abs(delta) <= 45000)
        {
            if (!clock->delta_ready)
            {
                clock->delta = delta;
                clock->delta_ready = 1;
            }
            else
                clock->delta += (delta - clock->delta) / 32;
            if (!clock->stable)
                clock->delta_min = clock->delta_max = delta;
            if (delta < clock->delta_min)
                clock->delta_min = delta;
            if (delta > clock->delta_max)
                clock->delta_max = delta;
            if (clock->delta_max - clock->delta_min > 18000)
                clock->stable = 0;
            else if (++clock->stable >= 100)
                clock->delta_locked = 1;
        }
    }
    return corrected;
}

/* The first video keyframe may lie a little before the start of the timeline
 * (raw is a 90-kHz timestamp that the clock maps below 0): over HTTP the
 * stream analysis is cut short, FFmpeg can then take the start time of the
 * container from an audio packet that comes first (2 ms) while the video
 * starts at 0. Dropped, that keyframe leaves the decoder with frames that
 * depend on it: no picture until the next keyframe, and on decoders that
 * need the parameter sets of the keyframe none until a seek. Up to one second
 * before the start the keyframe is played at 0 instead (the caller decides
 * that it is the first video packet and a keyframe). */
#define PTS_CLOCK_EARLY_KEY 90000
static inline int pts_clock_early_key(const PtsClock *clock, int64_t raw)
{
    int64_t before;
    if (!clock->base_ready || raw == PTS_CLOCK_INVALID)
        return 0;
    before = pts_clock_difference(clock->base, raw);
    return before > 0 && before <= PTS_CLOCK_EARLY_KEY;
}

static inline int64_t pts_clock_map(const PtsClock *clock, int kind, int64_t raw)
{
    const PtsTrackClock *track = &clock->track[kind];
    if (raw == PTS_CLOCK_INVALID)
        return PTS_CLOCK_INVALID;
    if (!track->ready)
        return raw - clock->base;
    return track->raw + pts_clock_difference(raw, track->raw) + track->offset;
}

#endif
