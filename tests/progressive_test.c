#include <assert.h>
/* The container counts its waits here instead of sleeping. */
#define usleep progressive_test_usleep
#include "../container/container_ffmpeg.c"

static int stopped, source_reads;
static uint8_t target[64];

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stopped;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

/* A read callback of FFmpeg: an error at the current end, then the data. */
static int source(void *opaque, uint8_t *data, int size)
{
    (void)opaque;
    if (!source_reads++)
        return AVERROR_EOF;
    assert(data == target && size == sizeof(target));
    memset(data, 7, size);
    return size;
}

static void data_after_a_failed_first_read(void)
{
    assert(ffmpeg_read_wrapper_base(NULL, target, sizeof(target), 0) == sizeof(target));
    assert(source_reads == 2 && target[0] == 7 && target[sizeof(target) - 1] == 7);
}

static void end_is_reported_as_eof(void)
{
    stopped = 1;
    assert(ffmpeg_read_wrapper_base(NULL, target, sizeof(target), 0) == AVERROR_EOF);
    stopped = 0;
}

/* Opening a growing local file: what the read callback of its I/O context does at the end. */
static int waits, arriving = -1;

int progressive_test_usleep(useconds_t usec)
{
    (void)usec;
    waits++;
    if (arriving >= 0) {
        /* The download delivers while the player waits. */
        assert(write(arriving, "de", 2) == 2);
        arriving = -1;
    }
    return 0;
}

static AVIOContext *open_growing_file(CustomIOCtx_t *io, char *name, int *download)
{
    AVIOContext *pb;
    *download = mkstemp(name);
    assert(*download >= 0);
    memset(io, 0, sizeof(*io));
    io->szFile = name;
    waits = 0;
    progressive_playback = 1;
    pb = container_ffmpeg_get_avio_context(io, 4096);
    assert(pb);
    return pb;
}

static void close_growing_file(CustomIOCtx_t *io, AVIOContext *pb, const char *name, int download)
{
    progressive_playback = 0;
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    fclose(io->pFile);
    close(download);
    unlink(name);
}

static void open_waits_for_the_download(void)
{
    char name[] = "/tmp/exteplayer3-progressive-XXXXXX";
    CustomIOCtx_t io;
    uint8_t bytes[8];
    int download;
    AVIOContext *pb = open_growing_file(&io, name, &download);
    assert(write(download, "abc", 3) == 3);
    assert(pb->read_packet(&io, bytes, sizeof(bytes)) == 3 && !waits);
    /* At the end: one wait, then what has arrived meanwhile. */
    arriving = download;
    assert(pb->read_packet(&io, bytes, sizeof(bytes)) == 2 && !memcmp(bytes, "de", 2));
    assert(waits == 1);
    /* Nothing arrives: one wait for every read. */
    assert(pb->read_packet(&io, bytes, sizeof(bytes)) == 0 && waits == 2);
    /* A player that is closing does not wait. */
    stopped = 1;
    assert(pb->read_packet(&io, bytes, sizeof(bytes)) == 0 && waits == 2);
    stopped = 0;
    close_growing_file(&io, pb, name, download);
}

static void open_stops_waiting(void)
{
    char name[] = "/tmp/exteplayer3-progressive-XXXXXX";
    CustomIOCtx_t io;
    uint8_t byte;
    int download;
    AVIOContext *pb = open_growing_file(&io, name, &download);
    /* A download that has stalled: 100 waits in a row, then none. */
    for (int i = 0; i < 150; i++)
        assert(pb->read_packet(&io, &byte, 1) == 0);
    assert(waits == 100);
    /* A download that trickles: every byte starts a new row, 600 waits in all. */
    for (int i = 0; i < 600; i++) {
        assert(write(download, "x", 1) == 1);
        assert(pb->read_packet(&io, &byte, 1) == 1);
        assert(pb->read_packet(&io, &byte, 1) == 0);
    }
    assert(waits == 600);
    close_growing_file(&io, pb, name, download);
}

int main(void)
{
    Context_t context = {0};
    g_context = &context;
    ffmpeg_real_read_org = source;
    data_after_a_failed_first_read();
    end_is_reported_as_eof();
    open_waits_for_the_download();
    open_stops_waiting();
    return 0;
}
