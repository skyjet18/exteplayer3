/*
 * DreamNextGen (AMLogic dreamone/dreamtwo) video output via the
 * Dreambox-private DVB-API on /dev/dvb/adapter0/video0:
 *   VIDEO_SET_DEC_SYSINFO  _IOWR('o', 65, struct dec_sysinfo)   48 B
 *   VIDEO_SET_FRAME        _IOWR('o', 64, struct video_frame)  168 B
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <linux/dvb/video.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "writer.h"
#include "bcm_ioctls.h"

#define cERR_DREAMVIDEO_NO_ERROR    0
#define cERR_DREAMVIDEO_ERROR      -1

/* Dual-log: stderr (filtered by serviceapp) + /tmp/dream_video.log. */
static void dv_log_emit(const char *line)
{
    static FILE *fp = NULL;
    static int   tried = 0;
    if (!fp && !tried) {
        tried = 1;
        int fd = open("/tmp/dream_video.log", // NOSONAR
                      O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                      0600);
        if (fd >= 0) {
            fp = fdopen(fd, "a");
            if (!fp) close(fd);
        }
        if (fp) setvbuf(fp, NULL, _IOLBF, 0);
    }
    if (!fp) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(fp, "%02d:%02d:%02d.%03ld %s\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, line);
}
#define DV_DBG(fmt, ...) do { \
    char _dv_buf[512]; \
    int _dv_n = snprintf(_dv_buf, sizeof(_dv_buf), "[dream_video] " fmt, ##__VA_ARGS__); \
    fprintf(stderr, "%s\n", _dv_buf); \
    if (_dv_n > 0) dv_log_emit(_dv_buf); \
} while (0)

/* Dreambox-private DVB-API extensions. */
struct dec_sysinfo {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t rate;          /* duration per frame in 96000-tick units */
    uint32_t extra;
    uint32_t status;
    uint32_t ratio;
    void    *param;         /* 8 byte on aarch64 -> total 48 bytes  */
    uint64_t ratio64;
};

struct video_frame {
    uint64_t       pts;           /* nanoseconds, GstClockTime style */
    ssize_t        bytes[8];
    const uint8_t *data[8];
    int            is_phys_addr[8];
};

/* video_frame.pts of the rest of a frame the driver took only in part */
#define DV_PTS_NONE UINT64_MAX

#define VIDEO_SET_DEC_SYSINFO  _IOWR('o', 65, struct dec_sysinfo)
#define VIDEO_SET_FRAME        _IOWR('o', 64, struct video_frame)

/* video_event.type of a PTS jump the driver waits to be flushed for */
#define DV_EVENT_DISCONTINUE   64

/* vdec_type_t — packed into dec_sysinfo.format. */
#define DEC_FORMAT_MPEG4_3  1
#define DEC_FORMAT_MPEG4_5  3
#define DEC_FORMAT_H264     4
#define DEC_FORMAT_AVS      13
#define DEC_FORMAT_HEVC     15
#define DEC_FORMAT_VP9      16

/* dec_sysinfo.param flag bits (kept as void *). */
#define EXTERNAL_PTS 1
#define SYNC_OUTSIDE 2

/* Dream-mapped vstream_type_t for VIDEO_SET_STREAMTYPE, the values
 * enigma2 uses for the Dreambox decoders. */
#define DREAM_STREAMTYPE_MPEG2  0
#define DREAM_STREAMTYPE_H264   1
#define DREAM_STREAMTYPE_MPEG4  4
#define DREAM_STREAMTYPE_HEVC   22
#define DREAM_STREAMTYPE_VP9    23
/* meson64 passes a stream type it does not map on as the decoder format;
 * 7 reaches the AVS decoder with the patched meson64 */
#define DREAM_STREAMTYPE_AVS    7
#define DREAM_STREAMTYPE_AVS2   15

static const char VIDEO_DEV[]   = "/dev/dvb/adapter0/video0";
static const char AMPOLL_DEV[]  = "/dev/amvideo_poll";

static pthread_mutex_t       dv_mutex       = PTHREAD_MUTEX_INITIALIZER;
static int                   dv_fd          = -1;
static int                   dv_poll_fd     = -1;
static int                   dv_inited      = 0;
static unsigned long long    dv_current_pts = 0;
static uint64_t              dv_frame_dur_ns  = 20000000ULL;   /* 50 fps */

/* Producer/consumer queue between av_read_frame and VIDEO_SET_FRAME. */
#define DV_Q_CAP   256              /* ~5 s @ 50 fps */
#define DV_STALL_MS 1200            /* video PTS stands still while the clock runs */
#define DV_LEAD_90K (2 * 90000)     /* frames written ahead of the frame on screen */
typedef struct {
    uint8_t *data;
    size_t   size;
    int64_t  pts_90k;   /* -1 = unknown */
} dv_qitem_t;

static dv_qitem_t       dv_q[DV_Q_CAP];
static int              dv_q_head    = 0;     /* producer writes here */
static int              dv_q_tail    = 0;     /* consumer reads here  */
static pthread_mutex_t  dv_q_mu      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   dv_q_nonemp  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   dv_q_nonfull = PTHREAD_COND_INITIALIZER;
static pthread_t        dv_q_thread;
static int              dv_q_running = 0;
static int              dv_q_stop    = 0;
/* paused: the consumer holds the queued frames, the decoder is frozen */
static int              dv_q_paused  = 0;
static uint64_t         dv_flush_vpts = 0;   /* PTS on screen at the last flush */
/* DivX 3.11: the picture size goes ahead of the first frame after a start or flush */
static int              dv_divx3_header = 1;

/* sysfs helpers — best-effort, errors ignored. */
static void dv_sysfs_write(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(value, f);
    fclose(f);
}

/* dreamvideosink fasst freerun_mode / show_first_frame_nosync nicht an
 * (runtime strace bestätigt). Wir auch nicht. disable_video=0 reicht
 * damit das video plane sichtbar wird. */
static void dv_enable_display(void)
{
    dv_sysfs_write("/sys/class/video/disable_video", "0");
}

static int dv_streamtype_for(int bcm_streamtype, int *out)
{
    switch (bcm_streamtype) {
    case STREAMTYPE_MPEG4_H264: *out = DREAM_STREAMTYPE_H264; return 0;
    case STREAMTYPE_MPEG2:      *out = DREAM_STREAMTYPE_MPEG2; return 0;
    case STREAMTYPE_MPEG1:      *out = DREAM_STREAMTYPE_MPEG2; return 0;
    case STREAMTYPE_MPEG4_H265: *out = DREAM_STREAMTYPE_HEVC; return 0;
    case STREAMTYPE_MPEG4_Part2: *out = DREAM_STREAMTYPE_MPEG4; return 0;
    case STREAMTYPE_DIVX311:    *out = DREAM_STREAMTYPE_MPEG4; return 0;
    case STREAMTYPE_VB9:        *out = DREAM_STREAMTYPE_VP9; return 0;
    case STREAMTYPE_AVS:        *out = DREAM_STREAMTYPE_AVS; return 0;
    case STREAMTYPE_AVS2:       *out = DREAM_STREAMTYPE_AVS2; return 0;
    default: return -1;
    }
}

static int dv_decformat_for(int bcm_streamtype, uint32_t *out)
{
    switch (bcm_streamtype) {
    case STREAMTYPE_MPEG4_H264: *out = DEC_FORMAT_H264; return 0;
    case STREAMTYPE_MPEG4_H265: *out = DEC_FORMAT_HEVC; return 0;
    case STREAMTYPE_MPEG4_Part2: *out = DEC_FORMAT_MPEG4_5; return 0;
    case STREAMTYPE_DIVX311:    *out = DEC_FORMAT_MPEG4_3; return 0;
    case STREAMTYPE_VB9:        *out = DEC_FORMAT_VP9; return 0;
    case STREAMTYPE_AVS:        *out = DEC_FORMAT_AVS; return 0;
    case STREAMTYPE_AVS2:       *out = 0; return 0;
    case STREAMTYPE_MPEG2:      *out = 0; return 0;
    case STREAMTYPE_MPEG1:      *out = 0; return 0;
    default: return -1;
    }
}

/* The size from an AVS sequence header: profile, level, progressive,
 * then 14 bits width and 14 bits height. */
static int dv_avs_size(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height)
{
    size_t i;
    for (i = 0; data && i + 10 <= len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0xb0) {
            uint32_t v = (uint32_t)data[i + 6] << 24 | (uint32_t)data[i + 7] << 16 | (uint32_t)data[i + 8] << 8 | data[i + 9];
            *width  = (v >> 17) & 0x3fff;
            *height = (v >> 3) & 0x3fff;
            return *width && *height ? 0 : -1;
        }
    }
    return -1;
}

