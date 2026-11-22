/*
 * eplayer3: command line playback using libeplayer3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <inttypes.h>
#include <stdarg.h>
#include <time.h>
#include "pcm_export.h"
#include "ca_bundle.h"

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include <pthread.h>

#include "strbuffer.h"
#include "common.h"
#include "misc.h"

#include "debug.h"

#define DUMP_BOOL(x) 0 == x ? "false"  : "true"

extern int ffmpeg_av_dict_set(const char *key, const char *value, int flags);
extern int32_t ffmpeg_drm_key_add(const char *pair);
extern int32_t ffmpeg_mpd_url_option(char digit, const char *value, size_t len);
extern int32_t ffmpeg_drm_key_command(const char *line);
extern int32_t ffmpeg_drm_key_caps(void);
extern void ffmpeg_drm_key_request_set(int32_t enable);
extern void ffmpeg_drm_key_timeout_set(int32_t seconds);
extern int32_t ffmpeg_drm_key_audio_switch(void);
extern int32_t ffmpeg_drm_key_stop(void);
extern int  container_ffmpeg_audio_info_gen(void);
extern int  container_ffmpeg_subtitle_info_gen(void);
extern int  container_ffmpeg_audio_live_info(int32_t track_id, int *channels, const char **description,
                                              AudioStreamInfo_t *info);
extern int  container_ffmpeg_video_live_info(int32_t track_id, VideoStreamInfo_t *info);
extern void       aac_software_decoder_set(const int32_t val);
extern void pcm_audio_export_set(int enabled);
extern void  aac_latm_software_decoder_set(const int32_t val);
extern void           aac_pts_dejitter_set(const int32_t val);
extern void       dts_software_decoder_set(const int32_t val);
extern void       wma_software_decoder_set(const int32_t val);
extern void       ac3_software_decoder_set(const int32_t val);
extern void      eac3_software_decoder_set(const int32_t val);
extern void       mp3_software_decoder_set(const int32_t val);
extern void       amr_software_decoder_set(const int32_t val);
extern void    vorbis_software_decoder_set(const int32_t val);
extern void      opus_software_decoder_set(const int32_t val);
extern void         dts_ac3_transcoder_set(const int32_t val);
extern void      truehd_ac3_transcoder_set(const int32_t val);
extern void          aac_he_transcoder_set(const int32_t val);
extern int32_t        audio_transcode_caps(void);
extern int32_t            hls_segment_caps(void);

extern void            rtmp_proto_impl_set(const int32_t val);
extern void        flv2mpeg4_converter_set(const int32_t val);
extern void        sel_program_id_set(const int32_t val);
extern void adaptive_quality_set(int32_t mode, int32_t default_audio);
extern void h264_resolution_limit_set(int32_t width, int32_t height);
extern void start_position_set(int64_t us);

extern void pcm_resampling_set(int32_t val);
extern void stereo_software_decoder_set(int32_t val);
extern void pcm_audio_channel_set(int32_t val);
extern void insert_pcm_as_lpcm_set(int32_t val);
extern void progressive_playback_set(int32_t val);

extern OutputHandler_t         OutputHandler;
extern PlaybackHandler_t       PlaybackHandler;
extern ContainerHandler_t      ContainerHandler;
extern ManagerHandler_t        ManagerHandler;

static Context_t *g_player = NULL;

/* Dual mono (two languages on L and R): left, stereo or right, the values of
 * enigma2's iAudioChannelSelection. Stdin "am<n>", --audio-channel. */
static int g_audio_channel = OUTPUT_AUDIO_CHANNEL_STEREO;

static int ParseAudioChannel(const char *text)
{
    if (!strcmp(text, "0") || !strcmp(text, "left"))
        return OUTPUT_AUDIO_CHANNEL_LEFT;
    if (!strcmp(text, "1") || !strcmp(text, "stereo"))
        return OUTPUT_AUDIO_CHANNEL_STEREO;
    if (!strcmp(text, "2") || !strcmp(text, "right"))
        return OUTPUT_AUDIO_CHANNEL_RIGHT;
    return -1;
}

/* The decoder of the box (AUDIO_CHANNEL_SELECT) and the software decoded PCM;
 * the result is the one of the decoder. */
static int SelectAudioChannel(int channel)
{
    g_audio_channel = channel;
    pcm_audio_channel_set(channel);
    return g_player->output->Command(g_player, OUTPUT_AUDIO_CHANNEL, &channel);
}

static void TerminateAllSockets(void)
{
    int i;
    for(i=0; i<1024; ++i)
    {
        if( 0 == shutdown(i, SHUT_RDWR) )
        {
            /* yes, I know that this is not good practice and I know what this could cause 
             * but in this use case it can be accepted. 
             * We must close socket because without closing it recv will return 0 (after shutdown)
             * 0 is not correctly handled by external libraries
             */
            close(i);
        }
    }
}

static int g_pfd[2] = {-1, -1}; /* Used to wake terminate thread and kbhit */
static int isPlaybackStarted = 0;
static pthread_mutex_t playbackStartMtx;

static int32_t g_windows_width = 1280;
static int32_t g_windows_height = 720;
static char *g_graphic_sub_path;
static int tracks_cache_disabled = 0;
static char *g_ca_file; /* -f ca_file=... */

const char* GetGraphicSubPath()
{
    return g_graphic_sub_path;
}

int32_t GetGraphicWindowWidth()
{
    return g_windows_width;
}

int32_t GetGraphicWindowHeight()
{
    return g_windows_height;
}

void E2iSendMsg(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* enigma2 follows the video of players outside its services here, e.g. for
 * the automatic resolution. An empty path turns it off. */
#ifndef EXTEPLAYER_VIDEO_SOCKET
#define EXTEPLAYER_VIDEO_SOCKET "/var/run/video.socket"
#endif

static void SendExternalVideo(const char *msg)
{
    static int fd = -1;
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, EXTEPLAYER_VIDEO_SOCKET, sizeof(addr.sun_path) - 1);
    if (!addr.sun_path[0])
    {
        return;
    }
    if (fd < 0)
    {
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            return;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    /* without enigma2 listening it goes nowhere */
    (void)sendto(fd, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof(addr));
}

/* Length of the well-formed UTF-8 sequence (2..4 bytes, no overlong form,
 * no surrogate, at most U+10FFFF) at p, 0 when there is none. Stops at the
 * terminating NUL. */
static size_t Utf8SequenceLength(const unsigned char *p)
{
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    size_t len;
    size_t i;

    if (p[0] >= 0xC2 && p[0] <= 0xDF)
    {
        len = 2;
    }
    else if (p[0] >= 0xE0 && p[0] <= 0xEF)
    {
        len = 3;
        if (p[0] == 0xE0)
            lo = 0xA0;
        else if (p[0] == 0xED)
            hi = 0x9F;
    }
    else if (p[0] >= 0xF0 && p[0] <= 0xF4)
    {
        len = 4;
        if (p[0] == 0xF0)
            lo = 0x90;
        else if (p[0] == 0xF4)
            hi = 0x8F;
    }
    else
    {
        return 0;
    }

    if (p[1] < lo || p[1] > hi)
    {
        return 0;
    }
    for (i = 2; i < len; ++i)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            return 0;
        }
    }
    return len;
}

/* Writes src into dst as the content of a JSON string: '"', '\\' and
 * control characters are escaped. A byte from 0x80 on that is not part of
 * a well-formed UTF-8 sequence (a Latin-1 tag) is written as \u00xx, so
 * the line stays valid UTF-8. What does not fit into size is cut off
 * between two characters, never inside an escape or a UTF-8 sequence.
 * The caller owns the buffer, the messages come from more than one thread. */
static const char *JsonEscape(char *dst, size_t size, const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t pos = 0;

    if (0 == size)
    {
        return "";
    }

    while (p && *p)
    {
        size_t in = 1;
        size_t out = 1;

        if (*p == '"' || *p == '\\')
        {
            out = 2;
        }
        else if (*p < 0x20)
        {
            out = 6;
        }
        else if (*p >= 0x80)
        {
            in = Utf8SequenceLength(p);
            if (in)
            {
                out = in;
            }
            else
            {
                in = 1;
                out = 6;
            }
        }

        if (pos + out >= size)
        {
            break;
        }

        if (out == 6)
        {
            snprintf(dst + pos, size - pos, "\\u%04x", *p);
        }
        else if (out == 2 && in == 1)
        {
            dst[pos] = '\\';
            dst[pos + 1] = (char)*p;
        }
        else
        {
            memcpy(dst + pos, p, in);
        }
        pos += out;
        p += in;
    }
    dst[pos] = '\0';
    return dst;
}

void E2iStartMsg(void)
{
    flockfile(stderr);
}

void E2iEndMsg(void)
{
    funlockfile(stderr);
}


static void TerminateWakeUp()
{
    int ret = write(g_pfd[1], "x", 1);
    if (ret != 1) {
        printf("TerminateWakeUp write return %d\n", ret);
    }
}

/* SIGINT, SIGTERM and SIGHUP stop the player the way the q command does:
 * PLAYBACK_STOP, then OUTPUT_CLOSE, so the decoders are stopped and the
 * devices released. The signals are blocked in every thread and taken by
 * this one; the command loop reads the request as q. A second signal, or a
 * stop that takes longer than STOP_SIGNAL_TIMEOUT seconds, ends the process
 * at once.
 * PR_SET_PDEATHSIG belongs to the thread that started the player, not to its
 * process: it comes when that thread ends, while the process that reads the
 * player may still be there. So the parent death signal is SIGUSR2, which
 * stops only when getppid() has changed, and getppid() is also checked every
 * second, which finds a parent that is gone without a signal as well. */
#define PARENT_DEATH_SIGNAL SIGUSR2
#define STOP_SIGNAL_TIMEOUT 5

static volatile sig_atomic_t g_stopSignal = 0;
static pid_t g_parentPid = 0;
static sigset_t g_stopSignals;

static void StopSignalsBlock(void)
{
    sigemptyset(&g_stopSignals);
    sigaddset(&g_stopSignals, SIGINT);
    sigaddset(&g_stopSignals, SIGTERM);
    sigaddset(&g_stopSignals, SIGHUP);
    sigaddset(&g_stopSignals, PARENT_DEATH_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &g_stopSignals, NULL);
    /* a front end that is gone is found through the end of stdin */
    signal(SIGPIPE, SIG_IGN);
    g_parentPid = getppid();
}

