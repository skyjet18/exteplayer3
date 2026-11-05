#include <assert.h>
#include "pts_clock.h"

int main(void)
{
    PtsClock clock = {0};
    int video, audio, replacement;
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
    /* A new stream and clock reset inherit the established AV relationship. */
    video_pts = pts_clock_update(&clock, 0, &replacement, 0, 6000000);
    assert(video_pts == audio_pts - clock.delta);
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
    return 0;
}