/* ----- Queue helpers + consumer thread -------------------------------- */

static int dv_q_is_full_locked(void)  { return ((dv_q_head + 1) % DV_Q_CAP) == dv_q_tail; }
static int dv_q_is_empty_locked(void) { return dv_q_head == dv_q_tail; }

static int dv_q_depth_locked(void)
{
    return (dv_q_head - dv_q_tail + DV_Q_CAP) % DV_Q_CAP;
}

static int dv_q_push(uint8_t *data, size_t size, int64_t pts_90k)
{
    pthread_mutex_lock(&dv_q_mu);
    /* Wait for room, a dropped frame breaks the H.264 reference chain.
     * The consumer waits for the decoder, so a full queue is normal. */
    while (dv_q_is_full_locked() && !dv_q_stop)
        pthread_cond_wait(&dv_q_nonfull, &dv_q_mu);
    if (dv_q_stop) {
        pthread_mutex_unlock(&dv_q_mu);
        free(data);
        return -1;
    }
    dv_q[dv_q_head].data    = data;
    dv_q[dv_q_head].size    = size;
    dv_q[dv_q_head].pts_90k = pts_90k;
    dv_q_head = (dv_q_head + 1) % DV_Q_CAP;
    pthread_cond_signal(&dv_q_nonemp);
    pthread_mutex_unlock(&dv_q_mu);
    return 0;
}

