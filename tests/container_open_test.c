#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/lsan_interface.h>
#include <libavformat/avformat.h>

/* The container allocates through these: a countdown that reaches zero fails. */
static int malloc_fails, strdup_fails, av_malloc_fails, context_fails;
static void *av_block;

static int due(int *countdown)
{
    return *countdown && !--*countdown;
}

static void *test_malloc(size_t size)
{
    return due(&malloc_fails) ? NULL : malloc(size);
}

static char *test_strdup(const char *text)
{
    return due(&strdup_fails) ? NULL : strdup(text);
}

static void *test_av_malloc(size_t size)
{
    return due(&av_malloc_fails) ? NULL : av_malloc(size);
}

static void test_av_free(void *block)
{
    if (block == av_block)
        av_block = NULL;
    av_free(block);
}

static AVFormatContext *test_alloc_context(void)
{
    return due(&context_fails) ? NULL : avformat_alloc_context();
}

#define malloc test_malloc
#define strdup test_strdup
#define av_malloc test_av_malloc
#define av_free test_av_free
#define avformat_alloc_context test_alloc_context
#include "../container/container_ffmpeg.c"

static PlaybackHandler_t playback;
static Context_t context = {.playback = &playback};
static char rtmp[] = "rtmp://127.0.0.1/app/stream";
static char empty[] = "/tmp/exteplayer3-open-XXXXXX";
static char wav[] = "/tmp/exteplayer3-open-XXXXXX";
static char adts[] = "/tmp/exteplayer3-open-XXXXXX.aac";

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

static int32_t open_input(char *name)
{
    return container_ffmpeg_init_av_context(&context, name, 0, NULL, 0, 0);
}

/* What container_ffmpeg_stop() does for an input that reads through the custom I/O. */
static void close_input(void)
{
    AVIOContext *pb = avContextTab[0]->pb;
    avformat_close_input(&avContextTab[0]);
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    fclose(custom_io_tab[0]->pFile);
    free(custom_io_tab[0]);
    custom_io_tab[0] = NULL;
    use_custom_io[0] = 0;
}

static void rtmp_uri_allocation_fails(void)
{
    strdup_fails = 1;
    assert(open_input(rtmp) == cERR_CONTAINER_FFMPEG_NOMEM && !avContextTab[0]);
}

static void rtmp_parameter_allocation_fails(void)
{
    for (int parameter = 1; parameter <= 3; parameter++) {
        malloc_fails = parameter;
        assert(open_input(rtmp) == cERR_CONTAINER_FFMPEG_NOMEM && !avContextTab[0]);
    }
    assert(!__lsan_do_recoverable_leak_check());
}

static void format_context_allocation_fails(void)
{
    context_fails = 1;
    assert(open_input(wav) == cERR_CONTAINER_FFMPEG_NOMEM);
}

static void missing_file_leaves_no_context(void)
{
    char missing[] = "/tmp/exteplayer3-open-missing/file";
    assert(open_input(missing) == cERR_CONTAINER_FFMPEG_OPEN && !avContextTab[0]);
}

static void io_buffer_allocation_closes_the_file(void)
{
    CustomIOCtx_t io = {.szFile = wav};
    av_malloc_fails = 1;
    assert(!container_ffmpeg_get_avio_context(&io, 4096) && !io.pFile);
}

/* An empty file: no demuxer takes it, avformat_open_input() fails. */
static void unknown_format_releases_the_io(void)
{
    assert(open_input(empty) == cERR_CONTAINER_FFMPEG_OPEN && !avContextTab[0]);
    assert(!custom_io_tab[0] && !use_custom_io[0]);
}

static void unknown_format_frees_the_io_context(void)
{
    assert(open_input(empty) == cERR_CONTAINER_FFMPEG_OPEN);
    assert(!__lsan_do_recoverable_leak_check());
}

static void unknown_format_leaves_the_lock_alone(void)
{
    assert(open_input(empty) == cERR_CONTAINER_FFMPEG_OPEN);
    assert(!pthread_rwlock_trywrlock(&mutex));
    pthread_rwlock_unlock(&mutex);
}

static void opened_input_keeps_no_options(void)
{
    assert(!open_input(wav) && avContextTab[0]->nb_streams == 1);
    close_input();
    assert(!__lsan_do_recoverable_leak_check());
}

static void stop_closes_the_custom_io(void)
{
    assert(!open_input(wav));
    isContainerRunning = 1;
    assert(!container_ffmpeg_stop(&context));
    assert(!avContextTab[0] && !custom_io_tab[0]);
}

static void read_ahead_buffer_goes_back_to_av_free(void)
{
    ffmpeg_buf = av_block = av_malloc(16);
    assert(av_block);
    ffmpeg_buf_free();
    assert(!av_block && !ffmpeg_buf);
}

/* Software decoding: the WAV file is sent as A_IPCM, so its track gets a
 * decoder context from wrapped_avcodec_get_context(). */
static AVCodecContext *added_decoder;

/* The track update links these; the WAV track takes none of their paths. */
stb_type_t GetSTBType(void)
{
    return STB_UNKNOWN;
}

const char *GetGraphicSubPath(void)
{
    return NULL;
}

uint8_t get_chan_config(int channels)
{
    (void)channels;
    return 0;
}

int avpriv_mpeg4audio_get_config(MPEG4AudioConfig *c, const uint8_t *buf, int bit_size, int sync_extension)
{
    (void)c, (void)buf, (void)bit_size, (void)sync_extension;
    return -1;
}

