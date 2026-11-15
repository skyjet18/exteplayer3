#include <assert.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/types.h>
#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS _IOR('o', 57, __u64)
#endif
#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif
#ifndef VIDEO_GET_FRAME_RATE
#define VIDEO_GET_FRAME_RATE _IOR('o', 56, unsigned int)
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

#define VC(w, h, f, p) \
    "{\"v_c\":{\"id\":1,\"e\":\"V_MPEG4/ISO/AVC\",\"n\":\"und\",\"w\":" #w ",\"h\":" #h \
    ",\"f\":" #f ",\"p\":" #p ",\"an\":0,\"ad\":1}}\n"

extern Manager_t VideoManager;
static Context_t context;
static PlaybackHandler_t playback;
static ManagerHandler_t managers = {.video = &VideoManager};
static OutputHandler_t outputs = {.video = &LinuxDvbOutput};

/* the driver: queued events, the PTS of the decoder and what it answers when asked */
static struct video_event events[8];
static int event_count, event_next;
static unsigned long long decoder_pts;
static int pts_fails, driver_width, driver_height, size_failures;
static unsigned int driver_frame_rate;
static const char *driver_progressive;
/* events the driver sends once it has been asked for the size, and its answers from then on */
static struct video_event later[4];
static int later_count;
static int later_width, later_height;
static unsigned int later_frame_rate;
static const char *later_progressive;
static int pts_requests, size_requests, frame_rate_requests, progressive_reads;

/* what the player has sent */
static char sent[1024];

int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == videofd && fds[0].events == POLLPRI && !timeout);
    fds[0].revents = event_next < event_count ? POLLPRI : 0;
    return event_next < event_count;
}

int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *argument;
    assert(fd == videofd);
    va_start(args, request);
    argument = va_arg(args, void *);
    va_end(args);
    if (request == VIDEO_GET_EVENT)
    {
        assert(event_next < event_count);
        *(struct video_event *)argument = events[event_next++];
        return 0;
    }
    if (request == VIDEO_GET_PTS)
    {
        pts_requests++;
        errno = ENOTTY;
        if (pts_fails)
            return -1;
        *(unsigned long long *)argument = decoder_pts;
        return 0;
    }
    if (request == VIDEO_GET_SIZE)
    {
        int width = driver_width, height = driver_height;
        size_requests++;
        if (later_count)
        {
            int i;
            for (i = 0; i < later_count; i++)
                events[event_count++] = later[i];
            later_count = 0;
            driver_width = later_width;
            driver_height = later_height;
            driver_frame_rate = later_frame_rate;
            driver_progressive = later_progressive;
        }
        errno = EINVAL;
        if (size_failures > 0)
        {
            size_failures--;
            return -1;
        }
        if (width < 0)
            return -1;
        ((video_size_t *)argument)->w = width;
        ((video_size_t *)argument)->h = height;
        return 0;
    }
    if (request == VIDEO_GET_FRAME_RATE)
    {
        frame_rate_requests++;
        *(unsigned int *)argument = driver_frame_rate;
        return 0;
    }
    assert(!"unexpected ioctl");
    return -1;
}

FILE *test_fopen(const char *path, const char *mode)
{
    assert(!strcmp(path, "/proc/stb/vmpeg/0/progressive") && !strcmp(mode, "r"));
    progressive_reads++;
    if (!driver_progressive)
        return NULL;
    return fmemopen((void *)driver_progressive, strlen(driver_progressive), "r");
}

void E2iSendMsg(const char *format, ...)
{
    va_list args;
    size_t used = strlen(sent);
    va_start(args, format);
    vsnprintf(sent + used, sizeof(sent) - used, format, args);
    va_end(args);
}

/* as main/exteplayer.c reports the current video track */
static void report_video_track(void)
{
    TrackDescription_t *track = NULL;
    assert(!VideoManager.Command(&context, MANAGER_GET_TRACK_DESC, &track) && track);
    E2iSendMsg("{\"v_c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"w\":%d,\"h\":%d,\"f\":%u,\"p\":%d,\"an\":%d,\"ad\":%d}}\n",
               track->Id, track->Encoding, track->Name, track->width, track->height,
               track->frame_rate, track->progressive, track->aspect_ratio_num, track->aspect_ratio_den);
    free(track->Encoding);
    free(track->Name);
    free(track);
}

int32_t container_ffmpeg_update_tracks(Context_t *ctx, char *filename, int initial)
{
    (void)ctx;
    (void)filename;
    (void)initial;
    return 0;
}