static int dv_q_pop(uint8_t **data_out, size_t *size_out, int64_t *pts_out)
{
    pthread_mutex_lock(&dv_q_mu);
    while ((dv_q_is_empty_locked() || dv_q_paused) && !dv_q_stop) {
        pthread_cond_wait(&dv_q_nonemp, &dv_q_mu);
    }
    if (dv_q_stop && dv_q_is_empty_locked()) {
        pthread_mutex_unlock(&dv_q_mu);
        return -1;
    }
    *data_out = dv_q[dv_q_tail].data;
    *size_out = dv_q[dv_q_tail].size;
    *pts_out  = dv_q[dv_q_tail].pts_90k;
    dv_q_tail = (dv_q_tail + 1) % DV_Q_CAP;
    pthread_cond_signal(&dv_q_nonfull);
    pthread_mutex_unlock(&dv_q_mu);
    return 0;
}

static void dv_q_drain(void)
{
    pthread_mutex_lock(&dv_q_mu);
    while (!dv_q_is_empty_locked()) {
        free(dv_q[dv_q_tail].data);
        dv_q[dv_q_tail].data = NULL;
        dv_q_tail = (dv_q_tail + 1) % DV_Q_CAP;
    }
    dv_q_head = dv_q_tail = 0;
    pthread_cond_broadcast(&dv_q_nonfull);
    pthread_mutex_unlock(&dv_q_mu);
}

/* Periodic stats — sampled by dv_consumer_main. */
static uint64_t dv_st_submitted = 0;   /* SET_FRAME calls that returned ok */
static uint64_t dv_st_dropped   = 0;   /* video buffer clears after a decoder stall */
static uint64_t dv_st_eagain_pollin = 0;
static uint64_t dv_st_last_log_ns = 0;
static uint64_t dv_st_last_submitted = 0;
static uint64_t dv_st_last_dropped   = 0;

/* a tsync value, the PTS on screen or the system clock */
static uint64_t dv_tsync_read(const char *path)
{
    char buf[32];
    uint64_t pts = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            pts = strtoull(buf, NULL, 0);
        }
        close(fd);
    }
    return pts;
}

/* Take the pending events of the driver. At a PTS jump it drops every
 * write until a flush: flush at once, as enigma2 does on the DVB path.
 * VIDEO_GET_EVENT succeeds without an event and leaves the buffer alone. */
static void dv_take_events(int fd)
{
    int evt[8];
    int i;
    for (i = 0; i < 8; i++) {
        memset(evt, 0, sizeof(evt));
        if (ioctl(fd, VIDEO_GET_EVENT, evt) < 0 || !evt[0])
            return;
        if (evt[0] == DV_EVENT_DISCONTINUE) {
            DV_DBG("PTS jump, flushing the video buffer");
            (void)ioctl(fd, VIDEO_CLEAR_BUFFER);
            dv_divx3_header = 1;
            __atomic_store_n(&dv_flush_vpts, dv_tsync_read("/sys/class/tsync/pts_video"), __ATOMIC_RELAXED);
        }
    }
}

/* dreamvideosink (runtime strace):
 *   - öffnet video0 mit O_NONBLOCK
 *   - öffnet /dev/amvideo_poll als O_RDWR (hat es offen, polled aber selten)
 *   - macht VIDEO_GET_EVENT drain initial
 *   - poll't VIDEO_GET_PTS regelmäßig zwischen SET_FRAMEs
 *   - bei EAGAIN: wartet auf amvideo_poll. */
