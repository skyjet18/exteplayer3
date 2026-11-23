#include <assert.h>
#include "pts_clock.h"

/* Tracks that do not count from the same origin. */

#define SECONDS(n) (INT64_C(90000) * (n))
#define HOURS(n) SECONDS(INT64_C(3600) * (n))
/* Up to this distance the first timestamps of two tracks are an A/V offset. */
#define EPOCH_LIMIT SECONDS(60)

static int epoch_video, epoch_audio, epoch_second;

/* 25 video and audio timestamps a second, the audio just after the video. */
static void epoch_run(PtsClock *clock, int64_t video, int64_t audio, int64_t now,
                      int64_t video_out, int64_t audio_out, int count)
{
    int i;
    for (i = 1; i <= count; i++)
    {
        assert(pts_clock_update(clock, 0, &epoch_video, video + i * 3600,
                                now + i * 40000) == video_out + i * 3600);
        assert(pts_clock_update(clock, 1, &epoch_audio, audio + i * 3600,
                                now + i * 40000 + 1) == audio_out + i * 3600);
    }
}

/* Video counts from 21.6 h and the audio is stamped 108 s earlier. The
 * demuxer takes such a timestamp for one of the next 33-bit round, so the
 * audio arrives 26.48 h after the video. */
static void epoch_far_apart(void)
{
    PtsClock clock = {0};
    const int64_t video = SECONDS(77760), audio = video - SECONDS(108) + PTS_CLOCK_WRAP;

    assert(pts_clock_update(&clock, 0, &epoch_video, video, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, audio, 2) == 0);
    epoch_run(&clock, video, audio, 2, 0, 0, 150);
    assert(clock.delta_locked && clock.delta <= 0 && clock.delta >= -3600);
    /* A second rendition in the origin of the audio, 30 ms after its last sample. */
    assert(pts_clock_update(&clock, 1, &epoch_second, audio + 150 * 3600 + 2700,
                            6000004) == 150 * 3600 + 2700);
    /* A seek: the video keeps the base, the audio starts with it again. */
    pts_clock_reset(&clock, 1);
    assert(pts_clock_update(&clock, 0, &epoch_video, video + SECONDS(600), 7000000) == SECONDS(600));
    assert(pts_clock_update(&clock, 1, &epoch_audio, audio + SECONDS(600), 7000001) == SECONDS(600));
    epoch_run(&clock, video + SECONDS(600), audio + SECONDS(600), 7000001,
              SECONDS(600), SECONDS(600), 10);

    /* The audio is delivered first. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, audio, 1) == 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, video, 2) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, audio + 1920, 3) == 1920);
    assert(pts_clock_update(&clock, 0, &epoch_video, video + 3600, 4) == 3600);

    /* Hours apart without a 33-bit round between them, later and earlier. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(8), 2) == 0);
    epoch_run(&clock, HOURS(1), HOURS(8), 2, 0, 0, 10);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(8), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(3), 2) == 0);
    epoch_run(&clock, HOURS(8), HOURS(3), 2, 0, 0, 10);

    /* The video is some frames on when the first audio comes: the audio
     * starts where the video started, not where it is now. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1) + 3600, 40001) == 3600);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1) + 7200, 80001) == 7200);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(8), 80002) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(8) + 1920, 80003) == 1920);

    /* Just over the limit. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, 900000, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 900000 + EPOCH_LIMIT + 1, 2) == 0);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) - EPOCH_LIMIT - 1, 2) == 0);
}

/* A/V offsets of the source are kept as they are. */
static void epoch_real_offsets(void)
{
    static const int64_t offsets[] = {
        18000, SECONDS(3), SECONDS(20), EPOCH_LIMIT, -18000, -SECONDS(20), -EPOCH_LIMIT
    };
    PtsClock clock = {0};
    unsigned int i;

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    {
        pts_clock_reset(&clock, 0);
        assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
        assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + offsets[i], 2) == offsets[i]);
        epoch_run(&clock, HOURS(1), HOURS(1) + offsets[i], 2, 0, offsets[i], 10);
        /* The audio first. */
        pts_clock_reset(&clock, 0);
        assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1), 1) == 0);
        assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1) + offsets[i], 2) == offsets[i]);
    }

    /* The distance counts from where the other track is, not from where it
     * started: audio that begins 100 s into the video and 20 s ahead of it. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    for (i = 1; i <= 50; i++)
        assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1) + SECONDS(2) * i,
                                1 + i * 2000000) == SECONDS(2) * i);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + SECONDS(120),
                            100000002) == SECONDS(120));

    /* A second audio rendition that joins later, 2 s after the last sample
     * of the first one, keeps these 2 s. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + 18000, 2) == 18000);
    epoch_run(&clock, HOURS(1), HOURS(1) + 18000, 2, 0, 18000, 150);
    assert(pts_clock_update(&clock, 1, &epoch_second, HOURS(1) + 18000 + 150 * 3600 + SECONDS(2),
                            6000004) == 18000 + 150 * 3600 + SECONDS(2));
    /* One from another origin is no first timestamp of its track: it is a
     * jump, anchored to the video with the A/V offset learned so far. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + 18000, 2) == 18000);
    epoch_run(&clock, HOURS(1), HOURS(1) + 18000, 2, 0, 18000, 150);
    assert(clock.delta_locked);
    assert(pts_clock_update(&clock, 1, &epoch_second, HOURS(9), 6000004) ==
           clock.track[0].corrected + clock.delta);
}

/* The 33-bit round is no other origin. */
static void epoch_wrap(void)
{
    PtsClock clock = {0};

    /* The audio has passed the end of the round, the video has not. */
    assert(pts_clock_update(&clock, 0, &epoch_video, PTS_CLOCK_WRAP - 100, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, PTS_CLOCK_WRAP + 100, 2) == 200);
    /* The same with timestamps that nobody has unwrapped. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, PTS_CLOCK_WRAP - 100, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 100, 2) == 200);
    assert(pts_clock_update(&clock, 0, &epoch_video, 3500, 3) == 3600);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 2020, 4) == 2120);
    /* One round and 20 s apart: an offset of 20 s. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + PTS_CLOCK_WRAP + SECONDS(20), 2) == SECONDS(20));
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(1) + PTS_CLOCK_WRAP, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(1) + SECONDS(20), 2) == SECONDS(20));
    /* Another origin, and the video passes the end of the round. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, PTS_CLOCK_WRAP - 100, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(5), 2) == 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, 3500, 3) == 3600);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(5) + 1920, 4) == 1920);
    /* The audio passes it. */
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(5), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, PTS_CLOCK_WRAP - 100, 2) == 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(5) + 3600, 3) == 3600);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 1820, 4) == 1920);
}