static void *StopSignalThreadFun(void *arg)
{
    struct timespec second = {1, 0};
    int waited = 0;
    (void)arg;

    for (;;)
    {
        int sig = sigtimedwait(&g_stopSignals, NULL, &second);
        if (sig < 0)
        {
            if (errno != EAGAIN)
                continue;
            if (g_stopSignal)
            {
                if (++waited >= STOP_SIGNAL_TIMEOUT)
                {
                    printf("Stop after signal %d takes too long, exit now\n", (int)g_stopSignal);
                    _exit(128 + g_stopSignal);
                }
                continue;
            }
            if (getppid() == g_parentPid)
                continue;
            printf("Parent process is gone, stopping\n");
            sig = SIGTERM;
        }
        else if (sig == PARENT_DEATH_SIGNAL)
        {
            if (getppid() == g_parentPid)
                continue; /* only the thread that started the player has ended */
            printf("Parent process is gone, stopping\n");
            sig = SIGTERM;
        }
        else if (g_stopSignal)
        {
            printf("Second signal %d, exit now\n", sig);
            _exit(128 + sig);
        }
        else
        {
            printf("Signal %d, stopping\n", sig);
        }
        if (!g_stopSignal)
        {
            g_stopSignal = sig;
            TerminateWakeUp();
        }
    }
    return NULL;
}

static void *TermThreadFun(void *arg)
{
#ifndef EXTEPLAYER_TERM_SOCKET
#define EXTEPLAYER_TERM_SOCKET "/tmp/.exteplayerterm.socket"
#endif
    const char *socket_path = EXTEPLAYER_TERM_SOCKET;
    if (!socket_path[0])
        return NULL;
    struct sockaddr_un addr;
    int fd = -1;
    int cl = -1;
    int nfds = 1;
    fd_set readfds;
    
    unlink(socket_path);
    if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) 
    {
        perror("TermThreadFun socket error");
        goto finish;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) 
    {
        perror("TermThreadFun bind error");
        goto finish;
    }

    if (listen(fd, 1) == -1) 
    {
        perror("TermThreadFun listen error");
        goto finish;
    }

    FD_ZERO(&readfds);
    FD_SET(g_pfd[0], &readfds);
    FD_SET(fd, &readfds);
    
    nfds = fd > g_pfd[0] ? fd + 1 : g_pfd[0] + 1;
    
    while (select(nfds, &readfds, NULL, NULL, NULL) == -1 
           && errno == EINTR)
    {
        /* Restart if interrupted by signal */
    }
    
    if (FD_ISSET(fd, &readfds))
    {
        pthread_mutex_lock(&playbackStartMtx);
        PlaybackDieNow(1);
        if (isPlaybackStarted)
            TerminateAllSockets();
        else
            kill(getpid(), SIGINT);
        pthread_mutex_unlock(&playbackStartMtx);
    }

finish:
    close(cl);
    close(fd);
    pthread_exit(NULL);

}

/* OE-A compatibility (the OE-Alliance exteplayer3, getParamMPD()): front ends
 * append options to a DASH url after ".mpd":
 *   &0=<n> video representation index, &1=<n> audio representation index,
 *   &5=<key> a key for every KID, &6=<key> for the video, &7=<key> for the
 *   audio, &8=<x> a front end parameter.
 * Each "&<digit>=" with one of these digits after the first ".mpd" is taken
 * up to the next "&" (or the end) and cut out of the url; the rest of the
 * url, also a query string before or after it, stays as it is. The keys are
 * default keys, --key and dk keys win (ffmpeg_mpd_url_option()). The per-KID
 * way is --key and --key-request. */
#define MPD_URL_OPTION_MAX 256

static void mpd_url_options(char *url)
{
    char *p = strstr(url, ".mpd");

    if (!p)
    {
        return;
    }
    p += 4;
    while (NULL != (p = strchr(p, '&')))
    {
        char digit = p[1];
        if ('\0' != digit && NULL != strchr("015678", digit) && '=' == p[2])
        {
            char *value = p + 3;
            size_t len = strcspn(value, "&\n");
            if (len > MPD_URL_OPTION_MAX)
            {
                printf("url option &%c= of %u characters: too long, ignored\n", digit, (unsigned)len);
            }
            else
            {
                ffmpeg_mpd_url_option(digit, value, len);
            }
            /* cut out, p is then at the next "&" or the end */
            memmove(p, value + len, strlen(value + len) + 1);
        }
        else
        {
            p++;
        }
    }
}

static char *map_inter_file_path(const char *filename)
{
    char *filename2 = NULL;

    if( strncmp(filename, "iptv://", 7) == 0 )
    {
        FILE *f = fopen(filename + 7, "r");
        if( f != NULL )
        {
            long file_size = -1;
            if (0 == fseek(f, 0L, SEEK_END))
            {
                file_size = ftell(f);
            }
            rewind(f);

            if (file_size >= 0)
            {
                filename2 = malloc((size_t)file_size + 2);
            }
            if (!filename2)
            {
                fclose(f);
                return NULL;
            }
            size_t num = fread(filename2, 1, (size_t)file_size, f);
            fclose(f);

            if (num > 0 && filename2[num-1] == '\n')
            {
                filename2[num-1] = '\0';
            }
            else
            {
                filename2[num] = '\0';
            }
            filename = filename2;
        }
        else
        {
            return NULL;
        }
    }

    size_t filename_len = strlen(filename);
    const char *filname_prefix = "";

    if( strstr(filename, "://") == NULL )
    {
        filname_prefix = "file://";
        filename_len += strlen(filname_prefix);
    }

    char *ret = malloc(filename_len+2);
    if (ret)
    {
        strcpy(ret, filname_prefix);
        strcat(ret, filename);
        mpd_url_options(ret);
    }

    if( filename2 )
    {
        free(filename2);
    }

    return ret;
}

/**
 * This is workaround for images where ServiceApp doesn't support suburi parameter used to pass second (audio) file for playback.
 * We will handle suburi parameter here, so this will work with any ServiceApp version.
 */
static void process_suburi(PlayFiles_t *play_files)
{
    if(play_files->szSecondFile)
    {
        return;
    }

    char *orig_file = play_files->szFirstFile;

    if( strncmp(orig_file, "http://", 7) != 0 && strncmp(orig_file, "https://", 8) != 0 )
    {
        return;
    }

    const char *p = strstr(orig_file, "&suburi=");

    if( p )
    {
        size_t part_len = (size_t) (p - orig_file);
        char *first_file = malloc(part_len + 1);
        char *second_file = malloc(strlen(p + 8) + 1);

        if (!first_file || !second_file)
        {
            /* keep the uri as it is */
            free(first_file);
            free(second_file);
            return;
        }

        play_files->szFirstFile = first_file;
        memcpy(play_files->szFirstFile, orig_file, part_len);
        play_files->szFirstFile[part_len] = '\0';

        part_len = strlen(p + 8);
        play_files->szSecondFile = second_file;
        memcpy(play_files->szSecondFile, p+8, part_len);
        play_files->szSecondFile[part_len] = '\0';

        free(orig_file);
    }
}

/* Written once by the open thread when PLAYBACK_OPEN has returned, so the
 * wait in front of it ends at once and not after its timeout. A pipe of its
 * own: g_pfd also ends the TermThread. */
static int g_openDonePfd[2] = {-1, -1};

/* stdin is read by a thread of its own (StdinThreadStart): it collects whole
 * lines, whatever their length, takes "dk" lines at once and passes the other
 * lines on through a pipe, which the command loops read as they read stdin
 * before. A reading thread may wait for a dk answer under the container lock
 * while the command loop waits for that lock (seek, track switch or list), so
 * dk must not go through the command loop. Without the thread (pipe or thread
 * not created) the command loops read stdin themselves, as before. */
#define STDIN_LINE_MAX 16384 /* dk with DRM_KEY_MAX pairs: about 4.3 kB */
static int g_cmdFd = 0;          /* stdin, or the read end of the pipe */
static FILE *g_cmdFile = NULL;   /* NULL: stdin */
static int g_cmdPipeWr = -1;

