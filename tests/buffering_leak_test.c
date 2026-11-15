#include <assert.h>
#include <sanitizer/lsan_interface.h>
#include "../output/linuxdvb_buffering.c"

/* The playback is terminated: BufferingWriteV() queues nothing any more. */
int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 1;
}

/* The buffering thread is not started here. */
bool PlaybackDieNowRegisterCallback(PlaybackDieNowCallback callback)
{
    (void)callback;
    return true;
}

ssize_t WriteWithRetry(Context_t *context, int pipefd, int fd, void *mutex, const void *data,
                       int size, int is_video)
{
    (void)context;
    (void)pipefd;
    (void)fd;
    (void)mutex;
    (void)data;
    (void)is_video;
    return size;
}

int main(void)
{
    static uint8_t frame[256];
    struct iovec vector = {frame, sizeof(frame)};
    int i;

    assert(!pthread_mutex_init(&bufferingMtx, NULL));
    videofd = 7;
    /* More chunks than stale pointers can hide from the leak check. */
    for (i = 0; i < 100; i++)
        assert(BufferingWriteV(videofd, &vector, 1) > 0);
    assert(!bufferingQueueHead && !bufferingDataSize);
    assert(!__lsan_do_recoverable_leak_check());
    pthread_mutex_destroy(&bufferingMtx);
    return 0;
}
