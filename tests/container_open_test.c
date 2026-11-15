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
    unlink(wav);
    unlink(empty);
    return 0;
}