static void StdinLine(char *line, size_t len)
{
    if (len >= 2 && 'd' == line[0] && 'k' == line[1])
    {
        line[len] = '\0';
        ffmpeg_drm_key_command(line);
        return;
    }
    if ('q' == line[0])
    {
        /* The command loop may wait for the container lock, which the
         * reading thread holds while it waits for the network. abortRequested
         * makes the interrupt callback end that wait, so the loop gets to the
         * q, which it handles as always. Before the start the open loop
         * handles q itself. */
        pthread_mutex_lock(&playbackStartMtx);
        if (isPlaybackStarted)
        {
            __atomic_store_n(&PlaybackHandler.abortRequested, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&playbackStartMtx);
    }
    while (len > 0)
    {
        ssize_t n = write(g_cmdPipeWr, line, len);
        if (n < 0)
        {
            if (EINTR == errno)
                continue;
            return;
        }
        line += n;
        len -= n;
    }
}

static void *StdinThreadFun(void *arg)
{
    char chunk[512];
    char *line = malloc(STDIN_LINE_MAX + 1);
    size_t len = 0;
    int skip = 0; /* the rest of a line longer than STDIN_LINE_MAX */
    int fifo = 0;
    struct stat st;
    (void)arg;

    if (NULL == line)
        return NULL;

    for (;;)
    {
        fd_set readfds;
        ssize_t n, i;

        FD_ZERO(&readfds);
        FD_SET(0, &readfds);
        if (-1 == select(1, &readfds, NULL, NULL, NULL))
        {
            if (EINTR == errno)
                continue;
            break;
        }
        n = read(0, chunk, sizeof(chunk));
        if (n < 0)
        {
            if (EINTR == errno || EAGAIN == errno || EWOULDBLOCK == errno)
                continue;
            break;
        }
        if (0 == n)
        {
            /* the end of a command pipe (or socket): the front end is gone */
            fifo = 0 == fstat(0, &st) && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
            break;
        }
        for (i = 0; i < n; i++)
        {
            if (!skip)
            {
                if (len < STDIN_LINE_MAX)
                {
                    line[len++] = chunk[i];
                }
                else
                {
                    printf("stdin: line longer than %d characters dropped\n", STDIN_LINE_MAX);
                    skip = 1;
                    len = 0;
                }
            }
            if ('\n' == chunk[i])
            {
                if (!skip)
                    StdinLine(line, len);
                skip = 0;
                len = 0;
            }
        }
    }
    if (len && !skip)
    {
        /* the last line without its end */
        StdinLine(line, len);
    }
    free(line);
    if (fifo)
    {
        /* the command loops read the end of the pipe as the stop command */
        close(g_cmdPipeWr);
    }
    return NULL;
}

static void StdinThreadStart(void)
{
    int pfd[2];
    int flags;
    pthread_t tid;

    if (pipe(pfd) == -1)
        return;
    flags = fcntl(pfd[0], F_GETFL);
    if (flags == -1 || fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK) == -1 ||
        NULL == (g_cmdFile = fdopen(pfd[0], "r")))
    {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    g_cmdPipeWr = pfd[1];
    if (0 != pthread_create(&tid, NULL, StdinThreadFun, NULL))
    {
        fclose(g_cmdFile);
        close(pfd[1]);
        g_cmdFile = NULL;
        g_cmdPipeWr = -1;
        return;
    }
    pthread_detach(tid);
    g_cmdFd = pfd[0];
}

/* Waits for a command, g_pfd and, when it is not -1, fd for at most ms. */
static int kbhit_fd(int ms, int fd)
{
    struct timeval tv;
    fd_set readfds;
    int maxfd = g_pfd[0] > g_cmdFd ? g_pfd[0] : g_cmdFd;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    FD_ZERO(&readfds);
    FD_SET(g_cmdFd, &readfds);
    FD_SET(g_pfd[0], &readfds);
    if (fd >= 0)
    {
        FD_SET(fd, &readfds);
        if (fd > maxfd)
            maxfd = fd;
    }

    if(-1 == select(maxfd + 1, &readfds, NULL, NULL, &tv))
    {
        return 0;
    }

    if(FD_ISSET(g_cmdFd, &readfds))
    {
        return 1;
    }

    return 0;
}

static int kbhit(int ms)
{
    return kbhit_fd(ms, -1);
}

extern int LinuxDvbEac3RestartState(void);

/* What an audio track switch does, without a change of track: the 0 s seek
 * of the container where it does one for a switch, then the restart of the
 * audio decoder. Done once after a DD+ passthrough start on Vu+ Ultimo 4K /
 * Duo 4K SE, which stays silent without it. isSeeking keeps the demuxer
 * thread out of the output meanwhile, as it does for a switch. */
static void Eac3PassthroughRestart(void)
{
    uint8_t wasSeeking = g_player->playback->isSeeking;
    int32_t id = -1;

    g_player->playback->isSeeking = 1;
    if (0 == g_player->manager->audio->Command(g_player, MANAGER_GET, &id) && id >= 0 &&
        g_player->container && g_player->container->selectedContainer &&
        0 == g_player->container->selectedContainer->Command(g_player, CONTAINER_SWITCH_AUDIO, &id))
    {
        g_player->output->audio->Command(g_player, OUTPUT_SWITCH, (void*)"audio");
    }
    g_player->playback->isSeeking = wasSeeking;
}

/* Non-blocking read of a command. The end of a command pipe (or socket)
 * means the front end that wrote to it is gone: read it as the stop
 * command. A terminal, /dev/null or a file on stdin keep the old
 * behaviour, so a player without a command pipe still plays to the end.
 * A stop signal is read as the stop command as well. */
static char *ReadCommand(char *buff, int size)
{
    struct stat st;
    FILE *in = g_cmdFile ? g_cmdFile : stdin;

    if (g_stopSignal)
    {
        strcpy(buff, "q\n");
        return buff;
    }
    if (fgets(buff, size, in))
    {
        return buff;
    }
    if (feof(in) && 0 == fstat(fileno(in), &st) &&
        (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)))
    {
        strcpy(buff, "q\n");
        return buff;
    }
    return NULL;
}

static void SetBuffering()
{
    static char buff[2048];
    memset( buff, '\0', sizeof(buff));
    if( setvbuf(stderr, buff, _IOLBF, sizeof(buff)) )
    {
        printf("SetBuffering: failed to change the buffer of stderr\n");
    }
    
    // make fgets not blocking 
    int flags = fcntl(fileno(stdin), F_GETFL, 0);
    fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
}

static void SetNice(int prio)
{
#if 0
    setpriority(PRIO_PROCESS, 0, -8);
    
    int prio = sched_get_priority_max(SCHED_RR) / 2;
    struct sched_param param = {
        .sched_priority = prio
    };
    sched_setscheduler(0, SCHED_RR, &param);
#else
    if (-1 == setpriority(PRIO_PROCESS, 0, prio))
    {
        printf("setpriority - failed\n");
    }
#endif
}

/* The audio manager has the values of the codec parameters, read when the
 * tracks were listed. The track that plays can be more exact: channel count
 * of the decoded frames, Atmos found in the packets, the stream values of
 * codec parameters that changed after the list. */
static void AudioLiveInfo(TrackDescription_t *track)
{
    int channels = 0;
    AudioStreamInfo_t info;
    const char *description = NULL;

    if (container_ffmpeg_audio_live_info(track->Id, &channels, &description, &info))
    {
        if (channels > 0)
        {
            track->channels = channels;
        }
        track->audio_info = info;
        if (description)
        {
            track->Description = description;
        }
    }
}

/* The video track that plays: the stream values of codec parameters that
 * changed after the list (HLS variant switch). */
static void VideoLiveInfo(TrackDescription_t *track)
{
    VideoStreamInfo_t info;

    if (container_ffmpeg_video_live_info(track->Id, &info))
    {
        track->video_info = info;
    }
}

/* The keys after "t" of an audio entry, from ,"sr" to the last value. */
static const char *AudioInfoJson(char *dst, size_t size, const AudioStreamInfo_t *info)
{
    char layout[2 * sizeof(info->channel_layout)];

    snprintf(dst, size, ",\"sr\":%d,\"br\":%d,\"cl\":\"%s\",\"bps\":%d", info->sample_rate,
             info->bit_rate, JsonEscape(layout, sizeof(layout), info->channel_layout), info->bits_per_sample);
    return dst;
}

/* The keys after "hdr" of v_c, after "n" of a v_l entry. */
static const char *VideoInfoJson(char *dst, size_t size, const VideoStreamInfo_t *info)
{
    char profile[2 * sizeof(info->profile_level)];
    char primaries[64];
    char transfer[64];

    snprintf(dst, size, ",\"pr\":\"%s\",\"bd\":%d,\"br\":%d,\"dv\":%d,\"cr\":\"%s\",\"tc\":\"%s\"",
             JsonEscape(profile, sizeof(profile), info->profile_level), info->bit_depth, info->bit_rate,
             info->dv_profile, JsonEscape(primaries, sizeof(primaries), info->color_primaries),
             JsonEscape(transfer, sizeof(transfer), info->color_transfer));
    return dst;
}

/* The audio tracks of the last MANAGER_LIST, with the values of the codec
 * parameters; audioTracksListed once there was one (the list can be NULL). */
static TrackDescription_t *audioTracks = NULL;
static int audioTracksListed = 0;

static void FreeTracks(TrackDescription_t *TrackList)
{
    int i;

    if (NULL == TrackList)
    {
        return;
    }
    for (i = 0; TrackList[i].Id >= 0; ++i)
    {
        free(TrackList[i].Encoding);
        free(TrackList[i].Name);
        free(TrackList[i].Title);
    }
    free(TrackList);
}

/* The a_l, s_l or v_l message of TrackList into cache. The audio entries get
 * the live values of the track that plays; TrackList is not changed. */
static void FormatTracks(strbuffer_t *cache, const char *argvBuff, const TrackDescription_t *TrackList)
{
    char tmp_str[1280];
    char enc[128];
    char name[512];
    char desc[64];
    char info[320];
    /* an audio entry with every string and every number at its limit
     * is about 1170 bytes: mind tmp_str when a buffer grows */
    char title[256];
    int i;

    if (NULL == TrackList)
    {
        // not tracks
        sprintf( tmp_str, "{\"%c_%c\": []}\n", argvBuff[0], argvBuff[1]);
        strbuffer_add( cache, tmp_str );
        return;
    }

    sprintf( tmp_str, "{\"%c_%c\": [", argvBuff[0], argvBuff[1]);
    strbuffer_add( cache, tmp_str );

    for (i = 0; TrackList[i].Id >= 0; ++i)
    {
        if(0 < i)
        {
            strbuffer_add( cache, ", " );
        }

        if ('s' == argvBuff[0])
            snprintf(tmp_str, sizeof(tmp_str), "{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"f\":%d,\"t\":\"%s\"}", TrackList[i].Id, JsonEscape(enc, sizeof(enc), TrackList[i].Encoding), JsonEscape(name, sizeof(name), TrackList[i].Name), TrackList[i].is_forced, JsonEscape(title, sizeof(title), TrackList[i].Title));
        else if ('a' == argvBuff[0])
        {
            TrackDescription_t track = TrackList[i];

            AudioLiveInfo(&track);
            snprintf(tmp_str, sizeof(tmp_str), "{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"c\":%d,\"d\":\"%s\",\"t\":\"%s\"%s}", track.Id, JsonEscape(enc, sizeof(enc), track.Encoding), JsonEscape(name, sizeof(name), track.Name), track.channels, JsonEscape(desc, sizeof(desc), track.Description), JsonEscape(title, sizeof(title), track.Title), AudioInfoJson(info, sizeof(info), &track.audio_info));
        }
        else
        {
            TrackDescription_t track = TrackList[i];

            VideoLiveInfo(&track);
            snprintf(tmp_str, sizeof(tmp_str), "{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\"%s}", track.Id, JsonEscape(enc, sizeof(enc), track.Encoding), JsonEscape(name, sizeof(name), track.Name), VideoInfoJson(info, sizeof(info), &track.video_info));
        }
        strbuffer_add( cache, tmp_str );
    }
    strbuffer_add( cache, "]}\n");
}

