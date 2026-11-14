#include <assert.h>
#include <signal.h>
#include "../output/linuxdvb_buffering.c"

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

static void blocked(int number)
{
    static const char text[] = "BufferingWriteV waits for room a single chunk can never get\n";
    (void)number;
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(2);
    _exit(1);
}

int main(void)
{
    uint8_t frame[256] = {1, 2, 3};
    struct iovec vector = {frame, sizeof(frame)};
    BufferingNode_t *node;

    assert(!pthread_mutex_init(&bufferingMtx, NULL));
    assert(!pthread_cond_init(&bufferingDataConsumedCond, NULL));
    assert(!pthread_cond_init(&bufferingdDataAddedCond, NULL));
    videofd = 7;
    maxBufferingDataSize = 128;
    signal(SIGALRM, blocked);
    alarm(3);
    /* One frame larger than the whole buffer, with nothing queued to be consumed. */
    assert(BufferingWriteV(videofd, &vector, 1) == sizeof(frame));
    alarm(0);
    node = bufferingQueueHead;
    assert(node && node == bufferingQueueTail && !node->next);
    assert(node->dataType == OUTPUT_VIDEO && node->dataSize == sizeof(frame));
    assert(!memcmp(node + 1, frame, sizeof(frame)));
    assert(bufferingDataSize == sizeof(*node) + sizeof(frame));
    free(node);
    pthread_mutex_destroy(&bufferingMtx);
    pthread_cond_destroy(&bufferingDataConsumedCond);
    pthread_cond_destroy(&bufferingdDataAddedCond);
    return 0;
}
