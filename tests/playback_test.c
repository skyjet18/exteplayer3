#include <assert.h>
#include "../playback/playback.c"

static int64_t position, captured;
static int selected, clears, switches, seek_error;
static int sync_error, plays;
static int clear_error, seek_calls, dash_seek;
static void *stamp_before_clear;

static int manager_command(void *opaque, ManagerCmd_t command, void *argument)
{
    if (command == MANAGER_GET)
        *(int32_t *)argument = selected;
    else if (command == MANAGER_SET)
        selected = *(int32_t *)argument;
    return 0;
}

static int output_command(void *opaque, OutputCmd_t command, void *argument)
{
    Context_t *context = opaque;
    if (command == OUTPUT_PTS)
        *(int64_t *)argument = position;
    else if (command == OUTPUT_CLEAR || command == OUTPUT_SWITCH) {
        assert(context->playback->isSeeking || context->playback->isAudioSwitching);
        if (!context->playback->isDash)
            position = 0;
        if (command == OUTPUT_CLEAR)
        {
            if (context->playback->isDash)
            {
                assert(context->playback->isSeeking && !context->playback->isAudioSwitching);
                assert(argument == NULL && context->playback->stamp != stamp_before_clear);
            }
            clears++;
            return clear_error;
        }
        else
            switches++;
    }
    else if (command == OUTPUT_AVSYNC)
        return sync_error;
    else if (command == OUTPUT_PLAY)
        plays++;
    return 0;
}

static int container_command(void *opaque, ContainerCmd_t command, void *argument)
{
    Context_t *context = opaque;
    int64_t zero = 0;
    assert(context->playback->isSeeking || context->playback->isAudioSwitching);
    if (command == CONTAINER_SEEK) {
        seek_calls++;
        captured = position;
        return seek_error;
    }
    /* live DASH switches without a seek, static DASH like the rest */
    if (command == CONTAINER_SWITCH_AUDIO)
        return context->playback->isDash && !dash_seek ? 0 : PlaybackSeek(context, &zero, 0);
    return 0;
}

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* waits up to 5 s for a change of the play state after waited_serial,
 * stores when the wait ended */
static uint32_t waited_serial;
static void *wait_change(void *arg)
{
    PlaybackWaitChange(waited_serial, 5000);
    *(int64_t *)arg = mono_us();
    return NULL;
}

int main(void)
{
    PlaybackHandler_t playback = {.isPlaying = 1};
    Output_t audio = {.Command = output_command};
    OutputHandler_t output = {.Command = output_command, .audio = &audio};
    Manager_t audio_manager = {.Command = manager_command};
    ManagerHandler_t manager = {.audio = &audio_manager};
    Container_t container = {.Command = container_command};
    ContainerHandler_t containers = {.selectedContainer = &container};
    Context_t context = {.playback = &playback, .output = &output,
                         .manager = &manager, .container = &containers};
    int64_t zero = 0;
    int32_t track = 1;

    position = 2013750; /* 22.375 seconds */
    assert(!PlaybackSeek(&context, &zero, 0));
    assert(captured == 2013750 && clears == 1 && !playback.isSeeking);
    position = 2913750;
    assert(!PlaybackSwitchAudio(&context, &track));
    assert(captured == 2913750 && clears == 2 && switches == 1);
    assert(!playback.isSeeking);
    position = 3813750;
    seek_error = -1;
    assert(PlaybackSeek(&context, &zero, 0) < 0);
    assert(position == 3813750 && clears == 2 && !playback.isSeeking);
    track = 0;
    assert(PlaybackSwitchAudio(&context, &track) < 0);
    assert(position == 3813750 && switches == 1 && selected == 1 && !playback.isSeeking);
    playback.isDash = 1;
    stamp_before_clear = playback.stamp;
    assert(!PlaybackSwitchAudio(&context, &track));
    assert(position == 3813750 && clears == 3 && switches == 2);
    assert(!playback.isSeeking && !playback.isAudioSwitching);
    track = 1;
    stamp_before_clear = playback.stamp;
    {
        int old_seek_calls = seek_calls;
        assert(!PlaybackSwitchAudio(&context, &track));
        assert(seek_calls == old_seek_calls && clears == 4 && switches == 3);
        assert(!playback.isSeeking && !playback.isAudioSwitching);
        stamp_before_clear = playback.stamp;
        assert(!PlaybackSwitchAudio(&context, &track)); /* Same track: no reset. */
        assert(clears == 4 && switches == 3 && playback.stamp == stamp_before_clear);
        track = 0;
        clear_error = -1;
        assert(PlaybackSwitchAudio(&context, &track) < 0);
        assert(clears == 5 && switches == 3 && seek_calls == old_seek_calls);
        assert(!playback.isSeeking && !playback.isAudioSwitching);
        clear_error = 0;
        track = 1;
        stamp_before_clear = playback.stamp;
        assert(!PlaybackSwitchAudio(&context, &track));
        assert(clears == 6 && switches == 4 && seek_calls == old_seek_calls);
        /* static DASH: the seek of the switch clears from the playback
         * position, no second clear after it */
        dash_seek = 1;
        seek_error = 0;
        track = 0;
        position = 4713750;
        stamp_before_clear = playback.stamp;
        assert(!PlaybackSwitchAudio(&context, &track));
        assert(seek_calls == old_seek_calls + 1 && captured == 4713750);
        assert(clears == 7 && switches == 5 && selected == 0);
        assert(!playback.isSeeking && !playback.isAudioSwitching);
        dash_seek = 0;
    }
    playback.isPlaying = 0;
    sync_error = -1;
    assert(PlaybackPlay(&context) < 0 && plays == 0 && !playback.isPlaying);
    /* stop and terminate without a playback handler: checked before it is
     * used, also in the error message */
    {
        Context_t empty = {0};
        PLAYBACK_SILENT = 0;
        assert(PlaybackStop(&empty) < 0);
        assert(PlaybackTerminate(&empty) == 0);
        PLAYBACK_SILENT = 1;
    }
    /* a change of the play state (here a seek) wakes a thread that waits for
     * one; without a change the wait ends at its timeout, after a change since
     * the serial was taken it does not wait */
    {
        pthread_t waiter;
        int64_t start, woken = 0;
        uint32_t serial = PlaybackChangeSerial();
        start = mono_us();
        PlaybackWaitChange(serial, 100);
        assert(mono_us() - start >= 90000);
        waited_serial = serial;
        assert(!pthread_create(&waiter, NULL, wait_change, &woken));
        usleep(200000);
        playback.isPlaying = 1;
        playback.isDash = 0;
        seek_error = 0;
        start = mono_us();
        assert(!PlaybackSeek(&context, &zero, 0));
        assert(!pthread_join(waiter, NULL));
        assert(woken >= start && woken - start < 1000000);
        start = mono_us();
        PlaybackWaitChange(serial, 5000);
        assert(mono_us() - start < 1000000);
    }
    return 0;
}