static int HandleTracks(Manager_t *ptrManager, const PlaybackCmd_t playbackSwitchCmd, const char *argvBuff)
{
    int commandRetVal = 0;
    
    if (NULL == ptrManager || NULL == argvBuff || 2 != strnlen(argvBuff, 2))
    {
        return -1;
    }
    
    switch (argvBuff[1]) 
    {
        case 'l': 
        {
            /* The audio list has values that change while the track plays.
             * Only those values are taken again, the list is formatted from
             * the copy of the last MANAGER_LIST: that refreshes the tracks
             * under the container lock, which the play thread holds while a
             * read waits for the network (a live edge, a stalled server). */
            if ('a' == argvBuff[0])
            {
                static int audioInfoGenSeen = 0;
                int audioInfoGen = container_ffmpeg_audio_info_gen();

                if (ptrManager->tracks_cache && audioInfoGenSeen != audioInfoGen)
                {
                    strbuffer_free( ptrManager->tracks_cache );
                    ptrManager->tracks_cache = NULL;
                    if (audioTracksListed)
                    {
                        ptrManager->tracks_cache = strbuffer_init( 1024 );
                        FormatTracks(ptrManager->tracks_cache, argvBuff, audioTracks);
                    }
                }
                audioInfoGenSeen = audioInfoGen;
            }
            /* the closed captions of the video stream are listed once they come */
            if ('s' == argvBuff[0])
            {
                static int subtitleInfoGenSeen = 0;
                int subtitleInfoGen = container_ffmpeg_subtitle_info_gen();

                if (ptrManager->tracks_cache && subtitleInfoGenSeen != subtitleInfoGen)
                {
                    strbuffer_free( ptrManager->tracks_cache );
                    ptrManager->tracks_cache = NULL;
                }
                subtitleInfoGenSeen = subtitleInfoGen;
            }
        	if( ptrManager->tracks_cache == NULL )
        	{
        		/* read tracks info into cache */
        		ptrManager->tracks_cache = strbuffer_init( 1024 );

				TrackDescription_t *TrackList = NULL;
				ptrManager->Command(g_player, MANAGER_LIST, &TrackList);
				FormatTracks(ptrManager->tracks_cache, argvBuff, TrackList);
				if ('a' == argvBuff[0] && !tracks_cache_disabled)
				{
					FreeTracks(audioTracks);
					audioTracks = TrackList;
					audioTracksListed = 1;
				}
				else
				{
					FreeTracks(TrackList);
				}
        	}
        	E2iSendMsg("%s", ptrManager->tracks_cache->buffer );

        	if( tracks_cache_disabled )
        	{
        		/* when tracks cache is disabled, then simply clean it's content */
				strbuffer_free( ptrManager->tracks_cache );
				ptrManager->tracks_cache = NULL;
        	}

            break;
        }
        case 'c': 
        {
            
            TrackDescription_t *track = NULL;
            ptrManager->Command(g_player, MANAGER_GET_TRACK_DESC, &track);
            if (NULL != track) 
            {
                char enc[128];
                char name[512];
                char title[256];

                JsonEscape(enc, sizeof(enc), track->Encoding);
                JsonEscape(name, sizeof(name), track->Name);
                JsonEscape(title, sizeof(title), track->Title);
                if ('s' == argvBuff[0])
                {
                    E2iSendMsg("{\"s_c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"f\":%d,\"t\":\"%s\"}}\n", track->Id, enc, name, track->is_forced, title);
                }
                else if ('a' == argvBuff[0])
                {
                    char desc[64];
                    char info[320];

                    AudioLiveInfo(track);
                    E2iSendMsg("{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"c\":%d,\"d\":\"%s\",\"t\":\"%s\"%s}}\n", argvBuff[0], argvBuff[1], track->Id , enc, name, track->channels, JsonEscape(desc, sizeof(desc), track->Description), title, AudioInfoJson(info, sizeof(info), &track->audio_info));
                }
                else // video
                {
                    char info[320];
                    char msg[sizeof(enc) + sizeof(name) + sizeof(info) + 192];

                    VideoLiveInfo(track);
                    snprintf(msg, sizeof(msg), "{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"w\":%d,\"h\":%d,\"f\":%u,\"p\":%d,\"an\":%d,\"ad\":%d,\"hdr\":%d%s}}\n", \
                    argvBuff[0], argvBuff[1], track->Id , enc, name, track->width, track->height, track->frame_rate, track->progressive, track->aspect_ratio_num, track->aspect_ratio_den, container_ffmpeg_get_hdr_type(), VideoInfoJson(info, sizeof(info), &track->video_info));
                    E2iSendMsg("%s", msg);
                    SendExternalVideo(msg);
                }
                free(track->Encoding);
                free(track->Name);
                free(track->Title);
                free(track);
            }
            else
            {
                // no tracks
                if ('s' == argvBuff[0])
                {
                    E2iSendMsg("{\"s_c\":{\"id\":-1,\"e\":\"\",\"n\":\"\",\"f\":0,\"t\":\"\"}}\n");
                }
                else if ('a' == argvBuff[0])
                {
                    E2iSendMsg("{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"c\":%d,\"d\":\"%s\",\"t\":\"%s\",\"sr\":0,\"br\":0,\"cl\":\"\",\"bps\":0}}\n", argvBuff[0], argvBuff[1], -1, "", "", 0, "", "");
                }
                else // video
                {
                    E2iSendMsg("{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"w\":%d,\"h\":%d,\"f\":%u,\"p\":%d,\"pr\":\"\",\"bd\":0,\"br\":0,\"dv\":0,\"cr\":\"\"}}\n", argvBuff[0], argvBuff[1], -1, "", "", -1, -1, 0, -1);
                }
            }
            break;
        }
        default: 
        {
            /* switch command available only for audio and subtitle tracks */
            if ('a' == argvBuff[0] || 's' == argvBuff[0])
            {
                int ok = 0;
                int id = -1;
                if ('i' == argvBuff[1])
                {
                    int idx = -1;
                    ok = sscanf(argvBuff+2, "%d", &idx);
                    if (idx >= 0)
                    {
                        TrackDescription_t *TrackList = NULL;
                        ptrManager->Command(g_player, MANAGER_LIST, &TrackList);
                        if( NULL != TrackList) 
                        {
                            int i = 0;
                            for (i = 0; TrackList[i].Id >= 0; ++i) 
                            {
                                if (idx == i)
                                {
                                    id = TrackList[i].Id;
                                }
                                free(TrackList[i].Encoding);
                                free(TrackList[i].Name);
                                free(TrackList[i].Title);
                            }
                            free(TrackList);
                        }
                    }
                    else
                    {
                        id = idx;
                    }
                }
                else
                {
                    ok = sscanf(argvBuff+1, "%d", &id);
                }
                
                if(id >= 0 || (1 == ok && id == -1))
                {
                    commandRetVal = g_player->playback->Command(g_player, playbackSwitchCmd, (void*)&id);
                    E2iSendMsg("{\"%c_%c\":{\"id\":%d,\"sts\":%d}}\n", argvBuff[0], 's', id, commandRetVal);
                }
            }
            break;
        }
    }
    
    return commandRetVal;
}

static void UpdateVideoTrack()
{
    HandleTracks(g_player->manager->video, (PlaybackCmd_t)-1, "vc");
}

