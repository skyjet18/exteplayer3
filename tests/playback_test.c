#include <assert.h>
#include "../playback/playback.c"

static int64_t position, captured;
static int selected, clears, switches, seek_error;
static int sync_error, plays;
static int clear_error, seek_calls;
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
    if (command == CONTAINER_SWITCH_AUDIO)
        return context->playback->isDash ? 0 : PlaybackSeek(context, &zero, 0);
    return 0;
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
    }
    playback.isPlaying = 0;
    sync_error = -1;
    assert(PlaybackPlay(&context) < 0 && plays == 0 && !playback.isPlaying);
    return 0;
}