static Writer_t writer;
Writer_t *getWriter(char *encoding) { assert(!strcmp(encoding, "V_MPEG4/ISO/AVC")); return &writer; }
Writer_t *getDefaultVideoWriter(void) { return NULL; }
Writer_t *getDefaultAudioWriter(void) { return NULL; }
ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; (void)ic; return 0; }
ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; (void)ic; return 0; }
int32_t LinuxDvbBuffOpen(Context_t *ctx, char *type, int outfd, void *mtx) { (void)ctx; (void)type; (void)outfd; (void)mtx; return 0; }
int32_t LinuxDvbBuffClose(Context_t *ctx) { (void)ctx; return 0; }
int32_t LinuxDvbBuffFlush(Context_t *ctx) { (void)ctx; return 0; }
int32_t LinuxDvbBuffFlushAudio(Context_t *ctx) { (void)ctx; return 0; }
int32_t LinuxDvbBuffResume(Context_t *ctx) { (void)ctx; return 0; }
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }
stb_type_t GetSTBType(void) { return STB_VUPLUS; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }

/* the video track as FFmpeg described it */
static void video_track(int width, int height, uint32_t frame_rate)
{
    Track_t track = {0};
    int id = 1;
    track.Id = id;
    track.Name = "und";
    track.Encoding = "V_MPEG4/ISO/AVC";
    track.width = width;
    track.height = height;
    track.frame_rate = frame_rate;
    track.aspect_ratio_den = 1;
    context.manager = &managers;
    context.output = &outputs;
    context.playback = &playback;
    videofd = 5;
    assert(!VideoManager.Command(&context, MANAGER_ADD, &track));
    assert(!VideoManager.Command(&context, MANAGER_SET, &id));
    assert(!VideoManager.Command(&context, MANAGER_REGISTER_UPDATED_TRACK_INFO, report_video_track));
}

static void size_event(int width, int height)
{
    assert(event_count < 8);
    events[event_count].type = VIDEO_EVENT_SIZE_CHANGED;
    events[event_count].u.size.w = width;
    events[event_count].u.size.h = height;
    events[event_count++].u.size.aspect_ratio = 1;
}

static void value_event(int type, int value)
{
    assert(event_count < 8);
    events[event_count].type = type;
    events[event_count++].u.frame_rate = value;
}

/* one video frame is written while the decoder is at pts; returns what the player sends for it */
static const char *frame(uint32_t width, uint32_t height, uint32_t frame_rate, unsigned long long pts)
{
    AudioVideoOut_t out = {0};
    out.type = "video";
    out.width = width;
    out.height = height;
    out.frameRate = frame_rate;
    decoder_pts = pts;
    sent[0] = 0;
    assert(!LinuxDvbOutput.Write(&context, &out));
    return sent;
}

/* the front end asks for the position while the decoder is at pts */
static const char *position(unsigned long long pts)
{
    unsigned long long reported = 0;
    decoder_pts = pts;
    sent[0] = 0;
    assert(!LinuxDvbOutput.Command(&context, OUTPUT_PTS, &reported) && reported == pts);
    return sent;
}

static void driver_answers(int width, int height, unsigned int frame_rate, const char *progressive)
{
    driver_width = width;
    driver_height = height;
    driver_frame_rate = frame_rate;
    driver_progressive = progressive;
}

/* once asked for the size, the driver sends these events and has the values of this stream */
static void driver_then_sends(int width, int height, unsigned int frame_rate, const char *progressive)
{
    int i;
    for (i = 0; i < event_count; i++)
        later[i] = events[i];
    later_count = event_count;
    event_count = 0;
    later_width = width;
    later_height = height;
    later_frame_rate = frame_rate;
    later_progressive = progressive;
}

/*
 * A track without a picture size starts: the decoder still has the PTS of
 * the previous playback, jumps to this stream and runs. After the quiet
 * period the driver has been asked, nothing has been sent.
 */
static void start_without_picture(uint32_t frame_rate)
{
    video_track(0, 0, frame_rate);
    assert(!strcmp(frame(0, 0, frame_rate, 575550), ""));
    assert(!strcmp(frame(0, 0, frame_rate, 738000), ""));
    assert(!strcmp(frame(0, 0, frame_rate, 742000), ""));
    assert(!size_requests && !frame_rate_requests && !progressive_reads);
    assert(!strcmp(frame(0, 0, frame_rate, 748000), ""));
    assert(size_requests == 1 && frame_rate_requests == 1 && progressive_reads == 1);
}