static int ParseParams(int argc,char* argv[], PlayFiles_t *playbackFiles, int *pAudioTrackIdx, int *subtitleTrackIdx, uint32_t *linuxDvbBufferSizeMB)
{   
    int ret = 0;
    int c;
    int quality = 0, default_audio = 0, export_pcm = 0;
    int xcode = 0; /* 1: -Z, 2: -X, 4: -Y, 8: --dts-eac3, 16: --truehd-eac3 */
    int xcode_eac3 = 0; /* -E */
    /* the long names of the transcoding options, values above every character */
    enum
    {
        OPT_START = 0x200, /* --start, not a transcoding option */
        OPT_DTS_AC3 = 0x100,
        OPT_TRUEHD_AC3,
        OPT_DTS_EAC3,
        OPT_TRUEHD_EAC3,
        OPT_HEAAC_AACLC,
        OPT_AUDIO_CHANNEL,
        OPT_KEY,
        OPT_KEY_REQUEST,
        OPT_KEY_TIMEOUT
    };
    static const struct option long_options[] =
    {
        {"start",       required_argument, NULL, OPT_START},
        {"dts-ac3",     no_argument, NULL, OPT_DTS_AC3},
        {"truehd-ac3",  no_argument, NULL, OPT_TRUEHD_AC3},
        {"dts-eac3",    no_argument, NULL, OPT_DTS_EAC3},
        {"truehd-eac3", no_argument, NULL, OPT_TRUEHD_EAC3},
        {"heaac-aaclc", no_argument, NULL, OPT_HEAAC_AACLC},
        {"audio-channel", required_argument, NULL, OPT_AUDIO_CHANNEL},
        {"key",         required_argument, NULL, OPT_KEY},
        {"key-request", no_argument, NULL, OPT_KEY_REQUEST},
        {"key-timeout", required_argument, NULL, OPT_KEY_TIMEOUT},
        {NULL, 0, NULL, 0}
    };
    while ( (c = getopt_long(argc, argv, "EZXYKG:W:H:A:V:U:we3dlsrimvCa:j:n:x:u:c:h:o:p:P:t:9:0:1:4:5:6:7:f:b:F:S:O:T:L:Q:D:M:", long_options, NULL)) != -1)
    {
        switch (c) 
        {
        case 'K':
            export_pcm = 1;
            break;
        case 'Z':
        case OPT_DTS_AC3:
            xcode |= 1;
            break;
        case 'X':
        case OPT_TRUEHD_AC3:
            xcode |= 2;
            break;
        case 'Y':
        case OPT_HEAAC_AACLC:
            xcode |= 4;
            break;
        case 'E':
            xcode_eac3 = 1;
            break;
        case OPT_AUDIO_CHANNEL:
            g_audio_channel = ParseAudioChannel(optarg);
            if (g_audio_channel < 0)
            {
                printf("--audio-channel: left, stereo or right (0, 1, 2), not \"%s\"\n", optarg);
                g_audio_channel = OUTPUT_AUDIO_CHANNEL_STEREO;
                ret = -1;
            }
            break;
        case OPT_DTS_EAC3:
            xcode |= 8;
            break;
        case OPT_KEY:
            if (ffmpeg_drm_key_add(optarg) < 0)
            {
                printf("--key: <kid>:<key>, 32 hex digits each, not \"%s\"\n", optarg);
                ret = -1;
            }
            break;
        case OPT_KEY_REQUEST:
            ffmpeg_drm_key_request_set(1);
            break;
        case OPT_KEY_TIMEOUT:
            ffmpeg_drm_key_timeout_set(atoi(optarg));
            break;
        case OPT_TRUEHD_EAC3:
            xcode |= 16;
            break;
        case 'G':
            g_graphic_sub_path = optarg;
            break;
        case 'W':
        {
            int val = atoi(optarg);
            if (val) g_windows_width = val;
            break;
        }
        case 'H':
        {
            int val = atoi(optarg);
            if (val) g_windows_height = val;
            break;
        }
        case 'a':
        {
            int flag = atoi(optarg);
            printf("Software decoder will be used for AAC codec\n");
            aac_software_decoder_set(flag & 0x01);
            aac_latm_software_decoder_set(flag & 0x02);
            break;
        }
        case 'j':
            printf("AAC PTS dejitter threshold set to %d ms\n", atoi(optarg));
            aac_pts_dejitter_set(atoi(optarg));
            break;
        case 'e':
            printf("Software decoder will be used for EAC3 codec\n");
            eac3_software_decoder_set(1);
            break;
        case 'A':
            printf("Software decoder will be used for AMR codec\n");
            amr_software_decoder_set(atoi(optarg));
            break;
        case 'V':
            printf("Software decoder will be used for VORBIS codec\n");
            vorbis_software_decoder_set(atoi(optarg));
            break;
        case 'U':
            printf("Software decoder will be used for OPUS codec\n");
            opus_software_decoder_set(atoi(optarg));
            break;
        case '3':
            printf("Software decoder will be used for AC3 codec\n");
            ac3_software_decoder_set(1);
            break;
        case 'd':
            printf("Software decoder will be used for DTS codec\n");
            dts_software_decoder_set(1);
            break;
        case 'm':
            printf("Software decoder will be used for MP3 codec\n");
            mp3_software_decoder_set(1);
            break;
        case 'w':
            printf("Software decoder will be used for WMA codec\n");
            wma_software_decoder_set(1);
            break;
        case 'l': 
            printf("Audio software decoding as LPCM\n");
            insert_pcm_as_lpcm_set(1);
            break;
        case 's':
            printf("Software decoder will decode to stereo\n");
            stereo_software_decoder_set(1);
            break;
        case 'r':
            printf("Software decoder do not use PCM resampling\n");
            pcm_resampling_set(0);
            break;
        case 'o':
            printf("Set progressive download to %d\n", atoi(optarg));
            progressive_playback_set(atoi(optarg));
            break;
        case 'p':
            SetNice(atoi(optarg));
            break;
        case 'P':
            sel_program_id_set(atoi(optarg));
            break;
        case 'Q':
        case 'D':
        {
            char *end;
            long value;
            errno = 0;
            value = strtol(optarg, &end, 10);
            if (errno || end == optarg || *end || value < 0 || value > (c == 'Q' ? 2 : 1))
            {
                printf("Invalid -%c value: %s\n", c, optarg);
                ret = -1;
            }
            else if (c == 'Q')
                quality = value;
            else
                default_audio = value;
            break;
        }
        case 'M':
        {
            char *end, *height_start;
            long width, height;
            errno = 0;
            width = strtol(optarg, &end, 10);
            int width_valid = end != optarg && *end == 'x';
            height_start = *end == 'x' ? end + 1 : end;
            height = strtol(height_start, &end, 10);
            if (errno || !width_valid ||
                end == height_start || *end || width < 0 || height < 0 ||
                width > INT32_MAX || height > INT32_MAX || (!width != !height))
            {
                printf("Invalid H.264 limit (expected WIDTHxHEIGHT): %s\n", optarg);
                ret = -1;
            }
            else
                h264_resolution_limit_set(width, height);
            break;
        }
        case 't':
            *pAudioTrackIdx = atoi(optarg);
            break;
        case '9':
            *subtitleTrackIdx = atoi(optarg);
            break;
        case 'x':
            if (optarg[0] != '\0')
            {
                playbackFiles->szSecondFile = map_inter_file_path(optarg);
                if (NULL == playbackFiles->szSecondFile && 0 == strncmp(optarg, "iptv://", 7))
                {
                    /* the iptv:// file itself is opened as the input: its
                     * open fails as for any input that cannot be opened,
                     * with FF_ERROR, PLAYBACK_OPEN sts -1 and exit code 10 */
                    printf("file: [%s] cannot be read\n", optarg);
                    playbackFiles->szSecondFile = map_inter_file_path(optarg + 7);
                }
                if (NULL == playbackFiles->szSecondFile)
                {
                    printf("file: [%s] cannot be read\n", optarg);
                    ret = -1;
                }
            }
            break;
        case 'h':
            ffmpeg_av_dict_set("headers", optarg, 0);
            break;
        case 'u':
            ffmpeg_av_dict_set("user_agent", optarg, 0);
            break;

        case 'c':
            printf("For now cookies should be set via headers option!\n");
            ffmpeg_av_dict_set("cookies", optarg, 0);
            break;
        case 'i':
            printf("Play in (infinity) loop.\n");
            PlaybackHandler.isLoopMode = 1;
            break;
        case 'v':
            printf("Use live TS stream mode.\n");
            PlaybackHandler.isTSLiveMode = 1;
            break;
        case 'n':
            printf("Force rtmp protocol implementation\n");
            rtmp_proto_impl_set(atoi(optarg));
            break;
        case 'C':
            printf("Disabling tracks cache\n");
            tracks_cache_disabled = 1;
            break;
        case '0':
            ffmpeg_av_dict_set("video_rep_index", optarg, 0);
            break;
        case '1':
            ffmpeg_av_dict_set("audio_rep_index", optarg, 0);
            break;
        case '4':
#ifdef HAVE_FLV2MPEG4_CONVERTER
            flv2mpeg4_converter_set(atoi(optarg));
#endif
            break;

        case '5':
            ffmpeg_av_dict_set("cenc_decryption_key", optarg, 0);
            break;

        case '6':
            ffmpeg_av_dict_set("cenc_decryption_video_key", optarg, 0);
            break;

        case '7':
            ffmpeg_av_dict_set("cenc_decryption_audio_key", optarg, 0);
            break;

        case 'f':
        {
            char *ffopt = strdup(optarg);
            char *ffval = ffopt ? strchr(ffopt, '=') : NULL;
            if (ffval)
            {
                *ffval = '\0';
                ffval += 1;
                ffmpeg_av_dict_set(ffopt, ffval, 0);
                if (!strcmp(ffopt, "ca_file") || !strcmp(ffopt, "cafile"))
                {
                    free(g_ca_file);
                    g_ca_file = strdup(ffval);
                }
            }
            free(ffopt);
            break;
        }
        case 'b':
            *linuxDvbBufferSizeMB = 1024 * 1024 * atoi(optarg);
            break;
        case 'S':
            playbackFiles->iFirstFileSize = (uint64_t) strtoull(optarg, (char **)NULL, 10);
            break;
        case 'O':
            playbackFiles->iFirstMoovAtomOffset = (uint64_t) strtoull(optarg, (char **)NULL, 10);
            break;
        case 'F':
            if (optarg[0] != '\0')
            {
                playbackFiles->szFirstMoovAtomFile = map_inter_file_path(optarg);
                if (NULL == playbackFiles->szFirstMoovAtomFile)
                {
                    printf("file: [%s] cannot be read\n", optarg);
                    ret = -1;
                }
            }
            break;

        case OPT_START:
        {
            char *end;
            double seconds;
            errno = 0;
            seconds = strtod(optarg, &end);
            /* !(x >= 0) is true for NaN as well */
            if (errno || end == optarg || *end || !(seconds >= 0) || seconds > 1e9)
            {
                printf("Invalid --start value (seconds): %s\n", optarg);
                ret = -1;
            }
            else
            {
                printf("Start at %.3f s\n", seconds);
                start_position_set((int64_t)(seconds * 1000000));
            }
            break;
        }

        case 'T':
        	PlaybackHandler.httpTimeout = (uint32_t) strtoul(optarg, NULL, 10);
            printf("Setting http timeout to %u ms\n", PlaybackHandler.httpTimeout);
            break;

        case 'L':
            set_log_level(optarg);
            break;

        default:
            printf ("?? getopt returned character code 0%o ??\n", c);
            ret = -1;
        }
    }
    
    adaptive_quality_set(quality, default_audio);
    pcm_audio_export_set(export_pcm);
    if (export_pcm)
    {
        aac_software_decoder_set(1);
        aac_latm_software_decoder_set(1);
        ac3_software_decoder_set(1);
        eac3_software_decoder_set(1);
        dts_software_decoder_set(1);
        wma_software_decoder_set(1);
        mp3_software_decoder_set(1);
        amr_software_decoder_set(1);
        vorbis_software_decoder_set(1);
        opus_software_decoder_set(1);
    }
    if (xcode && export_pcm)
    {
        printf("Audio transcoding (-Z, -X, -Y) is off: the PCM export (-K) takes the decoded audio\n");
    }
    else if (xcode)
    {
        /* 1: AC3 encoder, 2: AAC encoder, 4: E-AC-3 encoder */
        const int32_t caps = audio_transcode_caps();
        /* What is asked for, [0] for DTS and [1] for TrueHD: 0 nothing, 1 AC3,
         * 2 E-AC-3. E-AC-3 comes from the long option of the codec or from
         * -E, which takes every codec that -Z or -X selects; for one codec
         * E-AC-3 wins over AC3, in any order of the options. */
        const int want[2] = {
            ((xcode & 8) || ((xcode & 1) && xcode_eac3)) ? 2 : ((xcode & 1) ? 1 : 0),
            ((xcode & 16) || ((xcode & 2) && xcode_eac3)) ? 2 : ((xcode & 2) ? 1 : 0)
        };
        /* what the encoders leave of it: AC3 when the E-AC-3 encoder is
         * missing, nothing when the AC3 encoder is missing as well */
        int to[2];
        int i, no_eac3 = 0, no_ac3 = 0;
        for (i = 0; i < 2; i++)
        {
            to[i] = want[i];
            if (to[i] == 2 && !(caps & 4))
            {
                no_eac3 = 1;
                to[i] = 1;
            }
            if (to[i] == 1 && !(caps & 1))
            {
                no_ac3 = 1;
                to[i] = 0;
            }
        }
        if (no_eac3)
            printf("No E-AC-3 encoder in this FFmpeg: -E, --dts-eac3 and --truehd-eac3 are ignored%s\n", (caps & 1) ? ", AC3 is used" : "");
        if (no_ac3)
            printf("No AC3 encoder in this FFmpeg: -Z and -X are ignored\n");
        if (to[0] && to[0] == to[1])
            printf("DTS and TrueHD will be transcoded to %s 5.1\n", to[0] == 2 ? "E-AC-3" : "AC3");
        else
        {
            if (to[0])
                printf("DTS will be transcoded to %s 5.1\n", to[0] == 2 ? "E-AC-3" : "AC3");
            if (to[1])
                printf("TrueHD will be transcoded to %s 5.1\n", to[1] == 2 ? "E-AC-3" : "AC3");
        }
        if ((xcode & 4) && !(caps & 2))
            printf("No AAC encoder in this FFmpeg: -Y is ignored\n");
        else if (xcode & 4)
            printf("HE-AAC will be transcoded to AAC-LC\n");
        dts_ac3_transcoder_set(to[0]);
        truehd_ac3_transcoder_set(to[1]);
        aac_he_transcoder_set((xcode & 4) && (caps & 2));
    }
    if (0 == ret && optind < argc)
    {
        playbackFiles->szFirstFile = map_inter_file_path(argv[optind]);
        if (NULL == playbackFiles->szFirstFile)
        {
            printf("file: [%s] cannot be read\n", argv[optind]);
            ret = -1;
        }
        else
        {
            printf("file: [%s]\n", playbackFiles->szFirstFile);
        }
        ++optind;
    }
    else
    {
        ret = -1;
    }
    return ret;
}