static int dv_submit_frame(int fd, const uint8_t *data, size_t size, int64_t pts_90k)
{
    struct video_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.pts = (pts_90k > 0) ? (uint64_t)pts_90k : 0;
    fr.bytes[0]        = (ssize_t)size;
    fr.data[0]         = data;
    fr.is_phys_addr[0] = 0;

    /* Drain a pending event so the kernel buffer-free path can fire. */
    dv_take_events(fd);

    struct pollfd pfds[2];
    pfds[0].fd = fd;             pfds[0].events = POLLIN;
    pfds[1].fd = dv_poll_fd;     pfds[1].events = POLLIN;
    int nfds = (dv_poll_fd >= 0) ? 2 : 1;

    /* Wait for the decoder instead of dropping: a full buffer is normal
     * backpressure, and a dropped H.264 frame breaks the reference chain
     * so the decoder resets on the frame number gap. Only a stop request
     * or a decoder whose PTS stands still for DV_STALL_MS while the
     * clock runs ends the wait. */
    struct timespec t0, t;
    uint64_t last_pts = 0, pcr0 = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t offset = 0;
    for (;;) {
        /* no writes into a frozen decoder: a write that keeps finding the
         * buffer full ends in the write timeout of the driver right after
         * the pause, and the driver drops every frame until a flush */
        if (__atomic_load_n(&dv_q_paused, __ATOMIC_RELAXED)) {
            if (__atomic_load_n(&dv_q_stop, __ATOMIC_RELAXED))
                return -1;
            usleep(10000);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            continue;
        }
        int rc = ioctl(fd, VIDEO_SET_FRAME, &fr);
        if (rc >= 0) {
            /* the driver takes what fits and returns the byte count; the
             * rest follows without a PTS once there is room */
            offset += (size_t)rc;
            if (offset >= size) {
                dv_st_submitted++;
                return 0;
            }
            fr.pts      = DV_PTS_NONE;
            fr.bytes[0] = (ssize_t)(size - offset);
            fr.data[0]  = data + offset;
        } else if (errno != EAGAIN) {
            DV_DBG("VIDEO_SET_FRAME (%zu bytes) failed: %s", size, strerror(errno));
            return -1;
        }
        if (__atomic_load_n(&dv_q_stop, __ATOMIC_RELAXED))
            return -1;
        int pr = poll(pfds, nfds, 50);
        if (pr > 0) {
            dv_st_eagain_pollin++;
            /* readable but still full: take the event, do not spin */
            dv_take_events(fd);
            usleep(5000);
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        /* VIDEO_GET_PTS returns a cached value while the buffer is full */
        uint64_t vpts = dv_tsync_read("/sys/class/tsync/pts_video");
        uint64_t pcr = dv_tsync_read("/sys/class/tsync/pts_pcrscr");
        if (vpts != last_pts || __atomic_load_n(&dv_q_paused, __ATOMIC_RELAXED)) {
            /* frames still change, or a pause */
            last_pts = vpts;
            pcr0 = pcr;
            t0 = t;
        }
        long waited_ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        /* a stall only counts while the clock runs on */
        if (waited_ms > DV_STALL_MS && (int64_t)(pcr - pcr0) > DV_STALL_MS * 45LL) {
            DV_DBG("decoder stalled, video PTS stood still for %ld ms, clearing the video buffer", waited_ms);
            (void)ioctl(fd, VIDEO_CLEAR_BUFFER);
            __atomic_store_n(&dv_flush_vpts, dv_tsync_read("/sys/class/tsync/pts_video"), __ATOMIC_RELAXED);
            dv_st_dropped++;
            return 0;
        }
    }
}

static uint64_t dv_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Hold a frame until it is at most DV_LEAD_90K ahead of the frame on
 * screen. A write that waits long for room ends in a write timeout, after
 * which the driver drops every frame until a flush. The system clock lags
 * behind a seek, the PTS on screen does not. Free until the decoder shows
 * frames after a flush, and while it stands without a pause, the stall
 * check in dv_submit_frame handles that. */
static void dv_wait_lead(int64_t pts_90k)
{
    static uint64_t last_vpts = 0;  /* consumer thread only */
    static int still_ms = 0;
    while (!__atomic_load_n(&dv_q_stop, __ATOMIC_RELAXED)) {
        if (!__atomic_load_n(&dv_q_paused, __ATOMIC_RELAXED)) {
            uint64_t vpts = dv_tsync_read("/sys/class/tsync/pts_video");
            if (pts_90k < 0 || vpts == __atomic_load_n(&dv_flush_vpts, __ATOMIC_RELAXED))
                return;
            int32_t lead = (int32_t)((uint32_t)pts_90k - (uint32_t)vpts);
            /* far off is a jump, the decoder sorts it out */
            if (lead <= DV_LEAD_90K || lead > 10 * 90000)
                return;
            still_ms = (vpts == last_vpts) ? still_ms + 10 : 0;
            if (still_ms > DV_STALL_MS)
                return;
            last_vpts = vpts;
        }
        usleep(10000);
    }
}

/* Submit frames up to DV_LEAD_90K ahead of the frame on screen;
 * backpressure comes from dv_wait_lead and dv_submit_frame. */
static void *dv_consumer_main(void *arg)
{
    char tn[16] = "dv_consumer";
    (void)arg;
    prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);

    int64_t  pts_90k_mono = 0;     /* fallback when stream PTS missing */
    dv_st_last_log_ns = dv_now_ns();
    while (!dv_q_stop) {
        uint8_t *buf = NULL;
        size_t   sz  = 0;
        int64_t  pts_container = -1;
        if (dv_q_pop(&buf, &sz, &pts_container) < 0) break;
        pthread_mutex_lock(&dv_q_mu);
        int qd = dv_q_depth_locked();
        pthread_mutex_unlock(&dv_q_mu);

        /* the stream PTS, the kernel runs the system clock from it */
        int64_t fr_pts = pts_container;
        if (fr_pts < 0)
            fr_pts = pts_90k_mono;

        dv_wait_lead(fr_pts);
        if (dv_fd >= 0) dv_submit_frame(dv_fd, buf, sz, fr_pts);
        free(buf);
        pts_90k_mono += (int64_t)(dv_frame_dur_ns * 90 / 1000000);
        uint64_t now = dv_now_ns();

        if (now - dv_st_last_log_ns > 5000000000ULL) {
            uint64_t s_delta = dv_st_submitted - dv_st_last_submitted;
            uint64_t d_delta = dv_st_dropped   - dv_st_last_dropped;
            double secs = (double)(now - dv_st_last_log_ns) / 1e9;
            DV_DBG("stats: submitted=%llu (+%llu, %.1f/s) dropped=%llu (+%llu, %.1f/s) qdepth=%d lead=%dms frame_dur=%llums pts_inc=%lld",
                   (unsigned long long)dv_st_submitted, (unsigned long long)s_delta, s_delta/secs,
                   (unsigned long long)dv_st_dropped, (unsigned long long)d_delta, d_delta/secs,
                   qd, (int32_t)((uint32_t)fr_pts - (uint32_t)dv_tsync_read("/sys/class/tsync/pts_video")) / 90,
                   (unsigned long long)(dv_frame_dur_ns/1000000ULL),
                   (long long)(dv_frame_dur_ns * 90LL / 1000000LL));
            dv_st_last_submitted = dv_st_submitted;
            dv_st_last_dropped   = dv_st_dropped;
            dv_st_last_log_ns    = now;
        }
    }
    return NULL;
}