/* One track alone has nothing to be compared with. */
static void epoch_single_track(void)
{
    PtsClock clock = {0};

    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(26), 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, HOURS(26) + 1920, 2) == 1920);
    assert(pts_clock_update(&clock, 1, &epoch_second, HOURS(26) + 3840, 3) == 3840);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(26), 1) == 0);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(26) + 3600, 2) == 3600);
    /* After a seek the base is kept, also hours into the stream. */
    pts_clock_reset(&clock, 1);
    assert(pts_clock_update(&clock, 0, &epoch_video, HOURS(28), 3) == HOURS(2));
}

/* DASH keeps the timeline of the manifest whatever the distance is. */
static void epoch_preserve_timeline(void)
{
    PtsClock clock = {0};

    clock.preserve_timeline = 1;
    assert(pts_clock_update(&clock, 0, &epoch_video, 900000, 1) == 0);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 900000 + HOURS(8), 2) == HOURS(8));
    assert(pts_clock_update(&clock, 1, &epoch_second, 900000 + SECONDS(61), 3) == SECONDS(61));
    assert(pts_clock_update(&clock, 0, &epoch_video, 903600, 4) == 3600);
    pts_clock_reset(&clock, 1);
    assert(clock.preserve_timeline);
    assert(pts_clock_update(&clock, 1, &epoch_audio, 900000 + HOURS(8), 5) == HOURS(8));
    assert(pts_clock_update(&clock, 0, &epoch_video, 900000 + SECONDS(600), 6) == SECONDS(600));
}

/* Server-side ad insertion with separate renditions: audio and video jump
 * to a new origin at the same media time, but the audio gets there 1 s of
 * media earlier (its segments were read ahead after a late video segment).
 * The audio kept running: it continues from its own timestamps. Anchored to
 * where the video is at that moment, it would lag 1 s from then on. A track
 * whose packets stopped for 2 s or more is anchored to the other one. */
