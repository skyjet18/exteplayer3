#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

extern Manager_t AudioManager, VideoManager, SubtitleManager;
static pthread_rwlock_t container_lock = PTHREAD_RWLOCK_INITIALIZER;

int32_t container_ffmpeg_update_tracks(Context_t *context, char *filename, int initial)
{
    (void)context;
    (void)filename;
    (void)initial;
    pthread_rwlock_wrlock(&container_lock);
    pthread_rwlock_unlock(&container_lock);
    return 0;
}

static void check_manager(Manager_t *manager, Context_t *context)
{
    Track_t track = {0}, *stored = NULL;
    int id = 7;
    track.Id = id;
    track.Name = "first";
    track.Encoding = "test";
    track.stream = (void *)1;
    assert(!manager->Command(context, MANAGER_ADD, &track));
    assert(!manager->Command(context, MANAGER_SET, &id));
    track.Name = "second";
    track.stream = (void *)2;
    track.AVIdx = 1;
    track.aacbuf = malloc(4);
    assert(track.aacbuf);
    assert(!manager->Command(context, MANAGER_ADD, &track));
    assert(!manager->Command(context, MANAGER_GET_TRACK, &stored));
    assert(stored && stored->stream == (void *)2 && stored->AVIdx == 1);
    assert(!strcmp(stored->Name, "second"));
    /* Refreshing from the same owned buffer must not free that buffer. */
    track = *stored;
    assert(!manager->Command(context, MANAGER_ADD, &track));
    stored->aacbuf[0] = 42;
    assert(!manager->Command(context, MANAGER_DEL, NULL));
}

struct thread_args { Context_t *context; Manager_t *manager; };
static void *lists(void *opaque)
{
    struct thread_args *args = opaque;
    int i;
    for (i = 0; i < 1000; i++)
    {
        TrackDescription_t *list = NULL;
        assert(!args->manager->Command(args->context, MANAGER_LIST, &list));
        if (list)
        {
            int j;
            for (j = 0; list[j].Id >= 0; j++)
            {
                free(list[j].Name);
                free(list[j].Encoding);
            }
            free(list);
        }
    }
    return NULL;
}

int main(void)
{
    Context_t context = {0};
    PlaybackHandler_t playback = {0};
    Manager_t *managers[] = {&AudioManager, &VideoManager, &SubtitleManager};
    unsigned int i;
    context.playback = &playback;
    for (i = 0; i < sizeof(managers) / sizeof(managers[0]); i++)
    {
        pthread_t thread;
        struct thread_args args = {&context, managers[i]};
        int j;
        check_manager(managers[i], &context);
        assert(!pthread_create(&thread, NULL, lists, &args));
        for (j = 0; j < 1000; j++)
        {
            Track_t track = {0};
            track.Id = 1;
            track.Name = "concurrent";
            track.Encoding = "test";
            pthread_rwlock_rdlock(&container_lock);
            assert(!managers[i]->Command(&context, MANAGER_ADD, &track));
            pthread_rwlock_unlock(&container_lock);
        }
        assert(!pthread_join(thread, NULL));
        assert(!managers[i]->Command(&context, MANAGER_DEL, NULL));
    }
    return 0;
}