static void dv_q_start(void)
{
    if (dv_q_running) return;
    dv_q_stop    = 0;
    dv_q_paused  = 0;
    dv_q_head    = dv_q_tail = 0;
    dv_flush_vpts = dv_tsync_read("/sys/class/tsync/pts_video");
    if (pthread_create(&dv_q_thread, NULL, dv_consumer_main, NULL) == 0) {
        dv_q_running = 1;
    } else {
        DV_DBG("pthread_create consumer failed: %s", strerror(errno));
    }
}

static void dv_q_shutdown(void)
{
    if (!dv_q_running) return;
    pthread_mutex_lock(&dv_q_mu);
    dv_q_stop = 1;
    pthread_cond_broadcast(&dv_q_nonemp);
    pthread_cond_broadcast(&dv_q_nonfull);
    pthread_mutex_unlock(&dv_q_mu);
    pthread_join(dv_q_thread, NULL);
    dv_q_running = 0;
    dv_q_drain();
}

/* ----- Output_t handlers ---------------------------------------------- */

static int DreamVideoOpen(Context_t *context, char *type)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd < 0) {
        /* dreamvideosink öffnet video0 O_NONBLOCK + amvideo_poll O_RDWR. */
        dv_fd = open(VIDEO_DEV, O_RDWR | O_NONBLOCK);
        if (dv_fd < 0) {
            DV_DBG("open %s failed: %s", VIDEO_DEV, strerror(errno));
            pthread_mutex_unlock(&dv_mutex);
            return cERR_DREAMVIDEO_ERROR;
        }
        dv_q_start();
        dv_poll_fd = open(AMPOLL_DEV, O_RDWR | O_NONBLOCK);
        if (dv_poll_fd < 0)
            DV_DBG("open %s failed: %s", AMPOLL_DEV, strerror(errno));
        dv_enable_display();

        /* Reset state from any previous (e.g. dreamvideosink) session. */
        (void)ioctl(dv_fd, VIDEO_STOP, (void *)(uintptr_t)0);
        (void)ioctl(dv_fd, VIDEO_CLEAR_BUFFER);

        if (ioctl(dv_fd, VIDEO_SELECT_SOURCE, (void *)(uintptr_t)VIDEO_SOURCE_MEMORY) < 0)
            DV_DBG("SELECT_SOURCE_MEMORY failed: %s", strerror(errno));
        if (ioctl(dv_fd, VIDEO_FREEZE) < 0)
            DV_DBG("FREEZE failed: %s", strerror(errno));
    }
    dv_inited = 0;
    dv_frame_dur_ns = 20000000ULL;
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoClose(Context_t *context, char *type)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;

    /* Shut down the consumer thread before closing the fd. */
    dv_q_shutdown();
    pthread_mutex_lock(&dv_mutex);
    if (dv_fd >= 0) {
        /* Return decoder to DEMUX/stopped for the next consumer. */
        ioctl(dv_fd, VIDEO_STOP, (void *)(uintptr_t)0);
        ioctl(dv_fd, VIDEO_SELECT_SOURCE, (void *)(uintptr_t)VIDEO_SOURCE_DEMUX);
        close(dv_fd);
        dv_fd = -1;
    }
    if (dv_poll_fd >= 0) {
        close(dv_poll_fd);
        dv_poll_fd = -1;
    }
    dv_inited = 0;
    dv_current_pts = 0;
    pthread_mutex_unlock(&dv_mutex);
    DV_DBG("closed");
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoPlay(Context_t *context, char *type)
{
    /* Actual VIDEO_PLAY happens after SET_DEC_SYSINFO in dv_lazy_init. */
    return cERR_DREAMVIDEO_NO_ERROR;
}