static void ssai_jump_while_running(void)
{
    static int video, audio;
    const int64_t base = 900000, later = HOURS(1);
    PtsClock clock = {0};
    int64_t now = 0, v = 0, a = 0;
    int i, k;

    /* in step, the A/V delta locks (between -1 frame and 0: it is measured
     * when either track has just been read) */
    for (i = 0; i < 200; i++)
    {
        now += 40000;
        assert(pts_clock_update(&clock, 0, &video, base + i * 3600, now) == i * 3600);
        assert(pts_clock_update(&clock, 1, &audio, base + i * 3600, now + 1) == i * 3600);
    }
    assert(clock.delta_locked && clock.delta <= 0 && clock.delta >= -3600);
    /* a video segment comes 1 s late: 1 s of audio alone */
    for (i = 200; i < 225; i++)
    {
        now += 40000;
        assert(pts_clock_update(&clock, 1, &audio, base + i * 3600, now) == i * 3600);
    }
    /* then the video from where it stopped, the audio 25 frames ahead; the
     * new origin starts at frame 240 for both */
    for (k = 0; k <= 70; k++)
    {
        int vf = 200 + k, af = 225 + k;
        now += 40000;
        v = pts_clock_update(&clock, 0, &video, (vf >= 240 ? later : 0) + base + vf * 3600, now);
        a = pts_clock_update(&clock, 1, &audio, (af >= 240 ? later : 0) + base + af * 3600, now + 1);
        assert(v == vf * 3600);
        assert(a == af * 3600);
    }
    /* the audio stops for 3 s and comes back from yet another origin: it
     * is anchored to the video with the learned delta */
    for (i = 0; i < 75; i++)
    {
        now += 40000;
        v = pts_clock_update(&clock, 0, &video, later + base + (271 + i) * 3600, now);
    }
    a = pts_clock_update(&clock, 1, &audio, HOURS(5), now + 1);
    assert(a == clock.track[0].corrected + clock.delta);
}