int avpriv_copy_pce_data(PutBitContext *pb, GetBitContext *gb)
{
    (void)pb, (void)gb;
    return 0;
}

static int decoder_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    (void)opaque;
    if (command == MANAGER_ADD)
        added_decoder = ((Track_t *)argument)->avCodecCtx;
    else if (command == MANAGER_GET_TRACK || command == MANAGER_REF_LIST)
        *(Track_t **)argument = NULL;
    else if (command == MANAGER_REF_LIST_SIZE)
        *(int32_t *)argument = 0;
    return 0;
}

static Manager_t decoder_audio = {.Command = decoder_manager};
static ManagerHandler_t decoder_managers = {.audio = &decoder_audio};
static Context_t decoder_context = {.playback = &playback, .manager = &decoder_managers};

static AVCodecContext *open_decoder(void)
{
    terminating = 0; /* left set by the stop of the case before */
    added_decoder = NULL;
    assert(!container_ffmpeg_init_av_context(&decoder_context, wav, 0, NULL, 0, 0));
    assert(!container_ffmpeg_update_tracks(&decoder_context, wav, 1));
    assert(added_decoder && avcodec_is_open(added_decoder));
    return added_decoder;
}

static void stop_decoder(void)
{
    added_decoder = NULL;
    isContainerRunning = 1;
    assert(!container_ffmpeg_stop(&decoder_context));
}

/* A track list updates the tracks again: the track keeps its decoder. */
static void track_update_keeps_the_decoder(void)
{
    AVCodecContext *decoder = open_decoder();
    assert(!container_ffmpeg_update_tracks(&decoder_context, wav, 0));
    assert(added_decoder == decoder);
    stop_decoder();
}

/* The flush after a seek drops the frame the decoder holds. */
static void seek_flush_resets_the_decoder(void)
{
    AVCodecContext *decoder = open_decoder();
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    assert(packet && frame && !av_new_packet(packet, 64));
    memset(packet->data, 0, packet->size);
    assert(!avcodec_send_packet(decoder, packet));
    wrapped_avcodec_flush_buffers(0);
    assert(avcodec_receive_frame(decoder, frame) == AVERROR(EAGAIN));
    av_frame_free(&frame);
    av_packet_free(&packet);
    stop_decoder();
}

/* The decoders of all track updates are gone after the stop. */
static void stop_frees_the_decoders(void)
{
    open_decoder();
    assert(!container_ffmpeg_update_tracks(&decoder_context, wav, 0));
    stop_decoder();
    assert(!__lsan_do_recoverable_leak_check());
}

/* An audio manager whose track list is full: ManagerAdd() of manager/audio.c
 * refuses the 21st track and does not take the codec header of an AAC track. */
static int full_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    if (command == MANAGER_ADD)
        return -1;
    return decoder_manager(opaque, command, argument);
}

static Manager_t full_audio = {.Command = full_manager};
static ManagerHandler_t full_managers = {.audio = &full_audio};
static Context_t full_context = {.playback = &playback, .manager = &full_managers};

static void refused_audio_track_frees_its_codec_header(void)
{
    terminating = 0;
    assert(!container_ffmpeg_init_av_context(&full_context, adts, 0, NULL, 0, 0));
    assert(avContextTab[0]->nb_streams == 1 &&
           avContextTab[0]->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC);
    assert(!container_ffmpeg_update_tracks(&full_context, adts, 1));
    isContainerRunning = 1;
    assert(!container_ffmpeg_stop(&full_context));
    assert(!__lsan_do_recoverable_leak_check());
}

int main(void)
{
    static const uint8_t header[44] = {
        'R', 'I', 'F', 'F', 100, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x80, 0x3e, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 64, 0, 0, 0};
    uint8_t samples[64] = {0};
    int file = mkstemp(wav);
    assert(file >= 0 && write(file, header, sizeof(header)) == sizeof(header));
    assert(write(file, samples, sizeof(samples)) == sizeof(samples) && !close(file));
    /* ten ADTS frames, AAC LC mono 48 kHz: a single channel element with
     * no scale factor band, then the end element */
    static const uint8_t frame[11] = {0xff, 0xf1, 0x4c, 0x40, 0x01, 0x7f, 0xfc, 0x00, 0x00, 0x00, 0x07};
    file = mkstemps(adts, 4);
    assert(file >= 0);
    for (int i = 0; i < 10; i++)
        assert(write(file, frame, sizeof(frame)) == sizeof(frame));
    assert(!close(file));
    file = mkstemp(empty);
    assert(file >= 0 && !close(file));
    rtmp_proto_impl = 1; /* the native rtmp protocol, which takes its parameters from the uri */
    rtmp_uri_allocation_fails();
    rtmp_parameter_allocation_fails();
    format_context_allocation_fails();
    missing_file_leaves_no_context();
    io_buffer_allocation_closes_the_file();
    unknown_format_releases_the_io();
    unknown_format_frees_the_io_context();
    unknown_format_leaves_the_lock_alone();
    opened_input_keeps_no_options();
    stop_closes_the_custom_io();
    read_ahead_buffer_goes_back_to_av_free();
    track_update_keeps_the_decoder();
    seek_flush_resets_the_decoder();
    stop_frees_the_decoders();
    refused_audio_track_frees_its_codec_header();
    unlink(wav);
    unlink(adts);
    unlink(empty);
    return 0;
}
