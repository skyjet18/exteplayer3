#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include "../container/container_ffmpeg.c"

/* The seek of the read-ahead buffer (buff_ffmpeg.c) that leaves the buffer:
 * ffmpeg_seek() hands it to the filler thread and waits for its result. The
 * input behind the buffer is the test's: reads give zeros, seeks go where
 * they are asked to. The position is beyond 2 GiB, as in a large file. */

#define GIB (1024LL * 1024 * 1024)

static int64_t seek_offset = -1;
static int seeks, fail_seek;

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
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

void LinuxDvbBuffSetStamp(void *stamp)
{
    (void)stamp;
}

static int read_input(void *opaque, uint8_t *buf, int size)
{
    (void)opaque;
    memset(buf, 0, size);
    return size;
}

static int64_t seek_input(void *opaque, int64_t offset, int whence)
{
    (void)opaque;
    assert(whence == SEEK_SET);
    __atomic_store_n(&seek_offset, offset, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&seeks, 1, __ATOMIC_SEQ_CST);
    return fail_seek ? AVERROR(EIO) : offset;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Context_t context = {.playback = &playback};

static void start(int64_t pos)
{
    avContextTab[0] = avformat_alloc_context();
    assert(avContextTab[0]);
    avContextTab[0]->pb = avio_alloc_context(av_malloc(4096), 4096, 0, NULL, NULL, NULL, NULL);
    assert(avContextTab[0]->pb);
    avContextTab[0]->pb->pos = pos;
    /* 64 kB of read-ahead behind the 1 MB that stays free */
    ffmpeg_buf_size = FILLBUFDIFF + 65536;
    ffmpeg_buf_seek_time = 0; /* no wait for the refill */
    ffmpeg_buf = av_malloc(ffmpeg_buf_size);
    assert(ffmpeg_buf);
    ffmpeg_buf_read = ffmpeg_buf_write = ffmpeg_buf;
    ffmpeg_read_org = read_input;
    ffmpeg_seek_org = seek_input;
    seeks = 0;
    seek_offset = -1;
    assert(ffmpeg_start_fillerTHREAD(&context) == 0);
}

static void stop(void)
{
    int id = hasfillerThreadStartedID;
    int i;
    hasfillerThreadStarted[id] = 2;
    for (i = 0; i < 500 && hasfillerThreadStarted[id] != 0; i++)
        usleep(10000);
    assert(hasfillerThreadStarted[id] == 0);
    av_freep(&avContextTab[0]->pb->buffer);
    avio_context_free(&avContextTab[0]->pb);
    avformat_free_context(avContextTab[0]);
    avContextTab[0] = NULL;
    ffmpeg_buf_free();
}

/* 1 MB ahead of a position beyond 2 GiB: the new position does not fit an
 * int, the seek is still a success */
static void seek_beyond_2_gib(void)
{
    int64_t pos = 3 * GIB, ret;
    start(pos);
    ret = ffmpeg_seek(NULL, pos + 1048576, SEEK_SET);
    printf("buffered seek to %" PRId64 ": %" PRId64 " (input seeks %d)\n", pos + 1048576, ret, seeks);
    assert(seeks == 1 && seek_offset == pos + 1048576);
    assert(ret == pos + 1048576);
    stop();
}

/* the error of the input comes back to the caller */
static void failed_seek(void)
{
    int64_t pos = 3 * GIB, ret;
    fail_seek = 1;
    start(pos);
    ret = ffmpeg_seek(NULL, pos + 1048576, SEEK_SET);
    printf("failed buffered seek: %" PRId64 "\n", ret);
    assert(seeks == 1 && ret == AVERROR(EIO));
    stop();
    fail_seek = 0;
}

int main(void)
{
    seek_beyond_2_gib();
    failed_seek();
    printf("buffered seek tests passed\n");
    return 0;
}