int main(void)
{
    PtsClock clock = {0};
    static int video, audio, replacement, replacement_audio;
    int64_t video_pts, audio_pts;
    int i;
    assert(!pts_clock_video_gap(15000001, 1, 0, 0));
    assert(pts_clock_video_gap(15000002, 1, 0, 0));
    assert(!pts_clock_video_gap(15000002, 1, 2, 0));
    assert(!pts_clock_video_gap(15000002, 1, 0, 1));
    assert(pts_clock_update(&clock, 0, &video, 900000, 1) == 0);
    assert(pts_clock_update(&clock, 1, &audio, 909000, 2) == 9000);
    assert(clock.delta_ready && clock.delta == 9000); /* Natural 100ms offset. */
    assert(pts_clock_update(&clock, 0, &video, PTS_CLOCK_INVALID, 3) == PTS_CLOCK_INVALID);
    assert(pts_clock_map(&clock, 0, 899000) == -1000); /* Preserve B-frame DTS. */
    for (i = 1; i < 150; i++)
    {
        video_pts = pts_clock_update(&clock, 0, &video, 900000 + i * 3600, i * 40000);
        audio_pts = pts_clock_update(&clock, 1, &audio, 909000 + i * 3600, i * 40000 + 1);
        assert(audio_pts - video_pts == 9000);
    }
    assert(clock.delta_locked);
    /* A newly selected audio stream starts 30 ms after the last delivered sample. */
    assert(pts_clock_update(&clock, 1, &replacement_audio,
                            audio_pts + clock.base + 2700, 5960002) == audio_pts + 2700);
    assert(pts_clock_update(&clock, 1, &audio,
                            audio_pts + clock.base + 3600, 5960003) == audio_pts + 3600);
    /* A new stream and clock reset inherit the established AV relationship. */
    video_pts = pts_clock_update(&clock, 0, &replacement, 0, 6000000);
    assert(video_pts == clock.track[1].corrected - clock.delta);
    /* A long freeze must not change a locked AV delta. */
    {
        int64_t delta = clock.delta;
        pts_clock_update(&clock, 0, &replacement, 100000000, 106000000);
        pts_clock_update(&clock, 1, &audio, 100009000, 106000001);
        assert(clock.delta == delta);
    }
    pts_clock_reset(&clock, 1);
    assert(pts_clock_update(&clock, 0, &video, 900000 + 90000 * 60, 110000000) == 90000 * 60);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &video, PTS_CLOCK_WRAP - 100, 1) == 0);
    assert(pts_clock_update(&clock, 0, &video, 100, 2) == 200);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &video, 0, 1) == 0);
    assert(pts_clock_update(&clock, 0, &video, PTS_CLOCK_JUMP, 2) == PTS_CLOCK_JUMP);
    assert(pts_clock_update(&clock, 0, &video, PTS_CLOCK_JUMP * 2 + 1, 3) == PTS_CLOCK_JUMP);
    assert(pts_clock_difference(10, PTS_CLOCK_WRAP - 10) == 20);
    /* The DTS of packets read again by a live TS (-v): after the last one
     * also behind the wrap of the PES field, none before the first. */
    assert(pts_clock_after(500, PTS_CLOCK_WRAP - 1000));
    assert(!pts_clock_after(PTS_CLOCK_WRAP - 1000, 500));
    assert(!pts_clock_after(500, 500) && !pts_clock_after(400, 500));
    assert(pts_clock_after(0, -1));
    pts_clock_reset(&clock, 0);
    clock.preserve_timeline = 1;
    assert(pts_clock_update(&clock, 0, &video, 900000, 1) == 0);
    assert(pts_clock_update(&clock, 1, &audio, 900000, 2) == 0);
    /* Alternate audio content is 500 ms newer, not the last delivered sample. */
    assert(pts_clock_update(&clock, 1, &replacement_audio, 945000, 3) == 45000);
    assert(pts_clock_update(&clock, 0, &video, 936000, 4) == 36000);
    assert(pts_clock_update(&clock, 1, &audio, 954000, 5) == 54000);
    /* A read-ahead gap must not manufacture an offset on either track. */
    assert(pts_clock_update(&clock, 1, &replacement_audio, 1350000, 6) == 450000);
    assert(pts_clock_update(&clock, 0, &video, 990000, 7) == 90000);
    assert(pts_clock_update(&clock, 1, &audio, 999000, 8) == 99000);
    assert(pts_clock_map(&clock, 0, 986400) == 86400);
    pts_clock_reset(&clock, 1);
    assert(clock.preserve_timeline);
    assert(pts_clock_update(&clock, 1, &replacement_audio, 1800000, 9000000) == 900000);
    pts_clock_reset(&clock, 0);
    assert(pts_clock_update(&clock, 0, &video, PTS_CLOCK_WRAP - 100, 1) == 0);
    assert(pts_clock_update(&clock, 1, &audio, PTS_CLOCK_WRAP - 80, 2) == 20);
    assert(pts_clock_update(&clock, 1, &replacement_audio, 100, 3) == 200);
    assert(pts_clock_update(&clock, 0, &video, 120, 4) == 220);
    /* The first video keyframe up to 1 s before the start of the timeline
     * (start time of the container from an audio packet). */
    pts_clock_reset(&clock, 0);
    assert(!pts_clock_early_key(&clock, 900000)); /* no start yet */
    clock.base = 900180; /* audio at 2 ms */
    clock.base_ready = 1;
    assert(pts_clock_early_key(&clock, 900000));
    assert(pts_clock_early_key(&clock, 900180 - 90000));
    assert(!pts_clock_early_key(&clock, 900180 - 90001)); /* more than 1 s */
    assert(!pts_clock_early_key(&clock, 900180)); /* not before the start */
    assert(!pts_clock_early_key(&clock, 903780));
    assert(!pts_clock_early_key(&clock, PTS_CLOCK_INVALID));
    /* across the wrap of the 33-bit timestamps */
    clock.base = 100;
    assert(pts_clock_early_key(&clock, PTS_CLOCK_WRAP - 80));
    assert(!pts_clock_early_key(&clock, PTS_CLOCK_WRAP - 90001));
    clock.base = PTS_CLOCK_WRAP - 10;
    assert(!pts_clock_early_key(&clock, 5)); /* after the start, behind the wrap */
    epoch_far_apart();
    epoch_real_offsets();
    epoch_wrap();
    epoch_single_track();
    epoch_preserve_timeline();
    ssai_jump_while_running();
    return 0;
}
