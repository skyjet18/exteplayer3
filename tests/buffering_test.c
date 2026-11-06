#include <assert.h>
#include "../output/linuxdvb_buffering.c"

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

static BufferingNode_t *node(OutputType_t type)
{
    BufferingNode_t *result = calloc(1, sizeof(*result) + 4);
    assert(result);
    result->dataSize = 4;
    result->dataType = type;
    return result;
}

int main(void)
{
    PlaybackHandler_t playback = {.isDash = 1, .isAudioSwitching = 1};
    Context_t context = {.playback = &playback};
    BufferingNode_t *first = node(OUTPUT_VIDEO), *last = node(OUTPUT_VIDEO);
    BufferingNode_t *audio = node(OUTPUT_AUDIO);
    int output_pipe[2];
    char data;

    assert(!pthread_mutex_init(&bufferingMtx, NULL));
    assert(!pthread_cond_init(&bufferingDataConsumedCond, NULL));
    assert(!pthread_cond_init(&bufferingWriteFinishedCond, NULL));
    assert(!pipe(g_pfd) && !pipe(output_pipe));
    assert(fcntl(g_pfd[0], F_SETFL, O_NONBLOCK) >= 0);
    bufferingQueueHead = node(OUTPUT_AUDIO);
    bufferingQueueHead->next = first;
    first->next = audio;
    audio->next = last;
    last->next = node(OUTPUT_AUDIO);
    bufferingQueueTail = last->next;
    bufferingDataSize = 5 * (sizeof(BufferingNode_t) + 4);
    assert(!LinuxDvbBuffFlushAudio(&context));
    assert(bufferingQueueHead == first && first->next == last && !last->next);
    assert(bufferingQueueTail == last);
    assert(bufferingDataSize == 2 * (sizeof(BufferingNode_t) + 4));
    assert(!LinuxDvbBuffFlushAudio(&context)); /* Idempotent, no video loss. */
    FlushPipe(g_pfd[0]);
    assert(!WriteWithRetry(&context, g_pfd[0], output_pipe[1], NULL, "v", 1, 1));
    assert(read(output_pipe[0], &data, 1) == 1 && data == 'v');
    assert(!WriteWithRetry(&context, g_pfd[0], output_pipe[1], NULL, "a", 1, 0));
    assert(fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) >= 0);
    assert(read(output_pipe[0], &data, 1) == -1 && errno == EAGAIN);
    free(first);
    free(last);
    bufferingQueueHead = bufferingQueueTail = NULL;
    bufferingDataSize = 0;
    assert(!LinuxDvbBuffFlushAudio(&context));
    close(output_pipe[0]);
    close(output_pipe[1]);
    close(g_pfd[0]);
    close(g_pfd[1]);
    pthread_mutex_destroy(&bufferingMtx);
    pthread_cond_destroy(&bufferingDataConsumedCond);
    pthread_cond_destroy(&bufferingWriteFinishedCond);
    return 0;
}