/*
 * 640x360 at 25 fps after 1280x720 at 23.976 fps: the driver answers with the
 * values of the previous playback, then sends its size and frame-rate events.
 */
static void stale_answer_is_dropped_when_events_follow(void)
{
    driver_answers(1280, 720, 23976, "1\n");
    size_event(640, 360);
    value_event(VIDEO_EVENT_FRAME_RATE_CHANGED, 25000);
    driver_then_sends(640, 360, 25000, "1\n");
    start_without_picture(25000);
    assert(!strcmp(frame(0, 0, 25000, 751600), ""));
    assert(!strcmp(frame(0, 0, 25000, 751700), ""));
    assert(!strcmp(frame(0, 0, 25000, 780000), ""));
    assert(!strcmp(frame(0, 0, 25000, 800000), VC(640, 360, 25000, 1)));
    assert(size_requests == 1 && frame_rate_requests == 1 && progressive_reads == 2);
    assert(!strcmp(frame(0, 0, 25000, 900000), ""));
    assert(!strcmp(position(950000), ""));
}

/*
 * The file is written to the end before the quiet period is over (640x360
 * progressive after 1920x1080 interlaced); the front end goes on asking for
 * the position. The driver answers with the old values, then sends a size
 * and a progressive event.
 */
static void events_after_the_last_write_are_read_on_position_requests(void)
{
    driver_answers(1920, 1080, 25000, "0\n");
    size_event(640, 360);
    value_event(16, 1);
    driver_then_sends(640, 360, 25000, "1\n");
    video_track(0, 0, 25000);
    assert(!strcmp(frame(0, 0, 25000, 576848), ""));
    assert(!strcmp(frame(0, 0, 25000, 738000), ""));
    assert(!strcmp(frame(0, 0, 25000, 740000), ""));
    assert(!strcmp(position(745000), ""));
    assert(!size_requests);
    assert(!strcmp(position(760000), ""));
    assert(size_requests == 1 && frame_rate_requests == 1 && progressive_reads == 1);
    assert(!strcmp(position(800000), ""));
    assert(event_next == 2);
    assert(!strcmp(position(830000), ""));
    assert(!strcmp(position(850000), VC(640, 360, 25000, 1)));
    assert(size_requests == 1 && frame_rate_requests == 2 && progressive_reads == 1);
    assert(!strcmp(position(900000), ""));
}

/* Same picture as before, so no event; the file is written to the end after the driver was asked. */
static void held_answer_is_used_on_position_requests(void)
{
    driver_answers(640, 360, 25000, "1\n");
    start_without_picture(25000);
    assert(!strcmp(position(780000), ""));
    assert(!strcmp(position(800000), VC(640, 360, 25000, 1)));
    assert(size_requests == 2 && frame_rate_requests == 2 && progressive_reads == 2);
    assert(!strcmp(position(900000), ""));
}

/* The same while data is still written. */
static void unchanged_values_are_asked_from_the_decoder(void)
{
    driver_answers(640, 360, 25000, "1\n");
    start_without_picture(25000);
    assert(!strcmp(frame(0, 0, 25000, 780000), ""));
    assert(!strcmp(frame(0, 0, 25000, 793000), VC(640, 360, 25000, 1)));
    assert(!strcmp(frame(0, 0, 25000, 900000), ""));
    assert(pts_requests == 6 && size_requests == 2 && frame_rate_requests == 2 && progressive_reads == 2);
}

/* The driver has no size at first: it is asked again, the track is reported once. */
static void size_is_asked_again_when_the_driver_has_none(void)
{
    driver_answers(640, 360, 25000, "1\n");
    size_failures = 2;
    start_without_picture(25000);
    assert(!strcmp(position(800000), ""));
    assert(size_requests == 2);
    assert(!strcmp(position(830000), ""));
    assert(!strcmp(position(850000), VC(640, 360, 25000, 1)));
    assert(size_requests == 3 && frame_rate_requests == 3 && progressive_reads == 3);
}

/* The events come before the driver is asked: one report after the last of them. */
static void events_are_reported_once(void)
{
    video_track(0, 0, 90000000);
    size_event(1280, 720);
    value_event(VIDEO_EVENT_FRAME_RATE_CHANGED, 25000);
    value_event(16, 1);
    assert(!strcmp(frame(0, 0, 90000000, 575550), ""));
    assert(!strcmp(frame(0, 0, 90000000, 738000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 738100),
                   "{\"v_e\":{\"w\":1280,\"h\":720,\"a\":1,\"f\":25000,\"p\":1}}\n"));
    assert(!strcmp(frame(0, 0, 90000000, 740000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 742000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 780000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 790000), VC(1280, 720, 25000, 1)));
    assert(!size_requests && !frame_rate_requests && !progressive_reads);
}

