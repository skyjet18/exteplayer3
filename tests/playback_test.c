#include <assert.h>
#include "../playback/playback.c"

static int64_t position, captured;
static int selected, clears, switches, seek_error;

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
        assert(context->playback->isSeeking);
        position = 0;
        if (command == OUTPUT_CLEAR)
            clears++;
        else
            switches++;
    }
    return 0;
}

static int container_command(void *opaque, ContainerCmd_t command, void *argument)
{
    Context_t *context = opaque;
    int64_t zero = 0;
    assert(context->playback->isSeeking);
    if (command == CONTAINER_SEEK) {
        captured = position;
        return seek_error;
    }
    if (command == CONTAINER_SWITCH_AUDIO)
        return PlaybackSeek(context, &zero, 0);
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
    return 0;
}
