#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS _IOR('o', 57, __u64)
#endif
#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif
#define ioctl test_ioctl
#define fopen test_fopen
int test_ioctl(int fd, unsigned long request, ...);
FILE *test_fopen(const char *path, const char *mode);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef fopen

/* /proc/stb/vmpeg/0/fallback_framerate at the start of the video decoder:
 * the frame rate of the track (in 1/1000 frames per second) rounded to the
 * nearest one the driver accepts, written before VIDEO_SET_STREAMTYPE.
 * The node is a buffer of the test, or missing. */

static int node_exists = 1;
static char node[32];
static FILE *node_file;
static int node_writes, streamtype_set, written_before_streamtype;
static Track_t track;
static int has_track = 1;

int test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == VIDEO_SET_STREAMTYPE)
    {
        streamtype_set++;
        written_before_streamtype = node_writes;
    }
    return 0;
}

FILE *test_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/stb/vmpeg/0/fallback_framerate"))
    {
        errno = ENOENT;
        return NULL;
    }
    assert(!strcmp(mode, "w"));
    if (!node_exists)
    {
        errno = ENOENT;
        return NULL;
    }
    node_writes++;
    memset(node, 0, sizeof(node));
    node_file = fmemopen(node, sizeof(node) - 1, "w");
    assert(node_file);
    return node_file;
}

stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int reset_writer(void) { return 0; }
static WriterCaps_t video_caps = {.dvbStreamType = STREAMTYPE_MPEG4_H264};
static Writer_t video_writer = {.reset = reset_writer, .caps = &video_caps};
Writer_t *getWriter(char *name) { (void)name; return &video_writer; }
Writer_t *getDefaultVideoWriter(void) { return NULL; }
Writer_t *getDefaultAudioWriter(void) { return NULL; }
ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
int32_t LinuxDvbBuffOpen(Context_t *context, char *type, int outfd, void *mtx)
{
    (void)context;
    (void)type;
    (void)outfd;
    (void)mtx;
    return 0;
}
int32_t LinuxDvbBuffClose(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlush(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlushAudio(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffResume(Context_t *context) { (void)context; return 0; }
void WriterWakeUp(void) {}
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int video_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    if (command == MANAGER_GETENCODING)
    {
        *(char **)argument = strdup("V_MPEG4/ISO/AVC");
        return 0;
    }
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = has_track ? &track : NULL;
    return has_track ? 0 : -1;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t video = {.Command = video_command};
static ManagerHandler_t managers = {.video = &video};
static Context_t context = {.playback = &playback, .manager = &managers};

/* starts the video decoder with this track frame rate; returns what the node
 * got, "-" when nothing was written */
static const char *start(uint32_t frame_rate)
{
    track.frame_rate = frame_rate;
    node_writes = streamtype_set = written_before_streamtype = 0;
    memset(node, 0, sizeof(node));
    assert(!LinuxDvbPlay(&context, "video"));
    assert(streamtype_set == 1);
    if (node_writes)
        assert(written_before_streamtype == 1);
    return node_writes ? node : "-";
}

int main(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    videofd = 3;

    assert(!strcmp(start(25000), "25000"));
    assert(!strcmp(start(23976), "23976"));
    assert(!strcmp(start(24000), "24000"));
    assert(!strcmp(start(29970), "29970"));
    assert(!strcmp(start(30000), "30000"));
    assert(!strcmp(start(50000), "50000"));
    assert(!strcmp(start(59940), "59940"));
    /* GStreamer's loop never reaches the last entry */
    assert(!strcmp(start(60000), "60000"));
    assert(!strcmp(start(120000), "60000"));
    /* nearest: 48 to 50, 12.5 to 23.976, 27 to 25 */
    assert(!strcmp(start(48000), "50000"));
    assert(!strcmp(start(12500), "23976"));
    assert(!strcmp(start(27000), "25000"));

    /* frame rate unknown, no track, no node: nothing written, the start goes on */
    assert(!strcmp(start(0), "-"));
    has_track = 0;
    assert(!strcmp(start(25000), "-"));
    has_track = 1;
    node_exists = 0;
    assert(!strcmp(start(25000), "-"));

    printf("fallback frame rate tests passed\n");
    return 0;
}