/* the audio pause holds the system clock (dreamaudio device), the
 * consumer holds the queued frames */
static int DreamVideoPause(char *type, int pause)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;
    pthread_mutex_lock(&dv_q_mu);
    dv_q_paused = pause;
    pthread_cond_broadcast(&dv_q_nonemp);
    pthread_mutex_unlock(&dv_q_mu);
    pthread_mutex_lock(&dv_mutex);
    if (dv_fd >= 0 && ioctl(dv_fd, pause ? VIDEO_FREEZE : VIDEO_CONTINUE, (void *)(uintptr_t)0) < 0)
        DV_DBG("%s failed: %s", pause ? "VIDEO_FREEZE" : "VIDEO_CONTINUE", strerror(errno));
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoStop(Context_t *context, char *type)
{
    return DreamVideoClose(context, type);
}

/* the frame on screen; the queue runs seconds ahead of it */
static int DreamVideoPts(Context_t *context, unsigned long long *pts)
{
    uint64_t vp = 0;

    pthread_mutex_lock(&dv_mutex);
    /* the frame on screen; the last written frame runs far ahead of it on a
     * fast source, and VIDEO_GET_PTS gives nothing for a while after a pause */
    if (dv_fd < 0 || ioctl(dv_fd, VIDEO_GET_PTS, &vp) != 0 || !vp)
        vp = dv_tsync_read("/sys/class/tsync/pts_video");
    *pts = vp ? vp : dv_current_pts;
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

/* ----- Lazy init (first packet has width/height/fps) ------------------ */

static int dv_lazy_init(Context_t *context, AudioVideoOut_t *out)
{
    if (dv_inited) return 0;

    uint32_t width = out->width, height = out->height;
    char *Encoding = NULL;
    context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
    Writer_t *writer = getWriter(Encoding);
    free(Encoding);
    if (!writer) return -1;

    /* the demuxer has no size for AVS+, the stream has */
    if ((!width || !height) && writer->caps->dvbStreamType == STREAMTYPE_AVS &&
        dv_avs_size(out->extradata, out->extralen, &width, &height) < 0 &&
        dv_avs_size(out->data, out->len, &width, &height) < 0)
        return 0;
    if (!width || !height) return 0;

    uint32_t dec_format = 0;
    int streamtype = 0;
    if (dv_decformat_for(writer->caps->dvbStreamType, &dec_format) < 0 ||
        dv_streamtype_for(writer->caps->dvbStreamType, &streamtype)  < 0) {
        DV_DBG("no codec mapping for streamtype %d", writer->caps->dvbStreamType);
        return -1;
    }

    struct dec_sysinfo si;
    memset(&si, 0, sizeof(si));
    si.format = dec_format;
    si.width  = width;
    si.height = height;
    /* rate = frame duration in 96000-tick units (out->frameRate is milli-fps). */
    if (out->frameRate > 0) {
        si.rate = (uint32_t)((uint64_t)96000ULL * 1000U / out->frameRate);
        dv_frame_dur_ns = (uint64_t)1000000000ULL * 1000ULL / (uint64_t)out->frameRate;
    } else {
        si.rate = 3200;
        dv_frame_dur_ns = 33333333ULL;     /* ~30 fps fallback */
    }
    /* param must stay NULL — non-zero values wedge the decoder. */
    si.param = NULL;

    DV_DBG("lazy_init: w=%u h=%u out->frameRate(milli-fps)=%u → si.rate=%u(96k/frame) dv_frame_dur=%llums dec_format=%u streamtype=%d",
           width, height, out->frameRate, si.rate,
           (unsigned long long)(dv_frame_dur_ns/1000000ULL), dec_format, streamtype);
    if (ioctl(dv_fd, VIDEO_SET_DEC_SYSINFO, &si) < 0) {
        DV_DBG("VIDEO_SET_DEC_SYSINFO failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(dv_fd, VIDEO_SET_STREAMTYPE, (void *)(uintptr_t)streamtype) < 0) {
        DV_DBG("VIDEO_SET_STREAMTYPE %d failed: %s", streamtype, strerror(errno));
        return -1;
    }
    if (ioctl(dv_fd, VIDEO_PLAY) < 0)
        DV_DBG("VIDEO_PLAY failed: %s", strerror(errno));
    if (ioctl(dv_fd, VIDEO_SLOWMOTION, (void *)(uintptr_t)0) < 0)
        DV_DBG("VIDEO_SLOWMOTION failed: %s", strerror(errno));
    if (ioctl(dv_fd, VIDEO_FAST_FORWARD, (void *)(uintptr_t)0) < 0)
        DV_DBG("VIDEO_FAST_FORWARD failed: %s", strerror(errno));
    if (ioctl(dv_fd, VIDEO_CONTINUE, (void *)(uintptr_t)5) < 0)
        DV_DBG("VIDEO_CONTINUE failed: %s", strerror(errno));

    dv_inited = 1;
    return 0;
}

/* ----- WriteV hook: hand iovec off to the consumer thread ------------- */

static ssize_t dv_set_frame_writev(int fd, const struct iovec *iov, int iovcnt)
{
    (void)fd;

    /* Coalesce iovec and hand off to the consumer thread. */
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len;
    if (total == 0) return 0;

    uint8_t *buf = malloc(total);
    if (!buf) {
        DV_DBG("malloc(%zu) failed: %s", total, strerror(errno));
        return -1;
    }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }

    /* Snapshot the per-call PTS DreamVideoWrite cached for this frame
     * so the consumer thread can tag fr.pts properly. */
    int64_t pts_90k = (int64_t)dv_current_pts;
    /* Push either succeeded (consumer owns buf) or queue full/stopping
     * (dv_q_push freed buf). Report success either way. */
    (void)dv_q_push(buf, total, pts_90k);
    return (ssize_t)total;
}

/* ----- VP9 ------------------------------------------------------------ */

static void dv_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* The Amlogic VP9 decoder takes each frame behind an AMLV header: length,
 * inverted length, start code and signature. A superframe is split into
 * its frames, its index is dropped. */
static int dv_write_vp9(const uint8_t *data, size_t size)
{
    size_t sizes[8] = { size };
    size_t count = 1, index_size = 0;
    uint8_t marker = data[size - 1];

    if ((marker & 0xe0) == 0xc0) {
        size_t n = (marker & 7) + 1, bytes = ((marker >> 3) & 3) + 1;
        size_t isz = 2 + n * bytes;
        if (isz <= size && data[size - isz] == marker) {
            size_t pos = size - isz + 1, total = 0;
            for (size_t i = 0; i < n; i++) {
                sizes[i] = 0;
                for (size_t b = 0; b < bytes; b++)
                    sizes[i] |= (size_t)data[pos++] << (8 * b);
                total += sizes[i];
            }
            if (total == size - isz) {
                count = n;
                index_size = isz;
            } else {
                sizes[0] = size;
            }
        }
    }

    size_t total = size - index_size + count * 16;
    uint8_t *buf = malloc(total);
    if (!buf) {
        DV_DBG("malloc(%zu) failed: %s", total, strerror(errno));
        return -1;
    }
    size_t in = 0, out = 0;
    for (size_t i = 0; i < count; i++) {
        dv_put_be32(buf + out, (uint32_t)sizes[i] + 4);
        dv_put_be32(buf + out + 4, ~((uint32_t)sizes[i] + 4));
        dv_put_be32(buf + out + 8, 1);
        memcpy(buf + out + 12, "AMLV", 4);
        memcpy(buf + out + 16, data + in, sizes[i]);
        out += sizes[i] + 16;
        in += sizes[i];
    }
    (void)dv_q_push(buf, total, (int64_t)dv_current_pts);
    return 0;
}

/* ----- DivX 3.11 ------------------------------------------------------ */

/* The Amlogic decoder takes each DivX 3.11 frame behind a start code with
 * the signature and the frame length. */
static int dv_write_divx3(const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
    size_t head = dv_divx3_header ? 10 : 0;
    size_t total = head + 13 + 4 + size;
    uint8_t *buf = malloc(total);
    if (!buf) {
        DV_DBG("malloc(%zu) failed: %s", total, strerror(errno));
        return -1;
    }
    if (head) {
        uint32_t dimensions = width << 12 | height;
        dv_put_be32(buf, 1);
        buf[4] = 0x20;
        buf[5] = dimensions >> 16;
        buf[6] = dimensions >> 8;
        buf[7] = dimensions;
        buf[8] = 0;
        buf[9] = 0;
        dv_divx3_header = 0;
    }
    memcpy(buf + head, "\x00\x00\x00\x01\xb6" "DIVX3.11", 13);
    dv_put_be32(buf + head + 13, (uint32_t)size);
    memcpy(buf + head + 17, data, size);
    (void)dv_q_push(buf, total, (int64_t)dv_current_pts);
    return 0;
}

/* ----- Write --------------------------------------------------------- */

static int DreamVideoWrite(void *_context, void *_out)
{
    Context_t       *context = (Context_t *)_context;
    AudioVideoOut_t *out     = (AudioVideoOut_t *)_out;

    if (!out || !out->data || out->len == 0) return cERR_DREAMVIDEO_NO_ERROR;
    if (out->type && strcmp(out->type, "video") != 0)
        return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd < 0) { pthread_mutex_unlock(&dv_mutex); return cERR_DREAMVIDEO_ERROR; }

    if (dv_lazy_init(context, out) < 0) {
        pthread_mutex_unlock(&dv_mutex);
        return cERR_DREAMVIDEO_ERROR;
    }
    /* Keep dv_frame_dur_ns aligned with the actual source rate even when
     * lazy_init didn't run (first frames sometimes arrive with w/h=0).
     * Without this the consumer keeps its 50fps init pacing and the
     * kernel decoder swamps the display pipeline (input_fps=50 for a
     * 25fps source → 4-15 frames/sec dropped, visible stutter). */
    if (out->frameRate > 0)
        dv_frame_dur_ns = (uint64_t)1000000000ULL * 1000ULL / (uint64_t)out->frameRate;

    /* Cached for OUTPUT_PTS; the writev hook itself always feeds the
     * decoder pts=0 (see dv_submit_frame). */
    if (out->pts != (int64_t)INVALID_PTS_VALUE && out->pts >= 0)
        dv_current_pts = (unsigned long long)out->pts;

    char *Encoding = NULL;
    context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
    Writer_t *writer = getWriter(Encoding);
    free(Encoding);

    if (!writer || !writer->writeData) {
        pthread_mutex_unlock(&dv_mutex);
        return cERR_DREAMVIDEO_ERROR;
    }
    /* the queue may wait for room; position, pause and flush must not */
    pthread_mutex_unlock(&dv_mutex);

    /* the VP9 writer packs for Broadcom decoders */
    if (writer->caps && writer->caps->dvbStreamType == STREAMTYPE_VB9)
        return dv_write_vp9(out->data, out->len) < 0 ? cERR_DREAMVIDEO_ERROR : cERR_DREAMVIDEO_NO_ERROR;
    if (writer->caps && writer->caps->dvbStreamType == STREAMTYPE_DIVX311)
        return dv_write_divx3(out->data, out->len, out->width, out->height) < 0 ? cERR_DREAMVIDEO_ERROR : cERR_DREAMVIDEO_NO_ERROR;

    WriterAVCallData_t call;
    memset(&call, 0, sizeof(call));
    call.fd           = dv_fd;
    call.data         = out->data;
    call.len          = out->len;
    call.Pts          = out->pts;
    call.Dts          = out->dts;
    call.private_data = out->extradata;
    call.private_size = out->extralen;
    call.FrameRate    = out->frameRate;
    call.FrameScale   = out->timeScale;
    call.Width        = out->width;
    call.Height       = out->height;
    call.InfoFlags    = out->infoFlags;
    call.Version      = 0;
    call.WriteV       = dv_set_frame_writev;

    int res = writer->writeData(&call);

    if (res < 0) {
        DV_DBG("writeData %u bytes failed: %s", out->len, strerror(errno));
        return cERR_DREAMVIDEO_ERROR;
    }
    return cERR_DREAMVIDEO_NO_ERROR;
}