/* *********************************************************************** */
/* This thread is used in init phase because ffmpeg is blocking by default */
/* *********************************************************************** */

typedef struct
{
	PlayFiles_t *playbackFiles;
	int ret;
	/* set by the open thread with release order, after ret */
	volatile int init_finished;
} PlaybackInitParams_t;

static void *PlaybackInitThread(PlaybackInitParams_t *params)
{
    char threadname[17];
    strncpy(threadname, __func__, sizeof(threadname));
    threadname[16] = 0;
    prctl (PR_SET_NAME, (unsigned long)&threadname);

	params->ret = g_player->playback->Command(g_player, PLAYBACK_OPEN, params->playbackFiles);
	__atomic_store_n(&params->init_finished, 1, __ATOMIC_RELEASE);
	/* params is not touched after the flag: main may leave its block now.
	 * Without the pipe the wait ends after at most 1 s, as before. */
	if (g_openDonePfd[1] >= 0 && 1 != write(g_openDonePfd[1], "x", 1))
	{
	    printf("PlaybackInitThread wake-up write failed\n");
	}
	return NULL;
}

/* q during the open: how long the open may take to end after the interrupt
 * callback reports the stop (FFmpeg asks it every 100 ms while it waits) */
#define OPEN_STOP_WAIT_MS 2000

/* 1 when the open thread has returned within ms */
static int WaitOpenEnd(PlaybackInitParams_t *params, int ms)
{
    struct timespec tick = {0, 10 * 1000 * 1000};

    while (0 == __atomic_load_n(&params->init_finished, __ATOMIC_ACQUIRE))
    {
        if (ms <= 0)
            return 0;
        nanosleep(&tick, NULL);
        ms -= 10;
    }
    return 1;
}