/* FFmpeg knows the picture: the events change nothing but the progressive flag. */
static void known_picture_is_left_alone(void)
{
    video_track(1280, 720, 25000);
    driver_answers(1920, 1080, 50000, "0\n");
    size_event(1920, 1080);
    value_event(VIDEO_EVENT_FRAME_RATE_CHANGED, 50000);
    value_event(16, 0);
    assert(!strcmp(frame(1280, 720, 25000, 575550), ""));
    assert(!strcmp(frame(1280, 720, 25000, 738000), ""));
    assert(!strcmp(frame(1280, 720, 25000, 742000), VC(1280, 720, 25000, 0)
                   "{\"v_e\":{\"w\":1920,\"h\":1080,\"a\":1,\"f\":50000,\"p\":0}}\n"));
    assert(!strcmp(frame(1280, 720, 25000, 800000), ""));
    assert(!strcmp(frame(1280, 720, 25000, 900000), ""));
    assert(!pts_requests);
    size_event(1920, 1080);
    assert(!strcmp(position(950000), ""));
    assert(pts_requests == 1 && event_next == 3);
    assert(!size_requests && !frame_rate_requests && !progressive_reads);
}

/* Only the frame rate differs from the previous playback; no progressive file. */
static void event_and_decoder_values_are_combined(void)
{
    video_track(0, 0, 90000000);
    driver_answers(720, 576, 25000, NULL);
    value_event(VIDEO_EVENT_FRAME_RATE_CHANGED, 50000);
    assert(!strcmp(frame(0, 0, 90000000, 575550), ""));
    assert(!strcmp(frame(0, 0, 90000000, 738000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 740000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 780000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 790000), VC(720, 576, 50000, -1)));
    assert(size_requests == 1 && !frame_rate_requests && progressive_reads == 1);
}

/* Nothing is sent for a decoder that answers nothing; it is given up after ten more tries. */
static void silent_decoder_sends_nothing(void)
{
    unsigned long long pts;
    driver_answers(-1, 0, 0, NULL);
    start_without_picture(90000000);
    for (pts = 800000; pts <= 1250000; pts += 50000)
        assert(!strcmp(position(pts), ""));
    assert(size_requests == 11);
    assert(!strcmp(position(1300000), ""));
    assert(!strcmp(frame(0, 0, 90000000, 1350000), ""));
    assert(size_requests == 11);
}

static void decoder_without_pts_is_not_asked(void)
{
    video_track(0, 0, 90000000);
    driver_answers(1280, 720, 25000, "1\n");
    pts_fails = 1;
    assert(!strcmp(frame(0, 0, 90000000, 0), ""));
    assert(!strcmp(frame(0, 0, 90000000, 0), ""));
    assert(pts_requests == 1 && !size_requests && !frame_rate_requests && !progressive_reads);
}

/* The output keeps its state for the process: every case gets a process of its own. */
static int failed(const char *name, void (*test)(void))
{
    int status = 0;
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        test();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
    {
        fprintf(stderr, "video_info_test: %s failed\n", name);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += failed("stale_answer_is_dropped_when_events_follow", stale_answer_is_dropped_when_events_follow);
    failures += failed("events_after_the_last_write_are_read_on_position_requests", events_after_the_last_write_are_read_on_position_requests);
    failures += failed("held_answer_is_used_on_position_requests", held_answer_is_used_on_position_requests);
    failures += failed("unchanged_values_are_asked_from_the_decoder", unchanged_values_are_asked_from_the_decoder);
    failures += failed("size_is_asked_again_when_the_driver_has_none", size_is_asked_again_when_the_driver_has_none);
    failures += failed("events_are_reported_once", events_are_reported_once);
    failures += failed("known_picture_is_left_alone", known_picture_is_left_alone);
    failures += failed("event_and_decoder_values_are_combined", event_and_decoder_values_are_combined);
    failures += failed("silent_decoder_sends_nothing", silent_decoder_sends_nothing);
    failures += failed("decoder_without_pts_is_not_asked", decoder_without_pts_is_not_asked);
    return failures != 0;
}