/* ----- Command dispatch ----------------------------------------------- */

static int DreamVideoCommand(void *_context, OutputCmd_t cmd, void *arg)
{
    Context_t *context = (Context_t *)_context;

    switch (cmd) {
    case OUTPUT_OPEN:    return DreamVideoOpen(context, (char *)arg);
    case OUTPUT_CLOSE:   return DreamVideoClose(context, (char *)arg);
    case OUTPUT_PLAY:    return DreamVideoPlay(context, (char *)arg);
    case OUTPUT_STOP:    return DreamVideoStop(context, (char *)arg);
    case OUTPUT_PTS:     return DreamVideoPts(context, (unsigned long long *)arg);
    case OUTPUT_FLUSH:
    case OUTPUT_CLEAR:
        pthread_mutex_lock(&dv_mutex);
        dv_q_drain();
        if (dv_fd >= 0) (void)ioctl(dv_fd, VIDEO_CLEAR_BUFFER);
        dv_divx3_header = 1;
        __atomic_store_n(&dv_flush_vpts, dv_tsync_read("/sys/class/tsync/pts_video"), __ATOMIC_RELAXED);
        pthread_mutex_unlock(&dv_mutex);
        return cERR_DREAMVIDEO_NO_ERROR;
    case OUTPUT_PAUSE:    return DreamVideoPause((char *)arg, 1);
    case OUTPUT_CONTINUE: return DreamVideoPause((char *)arg, 0);
    case OUTPUT_SWITCH:
    case OUTPUT_AVSYNC:
    case OUTPUT_SLOWMOTION:
    case OUTPUT_AUDIOMUTE:
    case OUTPUT_FASTFORWARD:
    case OUTPUT_REVERSE:
    case OUTPUT_DISCONTINUITY_REVERSE:
    case OUTPUT_GET_FRAME_COUNT:
    case OUTPUT_GET_PROGRESSIVE:
    case OUTPUT_SET_BUFFER_SIZE:
    case OUTPUT_GET_BUFFER_SIZE:
        return cERR_DREAMVIDEO_NO_ERROR;
    default:
        return cERR_DREAMVIDEO_NO_ERROR;
    }
}

static char *DreamVideoCapabilities[] = { "video", NULL };

struct Output_s DreamVideoOutput = {
    "DreamVideo",
    &DreamVideoCommand,
    &DreamVideoWrite,
    DreamVideoCapabilities
};
