#ifndef EXTEPLAYER_PTS_CLOCK_H
#define EXTEPLAYER_PTS_CLOCK_H

#include <stdint.h>
#include <string.h>

#define PTS_CLOCK_INVALID INT64_MIN
#define PTS_CLOCK_WRAP (INT64_C(1) << 33)
#define PTS_CLOCK_JUMP (INT64_C(3) * 90000)
#define PTS_CLOCK_FRESH_US 700000

typedef struct {
    const void *stream;
    int64_t offset;
} PtsOffset;

typedef struct {
    PtsOffset cache[4];
    unsigned int next;
    int ready, trusted;
    int64_t raw, corrected, time_us, offset, step;
} PtsTrackClock;

typedef struct {
    PtsTrackClock track[2];
    int base_ready, delta_ready, delta_locked, stable;
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

static inline void pts_clock_reset(PtsClock *clock, int keep_base)
{
    int ready = keep_base && clock->base_ready;
    int64_t base = clock->base;
    memset(clock, 0, sizeof(*clock));
    clock->base_ready = ready;
    clock->base = base;
}

/* raw is a 90-kHz timestamp, not a PTS already shortened to a PES field. */
static inline int64_t pts_clock_update(PtsClock *clock, int kind,
                                       const void *stream, int64_t raw, int64_t now)
{
    PtsTrackClock *self = &clock->track[kind];
    PtsTrackClock *other = &clock->track[!kind];
    PtsOffset *entry = NULL;
    int64_t difference = 0, corrected;
    int self_fresh = self->ready && now >= self->time_us &&
                     now - self->time_us < PTS_CLOCK_FRESH_US;
    int fresh = other->ready && now >= other->time_us &&
                now - other->time_us < PTS_CLOCK_FRESH_US;
    int changed = 0;
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
        entry = &self->cache[self->next++ % 4];
        entry->stream = stream;
        if (kind && self_fresh)
            entry->offset = self->corrected - raw;
        else if (self->ready && fresh && clock->delta_ready && other->trusted)
            entry->offset = other->corrected + (kind ? clock->delta : -clock->delta) - raw;
        else
            entry->offset = self->ready ? self->offset : -clock->base;
    }
    if (self->ready && pts_clock_abs(raw + entry->offset - self->corrected) > PTS_CLOCK_JUMP)
        changed = 1;
    if (changed)
    {
        int64_t target = self->corrected + self->step;
        /* Never cross-anchor either track's very first timestamp. */
        self->trusted = fresh && clock->delta_ready;
        if (self->trusted)
            target = other->corrected + (kind ? clock->delta : -clock->delta);
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
