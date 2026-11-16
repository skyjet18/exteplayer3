#include <assert.h>
#include <errno.h>
#include <poll.h>
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
#define poll test_poll
#define fopen test_fopen
int test_ioctl(int fd, unsigned long request, ...);
int test_poll(struct pollfd *fds, nfds_t count, int timeout);
FILE *test_fopen(const char *path, const char *mode);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef poll
#undef fopen

/* The position of the DVB output (OUTPUT_PTS) with a driver whose
 * VIDEO_GET_PTS answers 0: before any video with a PTS was written that is
 * the position, afterwards the PTS of the audio decoder is taken when it has
 * one. The driver is described by the answers of the two ioctls. */

enum { VIDEO_FD = 3, AUDIO_FD = 4 };

static unsigned long long video_pts, audio_pts;
static int video_fails, audio_fails, audio_requests;

int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *argument;
    va_start(args, request);
    argument = va_arg(args, void *);
    va_end(args);
    if (request == VIDEO_GET_PTS)
    {
        assert(fd == VIDEO_FD);
        errno = EINVAL;
        if (video_fails)
            return -1;
        *(unsigned long long *)argument = video_pts;
        return 0;
    }
    if (request == AUDIO_GET_PTS)
    {
        assert(fd == AUDIO_FD);
        audio_requests++;
        errno = EINVAL;
        if (audio_fails)
            return -1;
        *(unsigned long long *)argument = audio_pts;
        return 0;
    }
    /* clear, play and the rest: accepted */
    return 0;
}

int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    assert(count == 1);
    fds[0].revents = 0;
    return 0;
}

FILE *test_fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    errno = ENOENT;
    return NULL;
}

stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int write_packet(void *call) { (void)call; return 100; }
static int reset_writer(void) { return 0; }
static WriterCaps_t video_caps = {.dvbStreamType = STREAMTYPE_MPEG4_H264};
static Writer_t video_writer = {.reset = reset_writer, .writeData = write_packet, .caps = &video_caps};
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
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int manager_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    if (command == MANAGER_GETENCODING)
        *(char **)argument = strdup("V_MPEG4/ISO/AVC");
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t video = {.Command = manager_command};
static Manager_t audio = {.Command = manager_command};
static ManagerHandler_t managers = {.video = &video, .audio = &audio};
static Context_t context = {.playback = &playback, .manager = &managers};

static void write_video(unsigned long long pts)
{
    uint8_t data[8] = {0, 0, 0, 1, 0x65};
    AudioVideoOut_t out = {.data = data, .len = sizeof(data), .pts = pts, .type = "video",
                           .width = 1280, .height = 720};
    assert(!LinuxDvbOutput.Write(&context, &out));
}

static unsigned long long position(int expected_ret)
{
    unsigned long long pts = 12345;
    assert(Command(&context, OUTPUT_PTS, &pts) == expected_ret);
    return pts;
}

int main(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    videofd = VIDEO_FD;
    audiofd = AUDIO_FD;

    /* nothing written yet: 0 is the position of the video decoder, as before */
    video_pts = 0;
    audio_pts = 4500;
    audio_requests = 0;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0 && audio_requests == 0);

    /* a stream that starts at 0: its first picture does not change that */
    write_video(0);
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0 && audio_requests == 0);
    write_video(INVALID_PTS_VALUE);
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0 && audio_requests == 0);

    /* video with a PTS written, the decoder still says 0: the audio PTS */
    write_video(90000);
    audio_pts = 90300;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 90300 && audio_requests == 1);

    /* without an audio PTS (none yet, failing, no audio decoder) 0 stays */
    audio_pts = 0;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0);
    audio_fails = 1;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0);
    audio_fails = 0;
    audiofd = -1;
    audio_requests = 0;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0 && audio_requests == 0);
    audiofd = AUDIO_FD;

    /* a running video decoder is asked alone */
    video_pts = 180000;
    audio_pts = 170000;
    audio_requests = 0;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 180000 && audio_requests == 0);

    /* a clear starts over: 0 is a position again until video is written */
    assert(!Command(&context, OUTPUT_CLEAR, "video"));
    video_pts = 0;
    audio_pts = 777;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0 && audio_requests == 0);
    write_video(270000);
    assert(position(cERR_LINUXDVB_NO_ERROR) == 777 && audio_requests == 1);
    assert(!Command(&context, OUTPUT_FLUSH, "video"));
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0);
    write_video(270000);
    assert(!Command(&context, OUTPUT_STOP, "video"));
    assert(position(cERR_LINUXDVB_NO_ERROR) == 0);

    /* a failing VIDEO_GET_PTS still falls back to the audio decoder */
    video_fails = 1;
    audio_pts = 360000;
    assert(position(cERR_LINUXDVB_NO_ERROR) == 360000);
    audio_fails = 1;
    assert(position(cERR_LINUXDVB_ERROR) == 0);

    printf("video PTS tests passed\n");
    return 0;
}
