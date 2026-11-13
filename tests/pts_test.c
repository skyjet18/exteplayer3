#include <assert.h>
#include "pts_clock.h"

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
    return 0;
}