int main(int argc, char* argv[])
{
    pthread_t termThread;
    int isTermThreadStarted = 0;
    
    int audioTrackIdx = -1;
    int subtitleTrackIdx = -1;
    int pts_check_nr = 0;
    int subtitleListGen = 0;
    
    uint32_t linuxDvbBufferSizeMB = 0; 
    
    char argvBuff[256];
    memset(argvBuff, '\0', sizeof(argvBuff));
    int commandRetVal = -1;

    /* before any thread is started, so that every thread inherits the mask */
    StopSignalsBlock();

    /* inform client that we can handle additional commands */
    /* start: the option --start <seconds> is known */
    /* xcode: the audio transcoding this build can do, 1: to AC3 (-Z, -X), 2: to AAC-LC (-Y), 4: to E-AC-3 (-E) */
    /* achannel: the stdin command am<n> (left/stereo/right of dual mono) is known */
    /* hlsseg: HLS segments played directly, 1: any name (.js, .png, none), 2: an image in front of the TS data */
    /* drmkey: 1 --key <kid>:<key>, 2 --key-request (DRM_KEY_REQ, stdin dk <kid>:<key>) */
    /* mpdopt: the OE-A options after .mpd in the url (&0= &1= &5= &6= &7= &8=) are taken off */
    E2iSendMsg("{\"EPLAYER3_EXTENDED\":{\"version\":%d"
               ",\"start\":1"
               ",\"xcode\":%d"
#ifdef HAVE_DREAMNEXTGEN
               /* winds on keyframes with f<speed> and b<speed> */
               ",\"trick\":1"
#endif
               ",\"achannel\":1"
               ",\"hlsseg\":%d"
               ",\"drmkey\":%d"
               ",\"mpdopt\":1"
               "}}\n", 187,
               audio_transcode_caps(),
               hls_segment_caps(), ffmpeg_drm_key_caps());

    PlayFiles_t playbackFiles;
    memset(&playbackFiles, 0x00, sizeof(playbackFiles));

    if (0 != ParseParams(argc, argv, &playbackFiles, &audioTrackIdx, &subtitleTrackIdx, &linuxDvbBufferSizeMB))
    {
        printf("Usage: exteplayer3 filePath [-u user-agent] [-c cookies] [-h headers] [-p prio] [-a n] [-d] [-w] [-l] [-s] [-i] [-C] [-T timeout] [-L desc] [-t audioTrackIndex] [-9 subtitleTrackIndex] [-x separateAudioUri] playbackUri\n");
        printf("[-K] export decoded S16LE audio to " PCM_EXPORT_PATH " (forces software decoding)\n");
        printf("[-b size] Linux DVB output buffer size in MB\n");
        printf("[-a 0|1|2|3] AAC software decoding, bit mask: 1 - AAC, 2 - AAC LATM (3 - both, 0 - none)\n");
        printf("[-j ms] AAC/DTS PTS dejitter threshold in ms, 0 - disabled (default 200)\n");
        printf("[-Q 0|1|2] HLS/DASH quality: auto/highest (0 or 2), lowest (1)\n");
        printf("[-D 0|1] Keep only default HLS audio renditions (default 0)\n");
        printf("[-M WIDTHxHEIGHT] Automatic H.264 resolution limit (default 0x0: disabled)\n");
        printf("[-e] EAC3 software decoding\n");
        printf("[-3] AC3 software decoding\n");
        printf("[-d] DTS software decoding\n");
        printf("[-m] MP3 software decoding\n");
        printf("[-A 0|1] disable|enable AMR software decoding\n");
        printf("[-V 0|1] disable|enable VORBIS software decoding\n");
        printf("[-U 0|1] disable|enable OPUS software decoding\n");
        printf("[-w] WMA2, WMA/PRO software decoding\n");
        printf("[-l] software decoder use LPCM for injection (otherwise wav PCM will be used)\n");
        printf("[-s] software decoding as stereo [downmix]\n");
        printf("[-r] PCM tracks go to the PCM writer as they are, without software decoding and resampling\n");
        printf("[-Z] [--dts-ac3] DTS transcoding to AC3 5.1 (also with -d)\n");
        printf("[-X] [--truehd-ac3] TrueHD/MLP transcoding to AC3 5.1\n");
        printf("[-E] -Z and -X transcode to E-AC-3 5.1 instead of AC3 (AC3 when FFmpeg has no E-AC-3 encoder)\n");
        printf("[--dts-eac3] DTS transcoding to E-AC-3 5.1, with or without -Z\n");
        printf("[--truehd-eac3] TrueHD/MLP transcoding to E-AC-3 5.1, with or without -X\n");
        printf("[-Y] [--heaac-aaclc] HE-AAC transcoding to AAC-LC (not for AAC decoded in software, -a)\n");
        printf("     all of them need the encoder in FFmpeg (\"xcode\" in EPLAYER3_EXTENDED) and are off with -K\n");
        printf("[--audio-channel left|stereo|right] dual mono channel at start (0|1|2, default stereo; stdin am<n>)\n");
        printf("[--key kid:key] CENC key of a KID, 32 hex digits each, repeatable (MP4, DASH, HLS fMP4)\n");
        printf("[--key-request] ask the front end for the key of other KIDs: DRM_KEY_REQ, answer on stdin dk kid:key or dk kid:-\n");
        printf("                (\"drmkey\" & 2 in EPLAYER3_EXTENDED: only with an FFmpeg that has decryption_key_url)\n");
        printf("[--key-timeout s] how long --key-request waits for the answer (default 10)\n");
        printf("[url.mpd&5=key] OE-A compatibility, options after .mpd in the url (taken off it; \"mpdopt\" in EPLAYER3_EXTENDED):\n");
        printf("                &0=n / &1=n video / audio representation index, &5=key for every KID,\n");
        printf("                &6=key / &7=key for the video / audio KIDs; --key and dk keys win.\n");
        printf("                The per-KID way is --key / --key-request\n");
#ifdef HAVE_FLV2MPEG4_CONVERTER
        printf("[-4 0|1] - disable/enable flv2mpeg4 converter\n");
#endif
        printf("[-C] disable tracks info cache\n");
        printf("[-T ms] set http timeout in ms (default 10000)\n");
        printf("[-L desc] description of debug level to change in format NAME:INT_VALUE\n");
        printf("[-i] play in infinity loop\n");
        printf("[-v] switch to live TS stream mode\n");
        printf("[-n 0|1|2] rtmp force protocol implementation auto(0) native/ffmpeg(1) or librtmp(2)\n");
        printf("[-o 0|1] set progressive download\n");
        printf("[-p value] nice value\n");
        printf("[-P value] select Program ID from multi-service stream\n");
        printf("[-t index] audio track switched on at start, position in the audio track list (0 - first, as stdin ai<n>)\n");
        printf("[-9 index] subtitle track switched on at start, position in the subtitle track list (0 - first, as stdin si<n>)\n");
        printf("[--start seconds] start position, before the first packet (not for live streams; \"start\" in EPLAYER3_EXTENDED)\n");
        printf("[-h headers] set custom HTTP headers \"Name: value\\r\\nName: value\\r\\n\"\n");
        printf("[-u user-agent] set custom http User-Agent header\n");
        printf("[-c cookies] set cookies - not working at now, please use -h instead\n");
        printf("[-x separateAudioUri]\n");
        printf("[-0 idx] video MPEG-DASH representation index\n");
        printf("[-1 idx] audio MPEG-DASH representation index\n");
        printf("[-5 key] MPEG-DASH cenc decryption key of all streams, 32 hex digits (default key)\n");
        printf("[-6 key] MPEG-DASH cenc decryption key of the video streams, 32 hex digits\n");
        printf("[-7 key] MPEG-DASH cenc decryption key of the audio streams, 32 hex digits\n");
        printf("[-f ffopt=ffval] any other ffmpeg option\n");
        printf("[-F path] additional file with moov atom data (used for mp4 playback in progressive download mode)\n");
        printf("[-O offset] moov atom offset in the original file (used for mp4 playback in progressive download mode)\n");
        printf("[-S size] remote file size (used for mp4 playback in progressive download mode)\n");
        printf("[-G path] directory where graphic subtitles frames will be saved\n");
        printf("[-W width] osd window width (width of the window used to scale graphic subtitle frame, default 1280)\n");
        printf("[-H height] osd window height (height of the window used to scale graphic subtitle frame, default 720)\n");
        exit(1);
    }
    
#ifdef HAVE_DREAMNEXTGEN
    /* The dreamaudiod service decodes or passes through what has a PES
     * writer (AC-3, E-AC-3, DTS, AAC, MPEG audio); the rest goes to it as
     * PCM. Must run after ParseParams, serviceapp's -a/-n would clear it.
     * TrueHD has no PES writer in this tree, it is always decoded. */
    wma_software_decoder_set(1);
    vorbis_software_decoder_set(1);
    opus_software_decoder_set(1);
    amr_software_decoder_set(1);
#endif

    ffmpeg_av_dict_set("fake_last_subtitle", "1", 0);

    /* FFmpeg 9 verifies https certificates by default. Images
     * whose CA bundle is not where the OpenSSL of the build looks (VTi:
     * only /etc/ssl/certs/ca-certificates.crt, OPENSSLDIR /etc/ssl)
     * would fail every https open; SSL_CERT_FILE reaches every
     * connection, also those of HLS/DASH segments and keys. */
    ca_bundle_setup(g_ca_file);

    process_suburi(&playbackFiles);

    g_player = malloc(sizeof(Context_t));
    if(NULL == g_player)
    {
        printf("g_player allocate error\n");
        exit(1);
    }
    
    pthread_mutex_init(&playbackStartMtx, NULL);
    do 
    {
        int flags = 0;
        
        if (pipe(g_pfd) == -1)
            break;
        
        /* Make read and write ends of pipe nonblocking */
        if ((flags = fcntl(g_pfd[0], F_GETFL)) == -1)
            break;
        
        /* Make read end nonblocking */
        flags |= O_NONBLOCK;
        if (fcntl(g_pfd[0], F_SETFL, flags) == -1)
            break;
        
        if ((flags = fcntl(g_pfd[1], F_GETFL)) == -1)
            break;
        
        /* Make write end nonblocking */
        flags |= O_NONBLOCK;
        if (fcntl(g_pfd[1], F_SETFL, flags) == -1)
            break;
        
        if(0 == pthread_create(&termThread, NULL, TermThreadFun, NULL))
            isTermThreadStarted = 1;
    } while(0);

    {
        pthread_t signalThread;
        if (0 == pthread_create(&signalThread, NULL, StopSignalThreadFun, NULL))
            pthread_detach(signalThread);
    }
    
    g_player->playback    = &PlaybackHandler;
    g_player->output      = &OutputHandler;
    g_player->container   = &ContainerHandler;
    g_player->manager     = &ManagerHandler;

    // make sure to stop when the parent dies (see StopSignalThreadFun)
    prctl(PR_SET_PDEATHSIG, PARENT_DEATH_SIGNAL);

    SetBuffering();
    StdinThreadStart();

    //Registering output devices
    g_player->output->Command(g_player, OUTPUT_ADD, "audio");
    g_player->output->Command(g_player, OUTPUT_ADD, "video");
    g_player->output->Command(g_player, OUTPUT_ADD, "subtitle");
    
    //Set LINUX DVB additional write buffer size 
    if (linuxDvbBufferSizeMB)
        g_player->output->Command(g_player, OUTPUT_SET_BUFFER_SIZE, &linuxDvbBufferSizeMB);
    
    /* the output keeps it for the start of the decoder */
    if (OUTPUT_AUDIO_CHANNEL_STEREO != g_audio_channel)
        SelectAudioChannel(g_audio_channel);

    g_player->manager->video->Command(g_player, MANAGER_REGISTER_UPDATED_TRACK_INFO, UpdateVideoTrack);
    if (strncmp(playbackFiles.szFirstFile, "rtmp", 4) && strncmp(playbackFiles.szFirstFile, "ffrtmp", 6))
    {
        g_player->playback->noprobe = 1;
    }

    /*
    PLAYBACK_OPEN can block (possibly forever) when for example network source is not available or returns invalid data and it is not possible to guess used codec (simulated using hls)
    During this state it is not possible to stop the player

    This dirty hack solves this by running PLAYBACK_OPEN in a thread and runs a very simple loop that waits only for quit command from ServiceApp.
    It is not possible to run full command loop, because we don't have enough info about stream in this state and some commands will crash.

    This is very ugly solution, but it works ...
    */

//    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_OPEN, &playbackFiles);
    {
    	pthread_t itid;
    	pthread_attr_t tattr;
    	PlaybackInitParams_t params;

    	memset( &params, 0, sizeof( PlaybackInitParams_t ));
    	params.playbackFiles = &playbackFiles;
        if (pipe(g_openDonePfd) == -1)
        {
            g_openDonePfd[0] = g_openDonePfd[1] = -1;
        }
    	pthread_attr_init(&tattr);
    	pthread_attr_setdetachstate(&tattr, 1);
    	pthread_create(&itid, &tattr, (void * (*)(void *)) PlaybackInitThread, (void *) &params);

    	while( __atomic_load_n(&params.init_finished, __ATOMIC_ACQUIRE) == 0 )
    	{
            if( NULL == ReadCommand(argvBuff, sizeof(argvBuff)-1) )
            {
                /* wait for data or the end of the open - max 1s */
                kbhit_fd(1000, g_openDonePfd[0]);
                continue;
            }

            if(0 == argvBuff[0])
            {
                continue;
            }

            switch(argvBuff[0])
            {
				case 'q':
				{
					/* Stop during the open: the interrupt callback of the
					 * open reports the stop, so FFmpeg leaves a wait on the
					 * network and the open thread returns; only then the
					 * process ends. exit() while that thread was still in
					 * FFmpeg ran the exit handlers beside it. An open that
					 * the callback cannot end (name resolution, a FIFO
					 * without a writer) is cancelled after the wait, as
					 * before. The exit code stays 11. */
					PlaybackDieNow(1);
					if (!WaitOpenEnd(&params, OPEN_STOP_WAIT_MS))
					{
						printf("open not ended %d ms after q, cancelled\n", OPEN_STOP_WAIT_MS);
						pthread_cancel(itid);
					}
					exit(11);
				}
				break;

				case 'd':
				{
					/* dk: CENC keys, the opening thread may wait for them */
					if ('k' == argvBuff[1])
					{
						ffmpeg_drm_key_command(argvBuff);
					}
				}
				break;

				default:
					break;
            }
    	}

    	commandRetVal = params.ret;
    }

    {
        /* the uri is not cut: every byte takes at most 6 */
        size_t fileSize = strlen(playbackFiles.szFirstFile) * 6 + 1;
        char *file = malloc(fileSize);

        E2iSendMsg("{\"PLAYBACK_OPEN\":{\"OutputName\":\"%s\", \"file\":\"%s\", \"sts\":%d}}\n", g_player->output->Name, file ? JsonEscape(file, fileSize, playbackFiles.szFirstFile) : "", commandRetVal);
        free(file);
    }
    if(commandRetVal < 0)
    {
        if(NULL != g_player)
        {
            free(g_player);
        }
        return 10;
    }
    
    {
        pthread_mutex_lock(&playbackStartMtx);
        isPlaybackStarted = 1;
        pthread_mutex_unlock(&playbackStartMtx);
        
        commandRetVal = g_player->output->Command(g_player, OUTPUT_OPEN, NULL);
        E2iSendMsg("{\"OUTPUT_OPEN\":{\"sts\":%d}}\n", commandRetVal);
        commandRetVal = g_player->playback->Command(g_player, PLAYBACK_PLAY, NULL);
        E2iSendMsg("{\"PLAYBACK_PLAY\":{\"sts\":%d}}\n", commandRetVal);
        
        if (g_player->playback->isPlaying)
        {
            PlaybackDieNowRegisterCallback(TerminateWakeUp);

            HandleTracks(g_player->manager->video, (PlaybackCmd_t)-1, "vc");
            HandleTracks(g_player->manager->audio, (PlaybackCmd_t)-1, "al");
            if (audioTrackIdx >= 0)
            {
                static char cmd[128] = ""; // static to not allocate on stack
                sprintf(cmd, "ai%d\n", audioTrackIdx);
                HandleTracks(g_player->manager->audio, PLAYBACK_SWITCH_AUDIO, cmd);
            }
            HandleTracks(g_player->manager->audio, (PlaybackCmd_t)-1, "ac");
            
            
            HandleTracks(g_player->manager->subtitle, (PlaybackCmd_t)-1, "sl");
            if (subtitleTrackIdx >= 0)
            {
                static char cmd[128] = ""; // static to not allocate on stack
                sprintf(cmd, "si%d\n", subtitleTrackIdx);
                HandleTracks(g_player->manager->subtitle, PLAYBACK_SWITCH_SUBTITLE, cmd);
            }
            HandleTracks(g_player->manager->subtitle, (PlaybackCmd_t)-1, "sc");
        }

        while(g_player->playback->isPlaying && 0 == PlaybackDieNow(0))
        {
            /* CENC: the audio track had no key and another one has, or the
             * video has none (FF_ERROR is out): as the commands a<id> and q */
            {
                int32_t drmAudio = ffmpeg_drm_key_audio_switch();
                if (drmAudio >= 0)
                {
                    char cmd[32];
                    snprintf(cmd, sizeof(cmd), "a%d\n", drmAudio);
                    HandleTracks(g_player->manager->audio, PLAYBACK_SWITCH_AUDIO, cmd);
                    HandleTracks(g_player->manager->audio, (PlaybackCmd_t)-1, "ac");
                }
                if (ffmpeg_drm_key_stop())
                {
                    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_STOP, NULL);
                    E2iSendMsg("{\"PLAYBACK_STOP\":{\"sts\":%d}}\n", commandRetVal);
                    continue;
                }
            }

            /* we made fgets non blocking */
            if( NULL == ReadCommand(argvBuff, sizeof(argvBuff)-1) )
            {
                int eac3Restart = LinuxDvbEac3RestartState();
                int subtitleInfoGen = container_ffmpeg_subtitle_info_gen();

                /* a track has been added while playing: the list again */
                if (subtitleInfoGen != subtitleListGen)
                {
                    subtitleListGen = subtitleInfoGen;
                    HandleTracks(g_player->manager->subtitle, (PlaybackCmd_t)-1, "sl");
                }
                if (2 == eac3Restart)
                {
                    Eac3PassthroughRestart();
                    continue;
                }
                /* wait for data - max 1s, 20 ms while a DD+ restart is close */
                kbhit(1 == eac3Restart ? 20 : 1000);
                continue;
            }

            if(0 == argvBuff[0])
            {
                continue;
            }
            
            switch(argvBuff[0])
            {
            case 'v':
            {
                HandleTracks(g_player->manager->video, (PlaybackCmd_t)-1, argvBuff);
            break;
            }
            case 'a': 
            {
                /* am<n>: left (0), stereo (1) or right (2) of dual mono audio;
                 * am alone reports it. sts is the one of the decoder of the
                 * box, software decoded audio follows the selection anyway. */
                if ('m' == argvBuff[1])
                {
                    int channel = -1;
                    commandRetVal = 0;
                    if (1 == sscanf(argvBuff + 2, "%d", &channel))
                    {
                        if (channel >= OUTPUT_AUDIO_CHANNEL_LEFT && channel <= OUTPUT_AUDIO_CHANNEL_RIGHT)
                            commandRetVal = SelectAudioChannel(channel);
                        else
                            commandRetVal = -1;
                    }
                    E2iSendMsg("{\"a_m\":{\"m\":%d,\"sts\":%d}}\n", g_audio_channel, commandRetVal);
                    break;
                }
                HandleTracks(g_player->manager->audio, PLAYBACK_SWITCH_AUDIO, argvBuff);
            break;
            }
            case 's': 
            {
                HandleTracks(g_player->manager->subtitle, PLAYBACK_SWITCH_SUBTITLE, argvBuff);
            break;
            }
            case 'q':
            {
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_STOP, NULL);
                E2iSendMsg("{\"PLAYBACK_STOP\":{\"sts\":%d}}\n", commandRetVal);
                break;
            }
            case 'd':
            {
                /* dk <kid>:<key> ...: CENC keys, the reading thread may wait for them */
                if ('k' == argvBuff[1])
                {
                    ffmpeg_drm_key_command(argvBuff);
                }
                break;
            }
            case 'c':
            {
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_CONTINUE, NULL);
                E2iSendMsg("{\"PLAYBACK_CONTINUE\":{\"sts\":%d}}\n", commandRetVal);
                break;
            }
            case 'p':
            {
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_PAUSE, NULL);
                E2iSendMsg("{\"PLAYBACK_PAUSE\":{\"sts\":%d}}\n", commandRetVal);
                break;
            }
            case 'm':
            {
                int speed = 0;
                sscanf(argvBuff+1, "%d", &speed);

                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_SLOWMOTION, &speed);
                E2iSendMsg("{\"PLAYBACK_SLOWMOTION\":{\"speed\":%d, \"sts\":%d}}\n", speed, commandRetVal);
                break;
            }
            case 'o':
            {
                int flags = 0;
                if( 1 == sscanf(argvBuff+1, "%d", &flags) )
                {
                    progressive_playback_set(flags);
                    E2iSendMsg("{\"PROGRESSIVE_DOWNLOAD\":{\"flags\":%d, \"sts\":0}}\n", flags);
                }
                break;
            }
            case 'f':
            {
                int speed = 0;
                sscanf(argvBuff+1, "%d", &speed);

                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_FASTFORWARD, &speed);
                E2iSendMsg("{\"PLAYBACK_FASTFORWARD\":{\"speed\":%d, \"sts\":%d}}\n", speed, commandRetVal);
                break;
            }
            case 'b': 
            {
                int speed = 0;
                sscanf(argvBuff+1, "%d", &speed);

                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_FASTBACKWARD, &speed);
                E2iSendMsg("{\"PLAYBACK_FASTBACKWARD\":{\"speed\":%d, \"sts\":%d}}\n", speed, commandRetVal);
                break;
            }
            case 'g':
            {
                int32_t gotoPos = 0;
                int64_t length = 0;
                int32_t lengthInt = 0;
                int64_t sec = 0;
                int8_t force = ('f' == argvBuff[1]) ? 1 : 0; // f - force, c - check
                
                sscanf(argvBuff+2, "%d", &gotoPos);
                if(0 <= gotoPos || force)
                {
                    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_LENGTH, (void*)&length);
                    E2iSendMsg("{\"PLAYBACK_LENGTH\":{\"length\":%"PRId64", \"sts\":%d}}\n", length, commandRetVal);

                    lengthInt = (int32_t)length;
                    if(10 <= lengthInt || force)
                    {
                        sec = gotoPos;
                        if(!force && gotoPos >= lengthInt)
                        {
                            sec = lengthInt - 10;
                        }
                        
                        commandRetVal = g_player->playback->Command(g_player, PLAYBACK_SEEK_ABS, (void*)&sec);
                        E2iSendMsg("{\"PLAYBACK_SEEK_ABS\":{\"sec\":%"PRId64", \"sts\":%d}}\n", sec, commandRetVal);
                    }
                }
                break;
            }
            case 'k': 
            {
                int32_t seek = 0;
                int64_t length = 0;
                int32_t lengthInt = 0;
                int64_t sec = 0;
                int64_t pts = 0;
                int32_t CurrentSec = 0;
                int8_t force = ('f' == argvBuff[1]) ? 1 : 0; // f - force, c - check
                
                sscanf(argvBuff+2, "%d", &seek);
                
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_PTS, &pts);
                CurrentSec = (int32_t)(pts / 90000);
                if (0 == commandRetVal)
                {
                    E2iSendMsg("{\"J\":{\"ms\":%"PRId64"}}\n", pts / 90);
                }
                if(0 == commandRetVal || force)
                {                    
                    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_LENGTH, (void*)&length);
                    E2iSendMsg("{\"PLAYBACK_LENGTH\":{\"length\":%"PRId64", \"sts\":%d}}\n", length, commandRetVal);
                    
                    lengthInt = (int32_t)length;
                    if(10 <= lengthInt || force )
                    {
                        int32_t ergSec = CurrentSec + seek;
                        if(!force && 0 > ergSec)
                        {
                            sec = CurrentSec * -1; // jump to start position
                        }
                        else if(!force && ergSec >= lengthInt)
                        {
                            sec = (lengthInt - CurrentSec) - 5;
                            if(0 > sec)
                            {
                                sec = 0; // no jump we are at the end
                            }
                        }
                        else
                        {
                            sec = seek;
                        }
                    }
                    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_SEEK, (void*)&sec);
                    E2iSendMsg("{\"PLAYBACK_SEEK\":{\"sec\":%"PRId64", \"sts\":%d}}\n", sec, commandRetVal);
                }
                break;
            }
            case 'l': 
            {
                int64_t length = 0;
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_LENGTH, (void*)&length);
                E2iSendMsg("{\"PLAYBACK_LENGTH\":{\"length\":%"PRId64", \"sts\":%d}}\n", length, commandRetVal);
                break;
            }
            case 'j': 
            {
                int64_t pts = 0;
                commandRetVal = g_player->playback->Command(g_player, PLAYBACK_PTS, &pts);
                if (0 == commandRetVal)
                {
                    int64_t lastPts = 0;
                    commandRetVal = 1;
                    if (g_player->container && g_player->container->selectedContainer)
                    {
                        commandRetVal = g_player->container->selectedContainer->Command(g_player->container, CONTAINER_LAST_PTS, &lastPts);
                    }
                    
                    if (0 == commandRetVal && lastPts != INVALID_PTS_VALUE)
                    {
                        E2iSendMsg("{\"J\":{\"ms\":%"PRId64",\"lms\":%"PRId64"}}\n", pts / 90, lastPts / 90);
                    }
                    else
                    {
                        E2iSendMsg("{\"J\":{\"ms\":%"PRId64"}}\n", pts / 90);
                    }
                }

                pts_check_nr++;

                if( pts_check_nr > 10 )
                {
                    pts_check_nr = 0;
                    int64_t length = 0;
                    commandRetVal = g_player->playback->Command(g_player, PLAYBACK_LENGTH, (void*)&length);
                    E2iSendMsg("{\"PLAYBACK_LENGTH\":{\"length\":%"PRId64", \"sts\":%d}}\n", length, commandRetVal);
                }

                break;
            }
            case 'i':
            {
                PlaybackHandler_t *ptrP = g_player->playback;
                if(ptrP)
                {
                    E2iSendMsg("{\"PLAYBACK_INFO\":{ \"isPlaying\":%s, \"isPaused\":%s, \"isForwarding\":%s, \"isSeeking\":%s, \"isCreationPhase\":%s,", \
                    DUMP_BOOL(ptrP->isPlaying), DUMP_BOOL(ptrP->isPaused), DUMP_BOOL(ptrP->isForwarding), DUMP_BOOL(ptrP->isSeeking), DUMP_BOOL(ptrP->isCreationPhase) );
                    E2iSendMsg("\"BackWard\":%d, \"SlowMotion\":%d, \"Speed\":%d, \"AVSync\":%d,", ptrP->BackWard, ptrP->SlowMotion, ptrP->Speed, ptrP->AVSync);
                    E2iSendMsg(" \"isVideo\":%s, \"isAudio\":%s, \"isSubtitle\":%s, \"isDvbSubtitle\":%s, \"isTeletext\":%s, \"mayWriteToFramebuffer\":%s, \"abortRequested\":%s }}\n", \
                    DUMP_BOOL(ptrP->isVideo), DUMP_BOOL(ptrP->isAudio), DUMP_BOOL(0), DUMP_BOOL(0), DUMP_BOOL(0), DUMP_BOOL(0), DUMP_BOOL(ptrP->abortRequested) );
                }
                
                break;
            }
            case 'n':
            {
                if( '1' == argvBuff[1] || '0' == argvBuff[1] )
                {
                    PlaybackHandler_t *ptrP = g_player->playback;
                    if(ptrP)
                    {
                        ptrP->isLoopMode = '1' == argvBuff[1] ? 1 : 0;
                        E2iSendMsg("{\"N\":{ \"isLoop\":%s }}\n", DUMP_BOOL(ptrP->isLoopMode));
                    }
                }
                break;
            }
            
            default: 
            {
                break;
            }
            }
        }

        g_player->output->Command(g_player, OUTPUT_CLOSE, NULL);
    }
    
    if(NULL != g_player)
    {
        int32_t playThreadRuns = 0;

        if (g_player->container && g_player->container->selectedContainer)
        {
            g_player->container->selectedContainer->Command(g_player, CONTAINER_STATUS, &playThreadRuns);
        }
        if (playThreadRuns)
        {
            /* The play thread has not ended within the wait of the stop (a
             * read the interrupt callback cannot end). It still uses
             * g_player and FFmpeg: no free, and no exit handlers beside it. */
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
        free(g_player);
    }
    
    if (isTermThreadStarted && 1 == write(g_pfd[1], "x", 1))
    {
        pthread_join(termThread, NULL);
    }
    
    pthread_mutex_destroy(&playbackStartMtx);
    
    close(g_pfd[0]);
    close(g_pfd[1]);
    
    exit(0);
}
