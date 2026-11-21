#include <assert.h>
#include <errno.h>
#include <pthread.h>

/* The container starts its threads through this. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument);
#define pthread_create test_pthread_create
#include "../container/container_ffmpeg.c"
#undef pthread_create

static int create_fails, threads_started, threads_ended;

/* A real thread that has run to its end when the caller goes on. */
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*start)(void *), void *argument)
{
    (void)attributes;
    if (create_fails)
        return EAGAIN;
    threads_started++;
    assert(!pthread_create(thread, NULL, start, argument));
    assert(!pthread_join(*thread, NULL));
    return 0;
}

/* The last call of the play thread but for clearing its flag. */
int8_t PlaybackDieNow(int8_t value)
{
    threads_ended += value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

int64_t PlaybackLastContinue(void)
{
    return 0;
}

uint32_t PlaybackChangeSerial(void)
{
    return 0;
}

void PlaybackWaitChange(uint32_t serial, int32_t timeoutMs)
{
    (void)serial;
    (void)timeoutMs;
}

void LinuxDvbBuffSetStamp(void *stamp)
{
    (void)stamp;
}

/* The play thread picks the live sync default of the box. */
stb_type_t GetSTBType(void)
{
    return STB_UNKNOWN;
}

static int32_t output_command(void *opaque, OutputCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == OUTPUT_GET_BUFFER_SIZE);
    *(uint32_t *)argument = 0;
    return 0;
}

static PlaybackHandler_t playback;
static OutputHandler_t output = {.Command = output_command};
static Context_t context = {.playback = &playback, .output = &output};

/* The playback is not playing any more: the thread ends before
 * container_ffmpeg_play() goes on behind pthread_create(). No thread is
 * left, so the stop has nothing to wait for and the next play starts one. */
static void thread_ends_at_once(void)
{
    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 1 && threads_ended == 1);
    assert(!hasPlayThreadStarted);

    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 2 && threads_ended == 2);
    assert(!hasPlayThreadStarted);

    isContainerRunning = 1;
    assert(!container_ffmpeg_stop(&context));
    assert(!isContainerRunning);
}

/* No thread could be created: none is reported, the next play starts one. */
static void thread_is_not_created(void)
{
    create_fails = 1;
    assert(container_ffmpeg_play(&context) == cERR_CONTAINER_FFMPEG_ERR);
    assert(threads_started == 2 && !hasPlayThreadStarted);
    create_fails = 0;
    assert(!container_ffmpeg_play(&context));
    assert(threads_started == 3 && threads_ended == 3 && !hasPlayThreadStarted);
}

/* Seeks from the decoder position: a decoder that has shown no frame of this
 * playback yet reports one of the playback before. A subtitle switch at the
 * start seeks 0 s from there; it went to the end of the earlier playback. */

static Context_t stale_context;
static Track_t stale_track;
static int64_t decoder_pts, stale_expected;
static int stale_step, stale_writes;

static int stale_playback(void *opaque, PlaybackCmd_t command, void *argument)
{
    (void)opaque;
    if (command == PLAYBACK_LENGTH)
        *(int64_t *)argument = 30;
    else {
        assert(command == PLAYBACK_PTS);
        *(int64_t *)argument = decoder_pts;
    }
    return 0;
}

static int no_track(void *opaque, ManagerCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = NULL;
    return 0;
}

static int stale_audio(void *opaque, ManagerCmd_t command, void *argument)
{
    (void)opaque;
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = &stale_track;
    return 0;
}

/* The seek of a track switch or of a key, from what the decoder reports. */
static void seek_from(int64_t position, int64_t seconds)
{
    decoder_pts = position;
    assert(!container_ffmpeg_seek(&stale_context, seconds, 0));
}

/* The audio output: the play thread writes here without the container lock. */
static int32_t stale_write(void *opaque, void *data)
{
    const AudioVideoOut_t *out = data;
    (void)opaque;
    /* The first packet behind a seek shows where it went. */
    if (++stale_writes == 1 && stale_step)
        assert(llabs(out->pts - stale_expected) < 90000);
    if (stale_writes < 4)
        return 0;
    stale_writes = 0;
    switch (stale_step++) {
    case 0:
        /* Four packets are written; the decoder reports 24 s of the playback before. */
        stale_expected = 0;
        seek_from(24 * 90000, 0);
        break;
    case 1:
        /* The decoder shows this packet: the seek stays where it is. */
        stale_expected = out->pts;
        seek_from(out->pts, 0);
        break;
    case 2:
        /* 24 s again, shown before that seek: a key seeks 5 s forward. */
        stale_expected += 5 * 90000;
        seek_from(24 * 90000, 5);
        break;
    default:
        stale_context.playback->isPlaying = 0;
    }
    return 0;
}

static void seek_from_an_earlier_playback(void)
{
    static const uint8_t header[44] = {
        'R', 'I', 'F', 'F', 0x24, 0x53, 7, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x80, 0x3e, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0x53, 7, 0};
    static uint8_t samples[480000]; /* 30 s of 8 kHz mono 16-bit silence */
    char name[] = "/tmp/exteplayer3-stale-XXXXXX";
    PlaybackHandler_t playing = {.Command = stale_playback, .isPlaying = 1};
    Manager_t none = {.Command = no_track}, audio = {.Command = stale_audio};
    ManagerHandler_t managers = {.audio = &audio, .video = &none, .subtitle = &none};
    Output_t audio_output = {.Write = stale_write}, subtitle_output = {0};
    OutputHandler_t outputs = {.Command = output_command, .audio = &audio_output,
                               .subtitle = &subtitle_output};
    int file = mkstemp(name);
    assert(file >= 0 && write(file, header, sizeof(header)) == sizeof(header));
    assert(write(file, samples, sizeof(samples)) == sizeof(samples) && !close(file));
    stale_context = (Context_t){.playback = &playing, .manager = &managers, .output = &outputs};
    assert(!container_ffmpeg_init_av_context(&stale_context, name, 0, NULL, 0, 0));
    stale_track.stream = avContextTab[0]->streams[0];
    stale_track.inject_raw_pcm = 1;
    FFMPEGThread(&stale_context);
    assert(stale_step == 4);
    if (use_custom_io[0]) {
        AVIOContext *pb = avContextTab[0]->pb;
        avformat_close_input(&avContextTab[0]);
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        fclose(custom_io_tab[0]->pFile);
        free(custom_io_tab[0]);
        custom_io_tab[0] = NULL;
        use_custom_io[0] = 0;
    } else
        avformat_close_input(&avContextTab[0]);
    unlink(name);
}

int main(void)
{
    thread_ends_at_once();
    thread_is_not_created();
    seek_from_an_earlier_playback();
    return 0;
}
