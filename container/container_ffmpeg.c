/*
 * Container handling for all stream's handled by ffmpeg
 * konfetti 2010; based on code from crow
 *
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

 /* Support Large File */
#define _FILE_OFFSET_BITS 64

/* ***************************** */
/* Includes                      */
/* ***************************** */
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <memory.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <ctype.h>

#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
//#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
//#endif

#include <ffmpeg/mpeg4audio.h>

#include "common.h"
#include "misc.h"
#include "debug.h"
#include "aac.h"
#include "pcm.h"
#include "pcm_export.h"
#include "pts_clock.h"
#include "ffmpeg_metadata.h"
#include "hevc_hdr.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */

#define HAVE_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 0, 100))

/* Some STB with old kernels have problem with default 
 * read/write functions in ffmpeg which use open/read
 * due to this we set own which use fopen/fread from
 * std library.
 */
#define USE_CUSTOM_IO

/* Error Constants */
#define cERR_CONTAINER_FFMPEG_NO_ERROR        0
#define cERR_CONTAINER_FFMPEG_INIT           -1
#define cERR_CONTAINER_FFMPEG_NOT_SUPPORTED  -2
#define cERR_CONTAINER_FFMPEG_INVALID_FILE   -3
#define cERR_CONTAINER_FFMPEG_RUNNING        -4
#define cERR_CONTAINER_FFMPEG_NOMEM          -5
#define cERR_CONTAINER_FFMPEG_OPEN           -6
#define cERR_CONTAINER_FFMPEG_STREAM         -7
#define cERR_CONTAINER_FFMPEG_NULL           -8
#define cERR_CONTAINER_FFMPEG_ERR            -9
#define cERR_CONTAINER_FFMPEG_END_OF_FILE    -10

#define IPTV_AV_CONTEXT_MAX_NUM 2

#define DEFAULT_HTTP_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36"
/* ***************************** */
/* Types                         */
/* ***************************** */
typedef enum {RTMP_NATIVE, RTMP_LIBRTMP, RTMP_NONE} eRTMPProtoImplType;

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

static pthread_rwlock_t mutex = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t seek_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t PlayThread;
static int32_t hasPlayThreadStarted = 0;

static AVFormatContext *avContextTab[IPTV_AV_CONTEXT_MAX_NUM] = {NULL, NULL};
static int32_t use_custom_io[IPTV_AV_CONTEXT_MAX_NUM] = {0, 0};
static AVDictionary *g_avio_opts = NULL;

static uint8_t isContainerRunning = 0;

static int64_t latestPts = 0;

static int32_t restart_audio_resampling = 0;

static int64_t g_seek_target_seconds = 0;
static bool g_do_seek_target_seconds = false;

#ifdef HAVE_DREAMNEXTGEN
/* After a seek audio starts with the first video keyframe: the kernel syncs
 * the audio to the clock the video sets, audio from before the keyframe stays
 * late by up to a GOP. Packets before the keyframe, and the count to give up
 * on streams that mark no keyframes. */
static bool seekWaitVideoKey = false;
static int64_t seekVideoKeyPts = INVALID_PTS_VALUE;
static int32_t seekVideoSkipped = 0;

/* Trick play: the decoder plays at its normal pace, it gets keyframes only.
 * Each one is a seek from the last one by speed times TRICK_STEP_90K, its PTS
 * are scaled by the speed from the start; backwards they are mirrored and
 * keep rising. Audio and subtitles are not fed. */
#define TRICK_STEP_90K 27000
static int32_t trickSpeed = 0;              /* >1 forwards, <0 backwards, 0 off */
static bool trickStep = false;              /* the seek to the next keyframe is due */
static int64_t trickAnchorPts = INVALID_PTS_VALUE;  /* stream PTS at the start */
static int64_t trickBasePts = INVALID_PTS_VALUE;    /* output PTS at the start */
static int64_t trickOutPts = INVALID_PTS_VALUE;     /* output PTS of the last keyframe */
static int64_t trickKeyPts = INVALID_PTS_VALUE;     /* stream PTS of the last keyframe */
static int64_t trickDueNs = 0;              /* the last keyframe is on screen then */
static int64_t trickStartNs = 0;
static int32_t trickMiss = 0;               /* seeks that found no keyframe beyond the last one */
#endif
static bool g_zero_seek = false;
/* relative seek: the decoder position it counts from (90 kHz) and its offset */
static int64_t g_seek_from_pts = INVALID_PTS_VALUE;
static int64_t g_seek_offset_us = 0;
static void *g_stamp;
static PtsClock ptsClock;
static int dashAudioResetPending;
static int pcm_audio_export_enabled;
static PcmExport pcmExport = {.fd = -1};

void pcm_audio_export_set(int enabled)
{
    pcm_audio_export_enabled = enabled != 0;
}

/* ***************************** */
/* Prototypes                    */
/* ***************************** */
static int32_t container_ffmpeg_seek_bytes(off_t pos);
static int32_t container_ffmpeg_seek(Context_t *context, int64_t sec, uint8_t absolute);
static int32_t container_ffmpeg_get_length(Context_t *context, int64_t *length);
static int32_t container_ffmpeg_stop(Context_t *context);
static int64_t calcPts(uint32_t avContextIdx, AVStream *stream, int64_t pts);
static int64_t calcDts(uint32_t avContextIdx, AVStream *stream, int64_t pts);
static int64_t calcConvertedPts(AVStream *stream, AVRational time_base, int64_t pts, int dts);
static int64_t doCalcPts(int64_t start_time, const AVRational time_base, int64_t pts);
void LinuxDvbBuffSetStamp(void *stamp);

/* Progressive playback means that we play local file
 * but this local file can grows up, for example 
 * we start playback before download was finished 
 */
static int32_t progressive_playback = 0;
void progressive_playback_set(int32_t val)
{
    progressive_playback = val;
}

/* --start: the position the play thread seeks to before it reads the first
 * packet, in us; 0 for none */
static int64_t g_start_position_us = 0;

void start_position_set(int64_t us)
{
    g_start_position_us = us;
}

#define getMutex(...) getMutex_uni(0, ##__VA_ARGS__)
#define getMutex_wr(...) getMutex_uni(1, ##__VA_ARGS__)

static void getMutex_uni(int type, const char *filename __attribute__((unused)), const char *function __attribute__((unused)), int32_t line)
{
    ffmpeg_printf(100, "::%d requesting mutex\n", line);
    if(type == 0)
    {
        pthread_rwlock_rdlock(&mutex);
    }
    else
    {
        pthread_rwlock_wrlock(&mutex);
    }

    ffmpeg_printf(100, "::%d received mutex\n", line);
}

static void releaseMutex(const char *filename __attribute__((unused)), const char *function __attribute__((unused)), int32_t line) 
{
    pthread_rwlock_unlock(&mutex);

    ffmpeg_printf(100, "::%d released mutex\n", line);
}

static void getSeekMutex()
{
    pthread_mutex_lock(&seek_mutex);
}

static void releaseSeekMutex() 
{
    pthread_mutex_unlock(&seek_mutex);
}

#include "output_pacing.c"

typedef int32_t (* Write_FN) (void  *, void *);

static int32_t Write(Write_FN WriteFun, void *context, void *privateData, int64_t pts)
{
    /* Because Write is blocking we will release mutex which protect 
     * avformat structures, during write time
     */
    int32_t ret = 0;
    Context_t *ctx = context;
    uint8_t **extra;
    uint32_t size;
    uint8_t *saved, *copy = NULL;
    if (WriteFun == ctx->output->subtitle->Write)
    {
        SubtitleOut_t *out = privateData;
        extra = &out->extradata;
        size = out->extralen;
    }
    else
    {
        AudioVideoOut_t *out = privateData;
        extra = &out->extradata;
        size = out->extralen;
    }
    saved = *extra;
    if (saved && size)
    {
        copy = malloc(size);
        if (!copy)
        {
            ffmpeg_err("allocating output codec data failed\n");
            return -1;
        }
        memcpy(copy, saved, size);
        *extra = copy;
    }
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    if (OutputPacingLeadMs() > 0)
    {
        if (ctx->output->video && WriteFun == ctx->output->video->Write)
            OutputPacingWait(ctx, PACING_STREAM_VIDEO, pts);
        else if (ctx->output->audio && WriteFun == ctx->output->audio->Write)
            OutputPacingWait(ctx, PACING_STREAM_AUDIO, pts);
    }
    ret = WriteFun(context, privateData);
    getMutex(__FILE__, __FUNCTION__,__LINE__);
    *extra = saved;
    free(copy);
    return ret;
}


#include "buff_ffmpeg.c"
#include "wrapped_ffmpeg.c"
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
#include "mpeg4p2_ffmpeg.c"
#endif

#ifdef HAVE_FLV2MPEG4_CONVERTER
#include "flv2mpeg4_ffmpeg.c"
#endif

/* changes when a closed caption track is added, read by the command thread */
static int ccTrackGen = 0;

int container_ffmpeg_subtitle_info_gen(void)
{
    return __atomic_load_n(&ccTrackGen, __ATOMIC_RELAXED);
}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 64, 101)
#define HAVE_CLOSED_CAPTIONS 1
#include "cc_ffmpeg.c"
#endif

/* This is also bad solution 
 * such configuration should passed maybe 
 * via struct 
 */
static int32_t wma_software_decode = 0;
static int32_t aac_software_decode = 0;
#ifdef __sh__
static int32_t aac_latm_software_decode = 1;
#else
static int32_t aac_latm_software_decode = 0;
#endif

/* Max. allowed difference (ms) between stream and smoothed AAC/DTS PTS
 * (a DASH remux can leave the stream PTS 80 ms off the sample count),
 * 0 - smoothing disabled.
 */
static int32_t aac_pts_dejitter_ms = 200;

static int32_t ac3_software_decode = 0;
static int32_t eac3_software_decode = 0;
static int32_t dts_software_decode = 0;
static int32_t amr_software_decode = 1;
static int32_t vorbis_software_decode = 1;
static int32_t opus_software_decode = 1;

static int32_t pcm_resampling = 1;
static int32_t stereo_software_decoder = 0;
/* left (0), stereo (1) or right (2) of dual mono audio for the software
 * decoded PCM; set from the command loop while the play thread reads it */
static volatile int32_t pcm_audio_channel = 1;
static int32_t insert_pcm_as_lpcm = 0;
static int32_t mp3_software_decode = 0;
static int32_t rtmp_proto_impl = 0; // 0 - auto, 1 - native, 2 - librtmp

static int32_t g_sel_program_id = -1;
static int32_t g_quality_mode;
static int32_t g_default_audio_only;
static int32_t g_h264_max_width, g_h264_max_height;

#ifdef HAVE_FLV2MPEG4_CONVERTER
static int32_t flv2mpeg4_converter = 1;
#else
static int32_t flv2mpeg4_converter = 0;
#endif

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

static void ffmpeg_silen_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    return;
}

void sel_program_id_set(const int32_t val)
{
    g_sel_program_id = val;
}

void adaptive_quality_set(int32_t mode, int32_t default_audio)
{
    g_quality_mode = mode;
    g_default_audio_only = default_audio;
}

void h264_resolution_limit_set(int32_t width, int32_t height)
{
    g_h264_max_width = width;
    g_h264_max_height = height;
}

static int video_allowed(AVStream *stream)
{
    AVCodecParameters *par = get_codecpar(stream);
    return par->codec_type == AVMEDIA_TYPE_VIDEO &&
           !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
           (par->codec_id != AV_CODEC_ID_H264 ||
            ((!g_h264_max_width || par->width <= g_h264_max_width) &&
             (!g_h264_max_height || par->height <= g_h264_max_height)));
}

static char* VideoCodec2Encoding(int32_t codec_id, int32_t *version);

/* video is never decoded here: a codec without an encoding (AV1, VVC, ...)
 * gets no track and no writer, so a variant with it has no picture */
static int video_has_writer(AVStream *stream)
{
    int32_t version = 0;
    return VideoCodec2Encoding((int32_t)get_codecpar(stream)->codec_id, &version) != NULL;
}

/* set for the second open of an HLS input: hls_quality_mode is not passed,
 * the demuxer opens every variant and select_program() chooses */
static int32_t g_hls_all_variants;

/* With hls_quality_mode the HLS demuxer opens one variant by bandwidth alone
 * and leaves the programs of the others empty. True when the playlist has
 * more variants and no video stream of the opened one has a writer. */
static int hls_preselection_without_writer(AVFormatContext *av)
{
    int video = 0;
    unsigned int i;
    if (av->nb_programs < 2)
        return 0;
    for (i = 0; i < av->nb_streams; i++)
    {
        AVStream *stream = av->streams[i];
        if (get_codecpar(stream)->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            continue;
        if (video_has_writer(stream))
            return 0;
        video = 1;
    }
    return video;
}

static int64_t variant_bandwidth(AVDictionary *metadata)
{
    AVDictionaryEntry *tag = av_dict_get(metadata, "variant_bitrate", NULL, 0);
    char *end;
    long long value;
    if (!tag)
        return 0;
    errno = 0;
    value = strtoll(tag->value, &end, 10);
    if (errno || *end || value < 0)
    {
        ffmpeg_err("invalid variant bandwidth: %s\n", tag->value);
        return 0;
    }
    return value;
}

static AVProgram *select_program(AVFormatContext *av)
{
    AVProgram *best = NULL, *audio = NULL;
    int64_t bandwidth = 0;
    int best_sized = 0;
    int best_writer = 0;
    unsigned int i, j;
    for (i = 0; i < av->nb_programs; i++)
    {
        AVProgram *p = av->programs[i];
        int64_t candidate;
        int has_video = 0, has_audio = 0, sized;
        int writer;
        if (!p->nb_stream_indexes)
            continue;
        if (g_sel_program_id > 0)
        {
            if (p->id == g_sel_program_id)
                return p;
            continue;
        }
        for (j = 0; j < p->nb_stream_indexes; j++)
        {
            AVStream *stream = av->streams[p->stream_index[j]];
            has_audio |= get_codecpar(stream)->codec_type == AVMEDIA_TYPE_AUDIO;
            has_video |= get_codecpar(stream)->codec_type == AVMEDIA_TYPE_VIDEO &&
                         !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC);
            if (video_allowed(stream))
                break;
        }
        if (j == p->nb_stream_indexes)
        {
            if (!has_video && has_audio && !audio)
                audio = p;
            continue;
        }
        /* a program without a writer for its video is taken only when no
         * program has one */
        writer = video_has_writer(av->streams[p->stream_index[j]]);
        if (best && writer != best_writer)
        {
            ffmpeg_printf(1, "program %d skipped: no writer for its video codec\n", writer ? best->id : p->id);
            if (!writer)
                continue;
            best = NULL;
        }
        candidate = variant_bandwidth(p->metadata);
        /* no picture size: the video of this program was never probed
         * (scrambled program of a multi program TS) */
        sized = get_codecpar(av->streams[p->stream_index[j]])->width > 0;
        if (!best || (g_quality_mode == 1 ? candidate < bandwidth : candidate > bandwidth) ||
            (candidate == bandwidth && sized && !best_sized))
        {
            best = p;
            bandwidth = candidate;
            best_sized = sized;
            best_writer = writer;
        }
    }
    return best ? best : (g_sel_program_id > 0 ? NULL : audio);
}

static int select_dash_video(AVFormatContext *av)
{
    int best = -1;
    int64_t bandwidth = 0, resolution = 0;
    int best_writer = 0;
    unsigned int i;
    for (i = 0; i < av->nb_streams; i++)
    {
        AVStream *stream = av->streams[i];
        AVCodecParameters *par = get_codecpar(stream);
        int64_t candidate, pixels;
        int writer;
        if (!video_allowed(stream))
            continue;
        /* a representation without a writer is taken only when none has one */
        writer = video_has_writer(stream);
        if (best >= 0 && writer != best_writer)
        {
            ffmpeg_printf(1, "video stream %d skipped: no writer for its codec\n", writer ? best : (int)i);
            if (!writer)
                continue;
            best = -1;
        }
        candidate = variant_bandwidth(stream->metadata);
        pixels = (int64_t)par->width * par->height;
        if (best < 0 ||
            (g_quality_mode == 1 ? candidate < bandwidth : candidate > bandwidth) ||
            (candidate == bandwidth && (g_quality_mode == 1 ? pixels < resolution : pixels > resolution)))
        {
            best = i;
            bandwidth = candidate;
            resolution = pixels;
            best_writer = writer;
        }
    }
    return best;
}

static int subtitle_is_forced(AVStream *stream)
{
    AVDictionaryEntry *title;
    const char *p;
    if (stream->disposition & AV_DISPOSITION_FORCED)
        return 1;
    title = av_dict_get(stream->metadata, "title", NULL, 0);
    if (!title)
        return 0;
    for (p = title->value; *p; p++)
    {
        if ((p == title->value || !isalnum((unsigned char)p[-1])) &&
            !strncasecmp(p, "forced", 6) && !isalnum((unsigned char)p[6]))
            return 1;
    }
    return 0;
}

static int is_hls_input(const AVFormatContext *av)
{
    return av->iformat &&
        (av_match_name("hls", av->iformat->name) ||
         av_match_name("applehttp", av->iformat->name));
}

/* Title of a track for display, NULL if it has none. The HLS demuxer exports
 * the NAME of a rendition (#EXT-X-MEDIA) as "comment". */
static char *stream_title(const AVFormatContext *av, const AVStream *stream)
{
    AVDictionaryEntry *title = av_dict_get(stream->metadata, "title", NULL, 0);

    if (!title && is_hls_input(av))
    {
        title = av_dict_get(stream->metadata, "comment", NULL, 0);
    }
    return title ? title->value : NULL;
}

static int packet_matches_track(const Track_t *track, unsigned int context_index,
                                const AVStream *stream)
{
    return track && track->AVIdx == context_index && track->stream == stream;
}

/* HDR type of the selected video stream, HEVC_HDR_*. The play thread finds it
 * in the parameter sets and SEI of the stream, the track info message reads
 * it from other threads. */
static HevcHdrDetector hevcHdrDetector;
static const AVStream *hevcHdrStream = NULL;
static int32_t hevcHdrType = HEVC_HDR_SDR;

int32_t container_ffmpeg_get_hdr_type(void)
{
    return __atomic_load_n(&hevcHdrType, __ATOMIC_RELAXED);
}

static void hevc_hdr_publish(Context_t *context, int type)
{
    if (type < HEVC_HDR_SDR || type > HEVC_HDR_GENERIC ||
        type == __atomic_load_n(&hevcHdrType, __ATOMIC_RELAXED))
        return;

    __atomic_store_n(&hevcHdrType, type, __ATOMIC_RELAXED);
    ffmpeg_printf(10, "HDR type of the video stream: %d\n", type);
    context->manager->video->Command(context, MANAGER_UPDATED_TRACK_INFO, NULL);
}

/* Looks at the packets of the selected video stream until the type is known;
 * hevc_hdr.c bounds how much of the stream that takes. */
static void hevc_hdr_update(Context_t *context, AVStream *stream, const AVPacket *packet)
{
    AVCodecParameters *par = get_codecpar(stream);

    if (hevcHdrStream != stream)
    {
        hevc_hdr_detector_reset(&hevcHdrDetector);
        hevcHdrStream = stream;
        hevc_hdr_publish(context, HEVC_HDR_SDR);
        if (par->codec_id != AV_CODEC_ID_HEVC)
        {
            hevcHdrDetector.done = 1;
        }
        else if (par->extradata && par->extradata_size > 0)
        {
            hevc_hdr_detector_configure(&hevcHdrDetector, par->extradata, (size_t)par->extradata_size);
            hevc_hdr_publish(context, hevc_hdr_detector_result(&hevcHdrDetector));
        }
    }

    if (!hevcHdrDetector.done && packet->data && packet->size > 0)
    {
        hevc_hdr_detector_feed(&hevcHdrDetector, packet->data, (size_t)packet->size);
        hevc_hdr_publish(context, hevc_hdr_detector_result(&hevcHdrDetector));
    }
}

/* The track ID of a stream. AVStream.id belongs to the demuxer, which may
 * find the stream of a packet by it (FLV: the track number, 0 for every
 * stream of a file without multitrack), so it is read here, never changed.
 * HLS IDs identify playlists, so embedded audio and video share one. */
static int stream_track_id(const AVFormatContext *av, const AVStream *stream)
{
    if (!stream->id || is_hls_input(av))
        return stream->index;
    return stream->id;
}

static void select_subtitle_stream(AVFormatContext *av, int id)
{
    unsigned int i;
    if (!av->iformat || (!is_hls_input(av) &&
                         strcmp(av->iformat->name, "dash")))
        return;
    for (i = 0; i < av->nb_streams; i++)
        if (get_codecpar(av->streams[i])->codec_type == AVMEDIA_TYPE_SUBTITLE)
            av->streams[i]->discard = stream_track_id(av, av->streams[i]) == id ?
                AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

void wma_software_decoder_set(const int32_t val)
{
    wma_software_decode = val;
}

void aac_software_decoder_set(const int32_t val)
{
    aac_software_decode = val;
}

void aac_latm_software_decoder_set(const int32_t val)
{
    aac_latm_software_decode = val;
}

void aac_pts_dejitter_set(const int32_t val)
{
    aac_pts_dejitter_ms = val > 0 ? val : 0;
}

void ac3_software_decoder_set(const int32_t val)
{
    ac3_software_decode = val;
}

void eac3_software_decoder_set(const int32_t val)
{
    eac3_software_decode = val;
}

void dts_software_decoder_set(const int32_t val)
{
    dts_software_decode = val;
}

void amr_software_decoder_set(const int32_t val)
{
    amr_software_decode = val;
}

void vorbis_software_decoder_set(const int32_t val)
{
    vorbis_software_decode = val;
}

void opus_software_decoder_set(const int32_t val)
{
    opus_software_decode = val;
}

void stereo_software_decoder_set(const int32_t val)
{
    stereo_software_decoder = val;
}

void pcm_audio_channel_set(const int32_t val)
{
    pcm_audio_channel = val;
}

/* Interleaved S16 stereo: the left or the right channel on both. */
static void pcm_select_channel(int16_t *samples, int32_t frames, int32_t channel)
{
    int32_t from = 0 == channel ? 0 : 1;
    int32_t i;
    if (1 == channel || 0 > channel || 2 < channel)
        return;
    for (i = 0; i < frames; i++)
        samples[2 * i + 1 - from] = samples[2 * i + from];
}

void insert_pcm_as_lpcm_set(const int32_t val)
{
    insert_pcm_as_lpcm = val;
}

void pcm_resampling_set(const int32_t val)
{
    pcm_resampling = val;
}

void mp3_software_decoder_set(const int32_t val)
{
    mp3_software_decode = val;
}

void rtmp_proto_impl_set(const int32_t val)
{
    rtmp_proto_impl = val;
}

void flv2mpeg4_converter_set(const int32_t val)
{
    flv2mpeg4_converter = val;
}

int32_t ffmpeg_av_dict_set(const char *key, const char *value, int32_t flags)
{
    return av_dict_set(&g_avio_opts, key, value, flags);
}

/* The video part of Codec2Encoding(): NULL for a codec without an encoding.
 * On its own, so that the variant selection can ask without the audio part. */
static char* VideoCodec2Encoding(int32_t codec_id, int32_t *version)
{
    switch (codec_id)
    {
    case AV_CODEC_ID_MPEG1VIDEO:
        return "V_MPEG1";
    case AV_CODEC_ID_MPEG2VIDEO:
        return "V_MPEG2";
    case AV_CODEC_ID_MJPEG:
        return "V_MJPEG";
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
        return "V_H263";
    case AV_CODEC_ID_FLV1:
        return flv2mpeg4_converter ? "V_MPEG4" : "V_FLV";
    case AV_CODEC_ID_VP5:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
        return "V_VP6";
    case AV_CODEC_ID_VP8:
        return "V_VP8";
#if LIBAVCODEC_VERSION_MAJOR > 54
    case AV_CODEC_ID_VP9:
        return "V_VP9";
#endif
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
        return "V_RV20";
    case AV_CODEC_ID_RV30:
        return "V_RV30";
    case AV_CODEC_ID_RV40:
        return "V_RV40";
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 21, 100)
    case AV_CODEC_ID_AVS2:
        return "V_AVS2";
#endif
    case AV_CODEC_ID_MPEG4:
        return "V_MPEG4";
#if LIBAVCODEC_VERSION_MAJOR < 53
    case AV_CODEC_ID_XVID:
#endif
#ifdef HAVE_DREAMNEXTGEN
    case AV_CODEC_ID_MSMPEG4V3:
        return "V_DIVX3";
#endif
    case AV_CODEC_ID_MSMPEG4V1:
    case AV_CODEC_ID_MSMPEG4V2:
#ifndef HAVE_DREAMNEXTGEN
    case AV_CODEC_ID_MSMPEG4V3:
#endif
        return "V_MPEG4"; //"V_DIVX3";
    case AV_CODEC_ID_WMV1:
        *version = 1;
        return "V_WMV";
    case AV_CODEC_ID_WMV2:
        *version = 2;
        return "V_WMV";
    case AV_CODEC_ID_WMV3:
        *version = 3;
        return "V_WMV";
    case AV_CODEC_ID_VC1:
        return "V_VC1";
    case AV_CODEC_ID_H264:
#if LIBAVCODEC_VERSION_MAJOR < 54
    case AV_CODEC_ID_FFH264:
#endif
        return "V_MPEG4/ISO/AVC";
#if LIBAVCODEC_VERSION_INT > AV_VERSION_INT(55, 92, 100)
    case AV_CODEC_ID_HEVC:
    // case AV_CODEC_ID_H265:
        return "V_HEVC";
#endif
#ifdef HAVE_DREAMNEXTGEN
    /* AVS video from libav is AV_CODEC_ID_CAVS, AV_CODEC_ID_AVS is an
     * unrelated game codec */
    case AV_CODEC_ID_CAVS:
#else
    case AV_CODEC_ID_AVS:
#endif
        return "V_AVS";
    }
    return NULL;
}

static char* Codec2Encoding(int32_t codec_id, int32_t media_type, uint8_t *extradata, int extradata_size, int profile, int32_t *version)
{
    char *video = VideoCodec2Encoding(codec_id, version);
    ffmpeg_printf(10, "Codec ID: %d (%.8x)\n", codec_id, codec_id);
    if (video)
        return video;
    switch (codec_id)
    {
    case AV_CODEC_ID_MP2:
        return "A_MPEG/L3";
    case AV_CODEC_ID_MP3:
        return (mp3_software_decode) ? "A_IPCM" : "A_MP3";
    case AV_CODEC_ID_AAC:
        if (extradata_size >= 2) {
            MPEG4AudioConfig m4ac;
            int off = avpriv_mpeg4audio_get_config(&m4ac, extradata, extradata_size * 8, 1);
            ffmpeg_printf(1,"aac [%d] off[%d]\n", m4ac.object_type, off);
            if (off < 0) {
                return "A_IPCM";
            }
            else if (AOT_USAC_NOSBR == m4ac.object_type) {
                // xHE-AAC (USAC) is object type 42: the bundled mpeg4audio.h still names that AOT_USAC_NOSBR
                // ADTS has no profile for it, the AAC writer would send it as AAC-LC
                return "A_IPCM";
            }
            else if (m4ac.chan_config > 7) {
                // 11-13 (6.1, 7.1): the ADTS header of the AAC writer has 3 bits
                // for the channel configuration and would make 3-5 of them
                return "A_IPCM";
            }
            else if (0 == m4ac.chan_config && STB_HISILICON != GetSTBType()) {
                // according to https://wiki.multimedia.cx/index.php/ADTS
                // "MPEG-4 Channel Configuration  - in the case of 0, the channel configuration is sent via an inband PCE"	
                // we already have AAC_LATM formatter which will include PCE
                return (aac_latm_software_decode) ? "A_IPCM" : "A_AAC_LATM";
            }
        }
        return (aac_software_decode) ? "A_IPCM" : "A_AAC";
    case AV_CODEC_ID_AAC_LATM:
        return (aac_latm_software_decode) ? "A_IPCM" : "A_AAC_LATM";
    case AV_CODEC_ID_AC3:
        return  (ac3_software_decode) ? "A_IPCM" : "A_AC3";
    case AV_CODEC_ID_EAC3:
        return  (eac3_software_decode) ? "A_IPCM" : "A_EAC3";
    case AV_CODEC_ID_DTS:
        return (dts_software_decode) ? "A_IPCM" : "A_DTS";
    case AV_CODEC_ID_WMAV1:
        return "A_IPCM";
    /* The Vu+ audio driver plays the WMA bypass values of the writers
     * (0x20, 0x21) as MPEG without an error, and its own WMA modes do not
     * take this data either; the Vu+ images build their GStreamer sink
     * without WMA. Vorbis and Opus have no decoder there at all. */
    case AV_CODEC_ID_WMAV2:
        return (wma_software_decode || STB_VUPLUS == GetSTBType()) ? "A_IPCM" : "A_WMA";
    case AV_CODEC_ID_WMAPRO:
        return (wma_software_decode || STB_VUPLUS == GetSTBType()) ? "A_IPCM" : "A_WMA/PRO";
    case AV_CODEC_ID_WMALOSSLESS:
        return "A_IPCM";
    case AV_CODEC_ID_MLP:
        return "A_IPCM";
    case AV_CODEC_ID_RA_144:
        return "A_IPCM";
    case AV_CODEC_ID_RA_288:
        return "A_IPCM";
    case AV_CODEC_ID_FLAC:
        return "A_IPCM";
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_U24LE:
    case AV_CODEC_ID_PCM_U24BE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_U32LE:
    case AV_CODEC_ID_PCM_U32BE:
        return pcm_resampling ? "A_IPCM" : "A_PCM";
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:
        return amr_software_decode ? "A_IPCM" : "A_AMR";
    case AV_CODEC_ID_VORBIS:
        return (vorbis_software_decode || STB_VUPLUS == GetSTBType()) ? "A_IPCM" : "A_VORBIS";
    case AV_CODEC_ID_OPUS :
        return (opus_software_decode || STB_VUPLUS == GetSTBType()) ? "A_IPCM" : "A_OPUS";

/* In exteplayer3 embedded text subtitle simple printed
 * to output like other data.  Maybe worth to consider is to use 
 * linux socket or pipe to put
 */

/* subtitle */
    case AV_CODEC_ID_SSA:
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 3, 100)
    case AV_CODEC_ID_ASS:
#endif
        return "S_TEXT/ASS"; /* Hellmaster1024: seems to be ASS instead of SSA */
    case AV_CODEC_ID_DVD_SUBTITLE:
    case AV_CODEC_ID_DVB_TELETEXT:
//    case CODEC_ID_DVB_TELETEXT:
//        return "S_TEXT/SRT"; /* fixme */
    case AV_CODEC_ID_MOV_TEXT:
        return "S_TEXT/MOV";
    case AV_CODEC_ID_TEXT: ///< raw UTF-8 text
        return "S_TEXT/UTF-8";
    case AV_CODEC_ID_SRT:
        return "S_TEXT/SRT";
    case AV_CODEC_ID_SUBRIP:
        return "S_TEXT/SUBRIP"; 
    case AV_CODEC_ID_WEBVTT:
        return "S_TEXT/WEBVTT";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        return "S_GRAPHIC/PGS";
    case AV_CODEC_ID_DVB_SUBTITLE:
        return "S_GRAPHIC/DVB";
    case AV_CODEC_ID_XSUB:
        return "S_GRAPHIC/XSUB";
    case AV_CODEC_ID_EIA_608:
        return "S_TEXT/EIA608";
    default:
        ffmpeg_err("Codec ID %d (%.8x) not found\n", codec_id, codec_id);
        // Default to injected-pcm for unhandled audio types.
        if (media_type == AVMEDIA_TYPE_AUDIO)
        {
            return "A_IPCM";
        }
        ffmpeg_err("Codec ID %d (%.8x) not found\n", codec_id, codec_id);
    }
    return NULL;
}

/* FFmpeg before 6.1 has the FF_PROFILE_ names only, FFmpeg 8 the AV_PROFILE_
 * names only. The profiles that came with 6.1 or later are tested with
 * #ifdef where they are used. */
#ifndef AV_PROFILE_AAC_LOW
#define AV_PROFILE_AAC_LOW       FF_PROFILE_AAC_LOW
#define AV_PROFILE_AAC_HE        FF_PROFILE_AAC_HE
#define AV_PROFILE_AAC_HE_V2     FF_PROFILE_AAC_HE_V2
#define AV_PROFILE_AAC_LD        FF_PROFILE_AAC_LD
#define AV_PROFILE_AAC_ELD       FF_PROFILE_AAC_ELD
#define AV_PROFILE_MPEG2_AAC_LOW FF_PROFILE_MPEG2_AAC_LOW
#define AV_PROFILE_MPEG2_AAC_HE  FF_PROFILE_MPEG2_AAC_HE
#define AV_PROFILE_DTS_ES        FF_PROFILE_DTS_ES
#define AV_PROFILE_DTS_96_24     FF_PROFILE_DTS_96_24
#define AV_PROFILE_DTS_HD_HRA    FF_PROFILE_DTS_HD_HRA
#define AV_PROFILE_DTS_HD_MA     FF_PROFILE_DTS_HD_MA
#define AV_PROFILE_DTS_EXPRESS   FF_PROFILE_DTS_EXPRESS
#endif

#define AUDIO_DESC_EAC3       "Dolby Digital +"
#define AUDIO_DESC_EAC3_ATMOS "Dolby Atmos"

/* Name of an audio codec for display, as precise as codec id and profile
 * allow. The result is a constant string, NULL if FFmpeg has no name. */
static const char *Codec2AudioDescription(int32_t codec_id, int profile)
{
    const char *name;

    switch (codec_id)
    {
    case AV_CODEC_ID_DTS:
#ifdef AV_PROFILE_DTS_HD_MA_X_IMAX
        if (profile == AV_PROFILE_DTS_HD_MA_X_IMAX)
            return "DTS:X IMAX";
#endif
#ifdef AV_PROFILE_DTS_HD_MA_X
        if (profile == AV_PROFILE_DTS_HD_MA_X)
            return "DTS:X";
#endif
        if (profile == AV_PROFILE_DTS_HD_MA)
            return "DTS-HD MA";
        if (profile == AV_PROFILE_DTS_HD_HRA)
            return "DTS-HD HRA";
        if (profile == AV_PROFILE_DTS_EXPRESS)
            return "DTS Express";
        if (profile == AV_PROFILE_DTS_ES)
            return "DTS-ES";
        if (profile == AV_PROFILE_DTS_96_24)
            return "DTS 96/24";
        return "DTS";
    case AV_CODEC_ID_AC3:
        return "Dolby Digital";
    case AV_CODEC_ID_EAC3:
#ifdef AV_PROFILE_EAC3_DDP_ATMOS
        if (profile == AV_PROFILE_EAC3_DDP_ATMOS)
            return AUDIO_DESC_EAC3_ATMOS;
#endif
        return AUDIO_DESC_EAC3;
    case AV_CODEC_ID_TRUEHD:
        return "Dolby TrueHD";
    case AV_CODEC_ID_MLP:
        return "MLP";
    case AV_CODEC_ID_MP2:
        return "MP2";
    case AV_CODEC_ID_MP3:
        return "MP3";
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM:
        if (profile == AV_PROFILE_AAC_LOW || profile == AV_PROFILE_MPEG2_AAC_LOW)
            return "AAC-LC";
        if (profile == AV_PROFILE_AAC_HE || profile == AV_PROFILE_MPEG2_AAC_HE)
            return "HE-AAC";
        if (profile == AV_PROFILE_AAC_HE_V2)
            return "HE-AAC v2";
        if (profile == AV_PROFILE_AAC_LD)
            return "AAC-LD";
        if (profile == AV_PROFILE_AAC_ELD)
            return "AAC-ELD";
#ifdef AV_PROFILE_AAC_USAC
        if (profile == AV_PROFILE_AAC_USAC)
            return "xHE-AAC";
#endif
        return "AAC";
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
        return "WMA";
    case AV_CODEC_ID_WMAPRO:
        return "WMA Pro";
    case AV_CODEC_ID_WMALOSSLESS:
        return "WMA Lossless";
    case AV_CODEC_ID_RA_144:
        return "RealAudio 14.4";
    case AV_CODEC_ID_RA_288:
        return "RealAudio 28.8";
    case AV_CODEC_ID_FLAC:
        return "FLAC";
    case AV_CODEC_ID_AMR_NB:
        return "AMR";
    case AV_CODEC_ID_AMR_WB:
        return "AMR-WB";
    case AV_CODEC_ID_VORBIS:
        return "Vorbis";
    case AV_CODEC_ID_OPUS:
        return "Opus";
    case AV_CODEC_ID_MP1:
        return "MPEG Layer I";
    case AV_CODEC_ID_ALAC:
        return "ALAC";
    case AV_CODEC_ID_WAVPACK:
        return "WavPack";
    case AV_CODEC_ID_APE:
        return "APE";
    case AV_CODEC_ID_TTA:
        return "TTA";
    case AV_CODEC_ID_SPEEX:
        return "Speex";
    case AV_CODEC_ID_PCM_ALAW:
        return "A-law";
    case AV_CODEC_ID_PCM_MULAW:
        return "mu-law";
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_BLURAY:
        return "LPCM";
    default:
        break;
    }

    /* the name FFmpeg has for the codec, "unknown_codec" if it has none */
    name = avcodec_get_name((enum AVCodecID)codec_id);
    if (!name)
        return NULL;
    if (!strncmp(name, "pcm_", 4))
        return "PCM";
    if (!strncmp(name, "dsd_", 4))
        return "DSD";
    return name;
}

/* ------------------------------------------------------------------ */
/* Dolby Atmos in E-AC-3                                              */
/*                                                                    */
/* The profile FFmpeg reports is a snapshot of the frames it probed   */
/* when the stream was opened. In a broadcast Atmos can start later   */
/* and end again. Atmos in E-AC-3 is Joint Object Coding, signalled   */
/* in the additional bitstream information of the syncframe header    */
/* (ETSI TS 103 420: flag_ec3_extension_type_a and a complexity index */
/* other than 0). The headers of the packets that are played anyway   */
/* are parsed; nothing is read or decoded for it.                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    const uint8_t *data;
    size_t         size;
    size_t         bitpos;
    int            ok;
} eac3_bits_t;

static uint32_t eac3_bits_read(eac3_bits_t *br, unsigned int n)
{
    uint32_t value = 0;

    if (!br->ok || n > br->size * 8 - br->bitpos)
    {
        br->ok = 0;
        return 0;
    }
    while (n--)
    {
        value = (value << 1) | ((br->data[br->bitpos >> 3] >> (7 - (br->bitpos & 7))) & 1);
        ++br->bitpos;
    }
    return value;
}

static void eac3_bits_skip(eac3_bits_t *br, unsigned int n)
{
    if (!br->ok || n > br->size * 8 - br->bitpos)
        br->ok = 0;
    else
        br->bitpos += n;
}

/* Parses the header (bsi) of one E-AC-3 syncframe, ETSI TS 102 366 E.1.2.
 * Returns 1 for a valid frame of substream 0 with its size in *frame_size;
 * *atmos is set if the frame signals JOC. */
static int eac3_parse_frame(const uint8_t *data, size_t size, unsigned int *frame_size, int *atmos)
{
    eac3_bits_t br = { data, size, 0, 1 };
    unsigned int i, frame_type, substreamid, sr_code, num_blocks, channel_mode, lfe_on, bitstream_id;

    *frame_size = 0;
    *atmos = 0;

    if (!data || size < 7)
        return 0;
    if (eac3_bits_read(&br, 16) != 0x0b77)
        return 0;

    frame_type  = eac3_bits_read(&br, 2);
    substreamid = eac3_bits_read(&br, 3);
    *frame_size = (eac3_bits_read(&br, 11) + 1) << 1;
    sr_code     = eac3_bits_read(&br, 2);

    num_blocks = 6;
    if (sr_code == 3)
    {
        if (eac3_bits_read(&br, 2) == 3)
            return 0;
    }
    else
    {
        static const unsigned int blocks[4] = { 1, 2, 3, 6 };
        num_blocks = blocks[eac3_bits_read(&br, 2)];
    }

    channel_mode = eac3_bits_read(&br, 3);
    lfe_on       = eac3_bits_read(&br, 1);

    if (!br.ok || frame_type == 3 || substreamid != 0 || *frame_size < 7 || *frame_size > size)
        return 0;

    /* the header is inside the frame, nothing behind it is looked at */
    br.size = *frame_size;

    bitstream_id = eac3_bits_read(&br, 5);
    if (!br.ok || bitstream_id <= 10 || bitstream_id > 16)
        return 0;

    /* volume control parameters */
    for (i = 0; i < (channel_mode ? 1U : 2U); ++i)
    {
        eac3_bits_skip(&br, 5);                 /* dialnorm */
        if (eac3_bits_read(&br, 1))
            eac3_bits_skip(&br, 8);             /* compression */
    }

    /* dependent stream channel map */
    if (frame_type == 1)
    {
        if (eac3_bits_read(&br, 1))
            eac3_bits_skip(&br, 16);
    }

    /* mixing metadata */
    if (eac3_bits_read(&br, 1))
    {
        if (channel_mode > 2)
        {
            eac3_bits_skip(&br, 2);             /* preferred downmix */
            if (channel_mode & 1)
                eac3_bits_skip(&br, 6);         /* center mix levels */
            if (channel_mode & 4)
                eac3_bits_skip(&br, 6);         /* surround mix levels */
        }

        if (lfe_on && eac3_bits_read(&br, 1))
            eac3_bits_skip(&br, 5);

        if (frame_type == 0)
        {
            for (i = 0; i < (channel_mode ? 1U : 2U); ++i)
            {
                if (eac3_bits_read(&br, 1))
                    eac3_bits_skip(&br, 6);     /* program scale factor */
            }

            if (eac3_bits_read(&br, 1))
                eac3_bits_skip(&br, 6);         /* external program scale factor */

            switch (eac3_bits_read(&br, 2))     /* mix control type */
            {
                case 1:
                    eac3_bits_skip(&br, 5);
                    break;
                case 2:
                    eac3_bits_skip(&br, 12);
                    break;
                case 3:
                {
                    unsigned int mix_data_size = (eac3_bits_read(&br, 5) + 2) << 3;
                    eac3_bits_skip(&br, mix_data_size);
                    break;
                }
                default:
                    break;
            }

            if (channel_mode < 2)
            {
                for (i = 0; i < (channel_mode ? 1U : 2U); ++i)
                {
                    if (eac3_bits_read(&br, 1))
                        eac3_bits_skip(&br, 14); /* pan information */
                }
            }

            if (eac3_bits_read(&br, 1))         /* frame mixing configuration */
            {
                for (i = 0; i < num_blocks; ++i)
                {
                    if (num_blocks == 1 || eac3_bits_read(&br, 1))
                        eac3_bits_skip(&br, 5);
                }
            }
        }
    }

    /* informational metadata */
    if (eac3_bits_read(&br, 1))
    {
        eac3_bits_skip(&br, 3);                 /* bsmod */
        eac3_bits_skip(&br, 2);                 /* copyright + original */

        if (channel_mode == 2)
            eac3_bits_skip(&br, 4);             /* surround + headphone mode */
        if (channel_mode >= 6)
            eac3_bits_skip(&br, 2);             /* surround EX mode */

        for (i = 0; i < (channel_mode ? 1U : 2U); ++i)
        {
            if (eac3_bits_read(&br, 1))
                eac3_bits_skip(&br, 8);         /* audio production information */
        }

        if (sr_code != 3)
            eac3_bits_skip(&br, 1);             /* source sample rate code */
    }

    if (frame_type == 0 && num_blocks != 6)
        eac3_bits_skip(&br, 1);                 /* converter sync */

    if (frame_type == 2)
    {
        int have_original_size = (num_blocks == 6);
        if (!have_original_size)
            have_original_size = eac3_bits_read(&br, 1) != 0;
        if (have_original_size)
            eac3_bits_skip(&br, 6);
    }

    if (!br.ok)
        return 0;

    /* additional bitstream information */
    if (eac3_bits_read(&br, 1))
    {
        unsigned int addbsil = eac3_bits_read(&br, 6);
        unsigned int flag, complexity = 0;

        if (!br.ok)
            return 0;

        eac3_bits_skip(&br, 7);
        flag = eac3_bits_read(&br, 1);
        if (br.ok && flag && addbsil >= 1)
            complexity = eac3_bits_read(&br, 8);
        *atmos = br.ok && flag && addbsil >= 1 && complexity >= 1;
    }

    return br.ok;
}

/* Size of an AC-3 syncframe (bsid <= 10), 0 if it is not valid. */
static unsigned int ac3_core_frame_size(const uint8_t *data, size_t size)
{
    static const unsigned int bitrates[19] = {
        32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
        192, 224, 256, 320, 384, 448, 512, 576, 640
    };
    unsigned int fscod, frame_size_code, bitrate;

    if (size < 7 || data[0] != 0x0b || data[1] != 0x77)
        return 0;

    fscod           = data[4] >> 6;
    frame_size_code = data[4] & 0x3f;
    if ((data[5] >> 3) > 10 || fscod == 3 || frame_size_code > 37)
        return 0;

    bitrate = bitrates[frame_size_code >> 1];
    switch (fscod)
    {
        case 0:  return bitrate * 4;
        case 1:  return (((bitrate * 320) / 147) + (frame_size_code & 1)) * 2;
        case 2:  return bitrate * 6;
        default: return 0;
    }
}

/* Walks the syncframes of one packet, 8 at most; 1 if one signals JOC. */
static int eac3_packet_has_atmos(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    unsigned int frame;

    if (!data || size < 7 || data[0] != 0x0b || data[1] != 0x77)
        return 0;

    for (frame = 0; frame < 8 && offset + 7 <= size; ++frame)
    {
        unsigned int frame_size = 0;
        int atmos = 0;

        if ((data[offset + 5] >> 3) <= 10)
        {
            /* AC-3 frame in front of the E-AC-3 frames */
            frame_size = ac3_core_frame_size(data + offset, size - offset);
            if (!frame_size || frame_size > size - offset)
                break;
            offset += frame_size;
            continue;
        }

        if (!eac3_parse_frame(data + offset, size - offset, &frame_size, &atmos))
            break;
        if (atmos)
            return 1;
        offset += frame_size;
    }
    return 0;
}

/* Packets with JOC that make it Atmos, with no more than STALE packets
 * without JOC in between; packets without JOC that end it again. Frames
 * without JOC are normal inside an Atmos stream (dependent substreams). */
#define EAC3_ATMOS_CONFIRM_PACKETS 3
#define EAC3_ATMOS_STALE_PACKETS   16
#define EAC3_ATMOS_LOST_PACKETS    48

/* What the demuxer thread knows about the audio track it plays. Written by
 * the demuxer thread, read by the command thread through the audio manager:
 * under audio_live_mutex. No other lock is taken while it is held. */
static pthread_mutex_t audio_live_mutex    = PTHREAD_MUTEX_INITIALIZER;
static int32_t         audio_live_track_id = -1;
static int             audio_live_channels = 0;   /* of the decoded frames, 0 = not decoded here */
static int             audio_live_atmos    = -1;  /* E-AC-3: -1 unknown, 0 no, 1 yes */
static int             audio_live_gen      = 0;   /* counts the changes of the values above */
/* demuxer thread only */
static unsigned int    audio_live_atmos_pos = 0;
static unsigned int    audio_live_atmos_neg = 0;

/* Changes when a value of container_ffmpeg_audio_live_info() has changed. */
int container_ffmpeg_audio_info_gen(void)
{
    int gen;

    pthread_mutex_lock(&audio_live_mutex);
    gen = audio_live_gen;
    pthread_mutex_unlock(&audio_live_mutex);
    return gen;
}

/* For the audio manager. Returns 0 if track_id is not the track that plays.
 * *channels: channel count of the decoded frames, 0 if the track is not
 * decoded here. *description: a constant string if the packets say more than
 * the codec parameters did, else NULL. */
int container_ffmpeg_audio_live_info(int32_t track_id, int *channels, const char **description)
{
    int ret = 0;

    pthread_mutex_lock(&audio_live_mutex);
    if (track_id >= 0 && track_id == audio_live_track_id)
    {
        *channels    = audio_live_channels;
        *description = audio_live_atmos == 1 ? AUDIO_DESC_EAC3_ATMOS :
                       (audio_live_atmos == 0 ? AUDIO_DESC_EAC3 : NULL);
        ret = 1;
    }
    pthread_mutex_unlock(&audio_live_mutex);
    return ret;
}

/* Called for every packet of the audio track that plays. */
static void audio_live_packet(int32_t track_id, int codec_id, const uint8_t *data, size_t size)
{
    int is_eac3 = codec_id == AV_CODEC_ID_EAC3;
    int detected = is_eac3 ? eac3_packet_has_atmos(data, size) : 0;
    int changed = 0;

    pthread_mutex_lock(&audio_live_mutex);
    if (track_id != audio_live_track_id)
    {
        audio_live_track_id  = track_id;
        audio_live_channels  = 0;
        audio_live_atmos     = -1;
        audio_live_atmos_pos = 0;
        audio_live_atmos_neg = 0;
        ++audio_live_gen;
    }

    if (is_eac3)
    {
        if (detected)
        {
            ++audio_live_atmos_pos;
            audio_live_atmos_neg = 0;
        }
        else
        {
            ++audio_live_atmos_neg;
            if (audio_live_atmos != 1 && audio_live_atmos_neg > EAC3_ATMOS_STALE_PACKETS)
                audio_live_atmos_pos = 0;
        }

        if (audio_live_atmos != 1 && audio_live_atmos_pos >= EAC3_ATMOS_CONFIRM_PACKETS)
        {
            audio_live_atmos = 1;
            changed = 1;
        }
        else if (audio_live_atmos == 1 && audio_live_atmos_neg >= EAC3_ATMOS_LOST_PACKETS)
        {
            audio_live_atmos     = 0;
            audio_live_atmos_pos = 0;
            changed = 1;
        }
        if (changed)
            ++audio_live_gen;
    }
    pthread_mutex_unlock(&audio_live_mutex);

    if (changed)
        ffmpeg_printf(1, "E-AC-3 Atmos (JOC) %s on audio track %d\n", detected ? "detected" : "no longer present", (int)track_id);
}

/* Called for every decoded frame of the audio track that plays. The codec
 * parameters do not follow a stream that changes its channel layout, as a
 * broadcast does between programme and commercials. */
static void audio_live_frame(int32_t track_id, int channels)
{
    int old = 0;

    pthread_mutex_lock(&audio_live_mutex);
    if (track_id == audio_live_track_id && channels > 0 && channels != audio_live_channels)
    {
        old = audio_live_channels;
        audio_live_channels = channels;
        ++audio_live_gen;
    }
    else
    {
        channels = 0;
    }
    pthread_mutex_unlock(&audio_live_mutex);

    if (channels)
        ffmpeg_printf(10, "audio track %d: %d channels decoded (before: %d)\n", (int)track_id, channels, old);
}

static int64_t doCalcPts(int64_t start_time, const AVRational time_base, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE)
        return INVALID_PTS_VALUE;
    if (time_base.den > 0)
    {
        pts = av_rescale(pts, (int64_t)time_base.num * 90000, time_base.den);
        /* av_rescale() gives INT64_MIN on an overflow: the cue FFmpeg puts
         * at the end of a WebVTT stream has the PTS INT64_MAX - 2 */
        if (pts == INT64_MIN)
            return INVALID_PTS_VALUE;
    }
    
    if (start_time != AV_NOPTS_VALUE)
    {
        pts = av_sat_sub64(pts, 90000 * start_time / AV_TIME_BASE);
    }

    if (pts & 0x8000000000000000ull)
    {
        pts = INVALID_PTS_VALUE;
    }
    else
    {
        pts &= 0x01FFFFFFFFull; // PES header can handle only 33 bit PTS
    }

    return pts;
}

static int64_t calcPts(uint32_t avContextIdx, AVStream *stream, int64_t pts)
{
    (void)avContextIdx;
    if (!stream)
        return INVALID_PTS_VALUE;
    return calcConvertedPts(stream, stream->time_base, pts, 0);
}

static int64_t calcDts(uint32_t avContextIdx, AVStream *stream, int64_t pts)
{
    (void)avContextIdx;
    if (!stream)
        return INVALID_PTS_VALUE;
    return calcConvertedPts(stream, stream->time_base, pts, 1);
}

static int64_t calcConvertedPts(AVStream *stream, AVRational time_base, int64_t pts, int dts)
{
    int kind = get_codecpar(stream)->codec_type == AVMEDIA_TYPE_AUDIO;
    int64_t raw, corrected;
    if (pts == AV_NOPTS_VALUE || time_base.den <= 0 || time_base.num <= 0)
        return INVALID_PTS_VALUE;
    raw = av_rescale_q(pts, time_base, (AVRational){1, 90000});
    if (dts || get_codecpar(stream)->codec_type == AVMEDIA_TYPE_SUBTITLE)
        corrected = pts_clock_map(&ptsClock, kind, raw);
    else
        corrected = pts_clock_update(&ptsClock, kind, stream, raw, av_gettime_relative());
    if (corrected < 0 || corrected == PTS_CLOCK_INVALID)
        return INVALID_PTS_VALUE;
    return corrected & (PTS_CLOCK_WRAP - 1);
}

/* Some muxers stamp AAC (and DTS) frames from a coarse clock, so PES PTS jitter
 * around the real sample position (e.g. +-15 ms sawtooth). HW decoders
 * (Broadcom) do TSM per PES and react to such jumps with gaps/drops,
 * which is audible as periodic stuttering. To avoid it, PTS of AAC
 * frames are generated from the number of injected samples and the
 * stream PTS is used only to (re)synchronize.
 */
typedef struct
{
    int64_t  anchorPts;  // 90 kHz, INVALID_PTS_VALUE - not synchronized
    int64_t  samples;    // samples injected since anchorPts
    int32_t  sampleRate;
    int32_t  trackId;
    uint32_t avIdx;
} AacPtsDejitter_t;

static void aac_pts_dejitter_reset(AacPtsDejitter_t *s)
{
    s->anchorPts  = INVALID_PTS_VALUE;
    s->samples    = 0;
    s->sampleRate = 0;
    s->trackId    = -1;
    s->avIdx      = 0;
}

static int32_t aac_get_frame_samples(const AVPacket *packet, AVStream *stream, int32_t *sampleRate)
{
    static const int32_t adtsSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    const uint8_t *d = packet->data;

    if (d && packet->size >= 7 && 0xFF == d[0] && 0xF0 == (d[1] & 0xF6))
    {
        uint32_t sfi = (d[2] >> 2) & 0x0F;
        if (sfi < sizeof(adtsSampleRates) / sizeof(adtsSampleRates[0]))
        {
            *sampleRate = adtsSampleRates[sfi];
            return ((d[6] & 0x03) + 1) * 1024;
        }
        return 0;
    }

    if (get_codecpar(stream)->codec_id == AV_CODEC_ID_DTS)
    {
        /* Core frame: NBLKS (7 bits) after the sync word, 32 samples per
         * block, at the core rate SFREQ (4 bits, 66 bits in). The rate of the
         * stream is the one of the decoder output: 96 kHz for DTS 96/24 and
         * DTS-HD over a 48 kHz core, whose frames still have the 512 samples
         * of the core. */
        static const int32_t dtsCoreSampleRates[16] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050,
                                                       44100, 0, 0, 12000, 24000, 48000, 96000, 192000};
        int32_t blocks = -1;
        uint32_t sfreq = 0;

        if (d && packet->size >= 10 && d[0] == 0x7F && d[1] == 0xFE && d[2] == 0x80 && d[3] == 0x01)
        {
            blocks = ((d[4] & 1) << 6) | (d[5] >> 2);
            sfreq = (d[8] >> 2) & 0x0F;
        }
        else if (d && packet->size >= 10 && d[0] == 0xFE && d[1] == 0x7F && d[2] == 0x01 && d[3] == 0x80)
        {
            blocks = ((d[5] & 1) << 6) | (d[4] >> 2);
            sfreq = (d[9] >> 2) & 0x0F;
        }

        if (blocks >= 0)
        {
            *sampleRate = dtsCoreSampleRates[sfreq];
            return *sampleRate > 0 ? (blocks + 1) * 32 : 0;
        }

        *sampleRate = get_codecpar(stream)->sample_rate;
        return *sampleRate > 0 && get_codecpar(stream)->frame_size > 0 ? get_codecpar(stream)->frame_size : 0;
    }

    *sampleRate = get_codecpar(stream)->sample_rate;
    if (*sampleRate <= 0)
    {
        return 0;
    }

    if (get_codecpar(stream)->frame_size > 0)
    {
        return get_codecpar(stream)->frame_size;
    }

    if (packet->duration > 0)
    {
        int32_t samples = (int32_t)av_rescale_q(packet->duration, stream->time_base, (AVRational){1, *sampleRate});
        int32_t profile = get_codecpar(stream)->profile;

        /* A coarse container clock (e.g. 1 ms in mkv) gives 2016 instead of 2048 samples,
         * so the generated PTS would drift. AAC frames are 1024 samples, 2048 with SBR. */
        if (profile != AV_PROFILE_AAC_LD && profile != AV_PROFILE_AAC_ELD && samples >= 512)
        {
            int32_t snapped = ((samples + 512) / 1024) * 1024;
            if (FFABS(snapped - samples) * 20 <= snapped)
            {
                return snapped;
            }
        }
        return samples;
    }

    return 0;
}

static int64_t aac_pts_dejitter(AacPtsDejitter_t *s, const AVPacket *packet, AVStream *stream, int32_t trackId, uint32_t avIdx, int64_t pts)
{
    int32_t sampleRate = 0;
    int32_t frameSamples = 0;

    if (pts == INVALID_PTS_VALUE || 0 == aac_pts_dejitter_ms ||
        (frameSamples = aac_get_frame_samples(packet, stream, &sampleRate)) <= 0)
    {
        aac_pts_dejitter_reset(s);
        return pts;
    }

    if (s->anchorPts != INVALID_PTS_VALUE && s->sampleRate == sampleRate &&
        s->trackId == trackId && s->avIdx == avIdx)
    {
        int64_t expected = (s->anchorPts + av_rescale(s->samples, 90000, sampleRate)) & 0x1FFFFFFFFLL;
        /* difference with 33-bit PTS wrap-around handling */
        int64_t diff = ((pts - expected + 0x100000000LL) & 0x1FFFFFFFFLL) - 0x100000000LL;

        if (FFABS(diff) <= (int64_t)aac_pts_dejitter_ms * 90)
        {
            ffmpeg_printf(200, "AAC PTS %"PRId64" -> %"PRId64" (diff %"PRId64")\n", pts, expected, diff);
            s->samples += frameSamples;
            return expected;
        }

        ffmpeg_printf(10, "AAC PTS resync, diff %"PRId64" ms\n", diff / 90);
    }

    s->anchorPts  = pts;
    s->samples    = frameSamples;
    s->sampleRate = sampleRate;
    s->trackId    = trackId;
    s->avIdx      = avIdx;
    return pts;
}

/* search for metatdata in context and stream
 * and map it to our metadata.
 */

static char* searchMeta(void * metadata, char* ourTag)
{
#if LIBAVCODEC_VERSION_MAJOR < 54
   AVMetadataTag *tag = NULL;
#else
   AVDictionaryEntry *tag = NULL;
#endif
   int i = 0;

   while (metadata_map[i] != NULL)
   {
      if (strcmp(ourTag, metadata_map[i]) == 0)
      {
#if LIBAVCODEC_VERSION_MAJOR < 54
          while ((tag = av_metadata_get(metadata, "", tag, AV_METADATA_IGNORE_SUFFIX)))
#else
          while ((tag = av_dict_get(metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
#endif
          {
              if (strcmp(tag->key, metadata_map[ i + 1 ]) == 0)
              {
                  return tag->value;
              }
          }
      }
      i++;
   }

   return NULL;
}

/* **************************** */
/* Worker Thread                */
/* **************************** */

/* Stall watchdog of http(s) contexts: one av_read_frame() that runs longer
 * than the limit of its context is ended through the interrupt callback.
 * A limit of 0 means not armed. 32 bit values only: the callback can run in
 * another thread, which must not see half of a 64 bit value. */
static uint32_t readStallLimitMs[IPTV_AV_CONTEXT_MAX_NUM] = {0, 0};
static volatile uint32_t readStallStartMs = 0;
static volatile uint32_t readStallActiveMs = 0; /* limit of the running read */
static volatile uint32_t readStallFired = 0;    /* the callback ended a call */

static uint32_t read_stall_now_ms(void)
{
    return (uint32_t)(av_gettime_relative() / 1000);
}

/* The same bound for the calls that open a context, avformat_open_input()
 * and avformat_find_stream_info(): each gets the limit for itself. With pb
 * the clock restarts whenever that context has read from its connection, so
 * the limit is on the time without a byte. pb must stay valid until
 * read_stall_disarm(); both are used by the opening thread only. */
static AVIOContext *readStallPb = NULL;
static uint32_t readStallBytes = 0;

static void read_stall_arm(uint32_t limit, AVIOContext *pb)
{
    readStallFired = 0;
    readStallPb = pb;
    readStallBytes = pb ? (uint32_t)pb->bytes_read : 0;
    readStallStartMs = read_stall_now_ms();
    readStallActiveMs = limit;
}

static void read_stall_disarm(void)
{
    readStallActiveMs = 0;
    readStallPb = NULL;
}

static int network_read_retry(int64_t start, int64_t now)
{
    return now >= start && now - start < 10 * AV_TIME_BASE;
}

static int64_t context_duration_ms(AVFormatContext *av)
{
    return av->duration > 0 && av->duration != AV_NOPTS_VALUE ? av->duration / 1000 : 0;
}

static int64_t ts_seek_position(int64_t time_us, int64_t duration_us,
                                int64_t bytes, int packet_size)
{
    int64_t target;
    if (time_us < 0 || duration_us <= 0 || bytes <= 0 ||
        (packet_size != 188 && packet_size != 192 && packet_size != 204))
        return -1;
    target = av_rescale(time_us, bytes, duration_us);
    if (target < 0)
        return -1;
    if (target >= bytes)
        target = bytes > packet_size ? bytes - packet_size : 0;
    return target / packet_size * packet_size;
}

static int fast_http_ts_seek(AVFormatContext *av, int64_t relative_us)
{
    int64_t packet_size, target;
    if (!av->pb || !(av->pb->seekable & AVIO_SEEKABLE_NORMAL) ||
        !av->iformat || strcmp(av->iformat->name, "mpegts") ||
        !av->url || (strncmp(av->url, "http://", 7) && strncmp(av->url, "https://", 8)) ||
        av_opt_get_int(av->priv_data, "ts_packetsize", 0, &packet_size) < 0)
        return -1;
    target = ts_seek_position(relative_us, av->duration, avio_size(av->pb), packet_size);
    if (target < 0)
        return -1;
    return avformat_seek_file(av, -1, INT64_MIN, target, INT64_MAX, AVSEEK_FLAG_BYTE);
}

static char *json_escape_metadata(const char *text)
{
    const uint8_t *p = (const uint8_t *)text, *end = p + strlen(text);
    char *result, *out;
    if ((size_t)(end - p) > (SIZE_MAX - 1) / 6 ||
        !(result = av_malloc((end - p) * 6 + 1)))
    {
        ffmpeg_err("cannot allocate escaped radio metadata\n");
        return NULL;
    }
    out = result;
    while (p < end)
    {
        if (*p == '"' || *p == '\\')
        {
            *out++ = '\\';
            *out++ = *p++;
        }
        else if (*p < 32)
        {
            out += sprintf(out, "\\u%04x", *p++);
        }
        else if (*p >= 128)
        {
            const uint8_t *next = p;
            int32_t code;
            if (av_utf8_decode(&code, &next, end, 0) >= 0)
            {
                memcpy(out, p, next - p);
                out += next - p;
                p = next;
            }
            else
                out += sprintf(out, "\\u%04x", *p++);
        }
        else
            *out++ = *p++;
    }
    *out = 0;
    return result;
}

static char *radio_message(const char *rt, const char *title, const char *artist)
{
    char *r = json_escape_metadata(rt), *t = json_escape_metadata(title);
    char *a = json_escape_metadata(artist), *message = NULL;
    if (r && t && a)
    {
        message = av_asprintf("{\"m_t\":{\"rt\":\"%s\",\"t\":\"%s\",\"a\":\"%s\"}}\n", r, t, a);
        if (!message)
            ffmpeg_err("cannot allocate radio metadata notification\n");
    }
    av_free(r);
    av_free(t);
    av_free(a);
    return message;
}

static char *radio_icy_message(const char *packet)
{
    const char *start = strstr(packet, "StreamTitle='"), *end;
    char *full, *artist = NULL, *message, *sep;
    if (!start || !(end = strstr(start += 13, "';")))
        return NULL;
    full = av_strndup(start, end - start);
    if (!full)
    {
        ffmpeg_err("cannot allocate ICY title\n");
        return NULL;
    }
    sep = strstr(full, " - ");
    if (sep)
        artist = av_strndup(full, sep - full);
    if (sep && !artist)
    {
        ffmpeg_err("cannot allocate ICY artist\n");
        av_free(full);
        return NULL;
    }
    message = radio_message(full, sep ? sep + 3 : full, artist ? artist : "");
    av_free(artist);
    av_free(full);
    return message;
}

static void radio_metadata_update(AVFormatContext *av, AVStream *stream,
                                  char **last, int has_video)
{
    uint8_t *icy = NULL;
    char *message = NULL, *rt;
    AVDictionaryEntry *title, *artist;
    AVDictionary *metadata = av->metadata;
    int updated = av->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED;
    if (has_video)
        return;
    if (stream && (stream->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED))
    {
        updated = 1;
        metadata = stream->metadata;
        stream->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
    if (av->pb && av_opt_get(av->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &icy) >= 0 && icy)
        message = radio_icy_message((char *)icy);
    av_free(icy);
    if (!message && (updated || !*last))
    {
        title = av_dict_get(metadata, "title", NULL, 0);
        if (!title)
            title = av_dict_get(metadata, "TIT2", NULL, 0);
        artist = av_dict_get(metadata, "artist", NULL, 0);
        if (!artist)
            artist = av_dict_get(metadata, "TPE1", NULL, 0);
        if (updated || title || artist)
        {
            rt = av_asprintf("%s%s%s", artist ? artist->value : "",
                             artist && title ? " - " : "", title ? title->value : "");
            if (rt)
            {
                message = radio_message(rt, title ? title->value : "", artist ? artist->value : "");
                av_free(rt);
            }
            else
                ffmpeg_err("cannot allocate radio text\n");
        }
    }
    av->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
    if (message && (!*last || strcmp(message, *last)))
    {
        E2iSendMsg("%s", message);
        av_freep(last);
        *last = message;
    }
    else
        av_free(message);
}

/* DreamOS live A/V start sync: in live TS audio can trail video by >1 s and
 * Dreambox drops it as late. Hold the output at start until enough audio is
 * buffered (PTS gap + 800 ms). On by default on Dreambox only;
 * EP3_LIVE_SYNC=1 enables it on other boxes, EP3_LIVE_SYNC=0 disables it. */
#include <time.h>
static volatile int     ls_state  = 0;   /* 0 not started, 1 holding, 2 done/off */
static int              ls_default = 0;  /* default of the box, set by FFMPEGThread() */
/* written by the demuxer thread, read by ls_thread: under ls_pts_mutex,
 * a 64-bit load or store is not atomic on 32-bit ARM and MIPS */
static pthread_mutex_t  ls_pts_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t          ls_last_v = INVALID_PTS_VALUE;
static int64_t          ls_last_a = INVALID_PTS_VALUE;

static void ls_set_pts(int64_t *dst, int64_t pts)
{
    pthread_mutex_lock(&ls_pts_mutex);
    *dst = pts;
    pthread_mutex_unlock(&ls_pts_mutex);
}

static int64_t ls_get_pts(const int64_t *src)
{
    int64_t pts;
    pthread_mutex_lock(&ls_pts_mutex);
    pts = *src;
    pthread_mutex_unlock(&ls_pts_mutex);
    return pts;
}

static int64_t ls_now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *ls_thread(void *arg)
{
    Context_t *context = (Context_t *)arg;
    int64_t t0 = ls_now_ms(), maxgap = 0, need = -1;
    /* safety margin on top of the measured gap; the watch below resyncs later
     * if it was not enough, so keep zapping fast. EP3_LIVE_SYNC_MARGIN=ms */
    const char *menv = getenv("EP3_LIVE_SYNC_MARGIN");
    int64_t margin = menv ? atoi(menv) : 400;
    while (ls_state == 1 && context->playback && context->playback->isPlaying)
    {
        int64_t el = ls_now_ms() - t0;
        int64_t last_v = ls_get_pts(&ls_last_v);
        int64_t last_a = ls_get_pts(&ls_last_a);
        need = -1;
        if (last_v != INVALID_PTS_VALUE && last_a != INVALID_PTS_VALUE)
        {
            int64_t gap = (last_v - last_a) / 90;
            if (gap > maxgap) maxgap = gap;
            need = maxgap > 200 ? maxgap + margin : 0;
            if (need > 3000) need = 3000;
        }
        else if (el >= 2000)
            need = 0;                     /* no audio seen: do not hold video */
        if ((need >= 0 && el >= need) || el >= 3500)
            break;
        usleep(50000);
    }
    if (ls_state == 1)
    {
        ls_state = 2;
        if (context->playback && context->playback->isPlaying && !context->playback->isPaused)
            context->output->Command(context, OUTPUT_CONTINUE, NULL);
        printf("{\"log\":\"live sync: audio gap %lld ms, held %lld ms\"}\n", (long long)maxgap, (long long)(ls_now_ms() - t0));
        fflush(stdout);
    }
    else
        return NULL;

    /* Keep watching while the live stream plays: when the audio buffered in the
     * decoder runs out (audio would come after its presentation time and be
     * dropped for the rest of the channel), hold the picture briefly until
     * there is enough audio again. EP3_LIVE_SYNC_WATCH=0 disables it. */
    const char *env = getenv("EP3_LIVE_SYNC_WATCH");
    if (env && env[0] == '0')
        return NULL;
    int low = 0;
    int64_t last_resync = 0;
    while (context->playback && context->playback->isPlaying)
    {
        usleep(250000);
        int64_t last_a = ls_get_pts(&ls_last_a);
        if (context->playback->isPaused || last_a == INVALID_PTS_VALUE)
        {
            low = 0;
            continue;
        }
        unsigned long long int cur = 0;
        if (context->output->Command(context, OUTPUT_PTS, &cur) != 0 || cur == 0)
            continue;
        int64_t d = (last_a - (int64_t)cur) & 0x1FFFFFFFFLL;
        if (d >= 0x100000000LL)
            d -= 0x200000000LL;
        int64_t slack = d / 90;
        if (slack < -10000 || slack > 30000)    /* discontinuity, not a buffer state */
        {
            low = 0;
            continue;
        }
        low = slack < 150 ? low + 1 : 0;
        if (low < 2 || ls_now_ms() - last_resync < 5000)
            continue;
        /* hold until about 1 s of audio is buffered ahead of the picture */
        int64_t t1 = ls_now_ms();
        context->output->Command(context, OUTPUT_PAUSE, NULL);
        while (context->playback && context->playback->isPlaying && !context->playback->isPaused &&
               ls_now_ms() - t1 < 3000)
        {
            int64_t dd = (ls_get_pts(&ls_last_a) - (int64_t)cur) & 0x1FFFFFFFFLL;
            if (dd >= 0x100000000LL)
                dd -= 0x200000000LL;
            if (dd / 90 >= 1000)
                break;
            usleep(50000);
        }
        if (context->playback && context->playback->isPlaying && !context->playback->isPaused)
            context->output->Command(context, OUTPUT_CONTINUE, NULL);
        last_resync = ls_now_ms();
        low = 0;
        printf("{\"log\":\"live sync: audio ran out (%lld ms), held %lld ms\"}\n", (long long)slack, (long long)(last_resync - t1));
        fflush(stdout);
    }
    return NULL;
}

static void ls_on_video(Context_t *context, int64_t pts)
{
    if (pts != INVALID_PTS_VALUE) ls_set_pts(&ls_last_v, pts);
    if (ls_state != 0) return;
    const char *env = getenv("EP3_LIVE_SYNC");
    int enabled = env ? env[0] != '0' : ls_default;
    if (!enabled || !avContextTab[0] ||
        (avContextTab[0]->duration != AV_NOPTS_VALUE && avContextTab[0]->duration > 0))
    {
        ffmpeg_printf(10, "live sync bypass: enabled=%d duration_us=%"PRId64"\n",
                      enabled,
                      avContextTab[0] ? avContextTab[0]->duration : AV_NOPTS_VALUE);
        ls_state = 2;
        return;
    }
    ls_state = 1;
    context->output->Command(context, OUTPUT_PAUSE, NULL);
    pthread_t th;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, ls_thread, context) != 0)
    {
        ls_state = 2;
        context->output->Command(context, OUTPUT_CONTINUE, NULL);
    }
    pthread_attr_destroy(&at);
}

/* ************************************************************************ */
/* Optional audio transcoding for outputs that cannot take the codec of the */
/* stream. The track is decoded as for the PCM injection, but the samples   */
/* are encoded again and written as if the stream carried the new codec.    */
/* ************************************************************************ */
#include <libavutil/audio_fifo.h>

/* Track_t::transcode, and the bits of audio_transcode_caps() */
#define AUDIO_XCODE_AC3 1
#define AUDIO_XCODE_AAC 2
#define AUDIO_XCODE_EAC3 4
/* E-AC-3 5.1: more than AC3 can have (640 kbit/s), and inside what the
 * encoder takes with six blocks in a frame (1024 kbit/s at 48 kHz) */
#define AUDIO_XCODE_EAC3_BITRATE 768000
/* avcodec_send_frame() and encoders that are found without registration */
#define HAVE_AUDIO_XCODE (LIBAVCODEC_VERSION_MAJOR >= 58)
/* the timestamps of the stream may differ this much (90 kHz) from the
 * count of the samples before the count starts again from the stream */
#define AUDIO_XCODE_MAX_DRIFT (200 * 90)

/* per codec of the stream: 0 off, 1 to AC3, 2 to E-AC-3 */
static int32_t dts_ac3_transcode = 0;
static int32_t truehd_ac3_transcode = 0;
static int32_t aac_he_transcode = 0;

void aac_he_transcoder_set(const int32_t val)
{
    aac_he_transcode = val;
}

void dts_ac3_transcoder_set(const int32_t val)
{
    dts_ac3_transcode = val;
}

void truehd_ac3_transcoder_set(const int32_t val)
{
    truehd_ac3_transcode = val;
}

/* the encoders of the linked FFmpeg: AUDIO_XCODE_AC3 | AUDIO_XCODE_AAC | AUDIO_XCODE_EAC3 */
int32_t audio_transcode_caps(void)
{
#if HAVE_AUDIO_XCODE
    return (avcodec_find_encoder(AV_CODEC_ID_AC3) ? AUDIO_XCODE_AC3 : 0) |
           (avcodec_find_encoder(AV_CODEC_ID_AAC) ? AUDIO_XCODE_AAC : 0) |
           (avcodec_find_encoder(AV_CODEC_ID_EAC3) ? AUDIO_XCODE_EAC3 : 0);
#else
    return 0;
#endif
}

typedef struct
{
    AVCodecContext *encoder;
    SwrContext *swr;
    AVAudioFifo *fifo;
    AVPacket *packet;
    /* what the set-up is for: track, target and format of the decoded frames */
    const void *stream;
    int target;
    int in_channels;
    int in_rate;
    int in_format;
    int out_channels;
    /* the set-up failed for this input, said once */
    int failed;
    /* 90 kHz time of the first sample sent to the encoder, and their count */
    int64_t base_pts;
    int64_t sent;
    /* AAC: the packet with its ADTS header */
    uint8_t *adts;
    unsigned int adts_size;
} AudioTranscoder_t;

/* HE-AAC that says so: by the profile the demuxer or the parser found, or
 * by SBR/PS in the AudioSpecificConfig. A stream that brings SBR without
 * any signalling looks like AAC-LC here and is not caught. */
static int AudioTranscoderIsHEAAC(AVStream *stream)
{
    const int profile = get_codecpar(stream)->profile;

#ifdef AV_PROFILE_AAC_HE
    if (profile == AV_PROFILE_AAC_HE || profile == AV_PROFILE_AAC_HE_V2 || profile == AV_PROFILE_MPEG2_AAC_HE)
#else
    if (profile == FF_PROFILE_AAC_HE || profile == FF_PROFILE_AAC_HE_V2 || profile == FF_PROFILE_MPEG2_AAC_HE)
#endif
        return 1;

    if (get_codecpar(stream)->codec_id == AV_CODEC_ID_AAC && get_codecpar(stream)->extradata_size >= 2)
    {
        MPEG4AudioConfig m4ac;
        if (avpriv_mpeg4audio_get_config(&m4ac, get_codecpar(stream)->extradata,
                                         get_codecpar(stream)->extradata_size * 8, 1) >= 0)
            return m4ac.sbr == 1 || m4ac.ps == 1;
    }
    return 0;
}

/* 0, or the codec the audio stream is to be encoded to */
static int AudioTranscoderTarget(AVStream *stream, const char *encoding)
{
    enum AVCodecID id = get_codecpar(stream)->codec_id;
    int target = 0;
    int32_t dolby = 0;

    /* the PCM export needs the decoded samples of every track */
    if (pcm_audio_export_enabled)
        return 0;

    if (id == AV_CODEC_ID_DTS)
        dolby = dts_ac3_transcode;
    else if (id == AV_CODEC_ID_TRUEHD || id == AV_CODEC_ID_MLP)
        dolby = truehd_ac3_transcode;

    if (dolby)
        /* AC3 also when E-AC-3 is asked for and its encoder is missing */
        target = (dolby == 2 && (audio_transcode_caps() & AUDIO_XCODE_EAC3)) ? AUDIO_XCODE_EAC3 : AUDIO_XCODE_AC3;
    /* only AAC that would go to the decoder of the box: not what is decoded
     * in software by an option or because no ADTS header can describe it */
    else if (aac_he_transcode && (!strcmp(encoding, "A_AAC") || !strcmp(encoding, "A_AAC_LATM")) &&
             AudioTranscoderIsHEAAC(stream))
        target = AUDIO_XCODE_AAC;

    return target ? (target & audio_transcode_caps()) : 0;
}

/* Marks the track for transcoding and opens its decoder.
 * 0: the track is played as without the option. */
static int AudioTranscoderSetupTrack(Track_t *track, uint32_t avIdx, AVStream *stream, const char *encoding)
{
    int target = AudioTranscoderTarget(stream, encoding);
    const AVCodec *codec;
    AVCodecContext *c;

    if (!target)
        return 0;

    codec = avcodec_find_decoder(get_codecpar(stream)->codec_id);
    c = codec ? wrapped_avcodec_get_context(avIdx, stream) : NULL;
    if (!c || (!avcodec_is_open(c) && avcodec_open2(c, codec, NULL) < 0))
    {
        ffmpeg_err("no decoder to transcode audio stream %d, it is played as it is\n", track->Id);
        return 0;
    }

    track->avCodecCtx = c;
    track->transcode = target;
    return 1;
}

#if HAVE_AUDIO_XCODE
/* the sample format for an encoder, planar float if it takes that */
static enum AVSampleFormat AudioTranscoderSampleFmt(const AVCodec *codec)
{
    const enum AVSampleFormat *fmt = NULL;
    const enum AVSampleFormat *it;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    int count = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmt, &count) < 0)
        fmt = NULL;
#else
    fmt = codec->sample_fmts;
#endif
    if (!fmt || *fmt == AV_SAMPLE_FMT_NONE)
        return AV_SAMPLE_FMT_FLTP;
    for (it = fmt; *it != AV_SAMPLE_FMT_NONE; ++it)
    {
        if (*it == AV_SAMPLE_FMT_FLTP)
            return AV_SAMPLE_FMT_FLTP;
    }
    return *fmt;
}

static int AudioTranscoderFrameChannels(const AVFrame *frame)
{
#if HAVE_CH_LAYOUT
    return frame->ch_layout.nb_channels;
#else
    return frame->channels;
#endif
}

/* the channel layout the encoder is opened with, 0: none for this input */
static uint64_t AudioTranscoderLayout(int target, int channels)
{
    /* AAC: the layouts of the channel configurations 1 to 6, which the ADTS
     * header can name; more channels are mixed down to 5.1 */
    static const uint64_t aac[] = { 0, AV_CH_LAYOUT_MONO, AV_CH_LAYOUT_STEREO, AV_CH_LAYOUT_SURROUND,
                                    AV_CH_LAYOUT_4POINT0, AV_CH_LAYOUT_5POINT0_BACK, AV_CH_LAYOUT_5POINT1_BACK };

    if (target == AUDIO_XCODE_AC3 || target == AUDIO_XCODE_EAC3)
        return AV_CH_LAYOUT_5POINT1;
    if (channels < 1)
        return 0;
    return aac[channels > 6 ? 6 : channels];
}

/* the 7 bytes in front of a raw AAC-LC frame; len counts them */
static void AudioTranscoderAdtsHeader(uint8_t *adts, int rate_index, int chan_config, int len)
{
    adts[0] = 0xFF;
    adts[1] = 0xF1;
    adts[2] = (1 << 6) | (rate_index << 2) | ((chan_config >> 2) & 1);
    adts[3] = ((chan_config & 3) << 6) | ((len >> 11) & 3);
    adts[4] = (len >> 3) & 0xFF;
    adts[5] = ((len & 7) << 5) | 0x1F;
    adts[6] = 0xFC;
}

/* 90 kHz time of a sample counted from base, as it goes into a PES header */
static int64_t AudioTranscoderPts(int64_t base, int64_t samples, int rate)
{
    int64_t pts;

    if (base == (int64_t)INVALID_PTS_VALUE)
        return INVALID_PTS_VALUE;
    pts = base + av_rescale(samples, 90000, rate);
    return pts < 0 ? (int64_t)INVALID_PTS_VALUE : (pts & 0x01FFFFFFFFLL);
}

/* The time of the next decoded frame against the count of the samples that
 * are ahead of it (sent and buffered): the new base when the count has to
 * start again from the stream, else the old one. */
static int64_t AudioTranscoderBase(int64_t base, int64_t samples, int rate, int64_t frame_pts)
{
    int64_t ahead = av_rescale(samples, 90000, rate);

    if (frame_pts == (int64_t)INVALID_PTS_VALUE)
        return base;
    if (base != (int64_t)INVALID_PTS_VALUE &&
        pts_clock_abs(pts_clock_difference(frame_pts, base + ahead)) <= AUDIO_XCODE_MAX_DRIFT)
        return base;
    return frame_pts - ahead;
}

static void AudioTranscoderFree(AudioTranscoder_t *state)
{
    if (state->fifo)
        av_audio_fifo_free(state->fifo);
    state->fifo = NULL;
    swr_free(&state->swr);
    avcodec_free_context(&state->encoder);
    av_packet_free(&state->packet);
    av_freep(&state->adts);
    state->adts_size = 0;
}

/* drops the encoder and what it has buffered: after a seek, for another
 * track and at the end */
static void AudioTranscoderClose(AudioTranscoder_t *state)
{
    AudioTranscoderFree(state);
    state->stream = NULL;
    state->target = 0;
    state->failed = 0;
}

static int AudioTranscoderInit(AudioTranscoder_t *state, const AVFrame *in)
{
    const int target = state->target;
    const AVCodec *codec = avcodec_find_encoder(target == AUDIO_XCODE_AC3 ? AV_CODEC_ID_AC3 :
                                                target == AUDIO_XCODE_EAC3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AAC);
    const uint64_t layout = AudioTranscoderLayout(target, state->in_channels);
    AVCodecContext *enc;
    int channels;
    int ret;

    /* AAC keeps the rate of the decoder: one an ADTS header can name */
    if (!codec || !layout || in->sample_rate <= 0 ||
        (target == AUDIO_XCODE_AAC && avpriv_mpeg4audio_sample_rates[aac_get_sample_rate_index(in->sample_rate)] != in->sample_rate))
    {
        ffmpeg_err("no encoder for %d channels %d Hz\n", state->in_channels, in->sample_rate);
        return -1;
    }

    state->encoder = enc = avcodec_alloc_context3(codec);
    state->swr = swr_alloc();
    state->packet = av_packet_alloc();
    if (!enc || !state->swr || !state->packet)
        return -1;

#if HAVE_CH_LAYOUT
    av_channel_layout_from_mask(&enc->ch_layout, layout);
    channels = enc->ch_layout.nb_channels;
#else
    enc->channel_layout = layout;
    enc->channels = channels = av_get_channel_layout_nb_channels(layout);
#endif
    if (target == AUDIO_XCODE_AC3)
    {
        enc->bit_rate = 640000;
        enc->sample_rate = 48000;
    }
    else if (target == AUDIO_XCODE_EAC3)
    {
        enc->bit_rate = AUDIO_XCODE_EAC3_BITRATE;
        enc->sample_rate = 48000;
    }
    else
    {
        enc->bit_rate = 64000 * channels;
        enc->sample_rate = in->sample_rate;
#ifdef AV_PROFILE_AAC_LOW
        enc->profile = AV_PROFILE_AAC_LOW;
#else
        enc->profile = FF_PROFILE_AAC_LOW;
#endif
        /* raw frames from every encoder, the ADTS header is written here */
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    enc->sample_fmt = AudioTranscoderSampleFmt(codec);
    enc->time_base = (AVRational){1, enc->sample_rate};

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0 || enc->frame_size <= 0)
    {
        ffmpeg_err("encoder init failed: %d\n", ret);
        return -1;
    }

#if HAVE_CH_LAYOUT
    {
        AVChannelLayout in_layout = {0};
        if (av_channel_layout_check(&in->ch_layout))
            av_channel_layout_copy(&in_layout, &in->ch_layout);
        else
            av_channel_layout_default(&in_layout, state->in_channels);
        av_opt_set_chlayout(state->swr, "in_chlayout", &in_layout, 0);
        av_opt_set_chlayout(state->swr, "out_chlayout", &enc->ch_layout, 0);
        av_channel_layout_uninit(&in_layout);
    }
#else
    av_opt_set_int(state->swr, "in_channel_layout",
                   in->channel_layout ? in->channel_layout : (uint64_t)av_get_default_channel_layout(state->in_channels), 0);
    av_opt_set_int(state->swr, "out_channel_layout", layout, 0);
#endif
    av_opt_set_sample_fmt(state->swr, "in_sample_fmt", in->format, 0);
    av_opt_set_sample_fmt(state->swr, "out_sample_fmt", enc->sample_fmt, 0);
    av_opt_set_int(state->swr, "in_sample_rate", in->sample_rate, 0);
    av_opt_set_int(state->swr, "out_sample_rate", enc->sample_rate, 0);
    /* as the PCM path: no clipping when channels are mixed down */
    av_opt_set_double(state->swr, "rematrix_maxval", 1.0, 0);

    ret = swr_init(state->swr);
    if (ret < 0)
    {
        ffmpeg_err("resampler init failed: %d\n", ret);
        return -1;
    }

    state->fifo = av_audio_fifo_alloc(enc->sample_fmt, channels, enc->frame_size * 2);
    if (!state->fifo)
        return -1;

    state->out_channels = channels;
    state->base_pts = INVALID_PTS_VALUE;
    state->sent = 0;
    ffmpeg_printf(1, "audio transcoder ready: %s %d channels %d Hz %d kbit/s\n",
                  codec->name, channels, enc->sample_rate, (int)(enc->bit_rate / 1000));
    return 0;
}

/* writes one packet of the encoder */
static void AudioTranscoderWrite(AudioTranscoder_t *state, Context_t *context)
{
    AVPacket *packet = state->packet;
    AudioVideoOut_t out;
    /* the encoder counts in samples and takes its own delay off */
    int64_t pts = AudioTranscoderPts(state->base_pts,
                                     packet->pts != AV_NOPTS_VALUE ? packet->pts : state->sent - state->encoder->frame_size,
                                     state->encoder->sample_rate);

    memset(&out, 0, sizeof(out));
    out.data = packet->data;
    out.len = packet->size;
    out.pts = pts;
    out.type = "audio";
    if (state->target == AUDIO_XCODE_AAC)
    {
        const int len = packet->size + AAC_HEADER_LENGTH;

        av_fast_malloc(&state->adts, &state->adts_size, len);
        /* 13 bits for the length of the frame */
        if (!state->adts || len > 0x1FFF)
            return;
        AudioTranscoderAdtsHeader(state->adts, aac_get_sample_rate_index(state->encoder->sample_rate),
                                  get_chan_config(state->out_channels), len);
        memcpy(state->adts + AAC_HEADER_LENGTH, packet->data, packet->size);
        out.data = state->adts;
        out.len = len;
    }
    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &out, pts) < 0)
    {
        ffmpeg_err("(transcoded) writing data to audio device failed\n");
    }
}

/* encodes the full frames of the FIFO */
static int AudioTranscoderEncode(AudioTranscoder_t *state, Context_t *context)
{
    AVCodecContext *enc = state->encoder;
    int ret = 0;

    while (ret >= 0 && av_audio_fifo_size(state->fifo) >= enc->frame_size)
    {
        AVFrame *frame = wrapped_frame_alloc();
        if (!frame)
            return -1;

        frame->nb_samples = enc->frame_size;
        frame->format = enc->sample_fmt;
        frame->sample_rate = enc->sample_rate;
        frame->pts = state->sent;
#if HAVE_CH_LAYOUT
        ret = av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
#else
        frame->channel_layout = enc->channel_layout;
        frame->channels = enc->channels;
#endif
        if (ret >= 0)
            ret = av_frame_get_buffer(frame, 0);
        if (ret >= 0 && av_audio_fifo_read(state->fifo, (void **)frame->extended_data, frame->nb_samples) < frame->nb_samples)
            ret = -1;
        if (ret >= 0)
            ret = avcodec_send_frame(enc, frame);
        wrapped_frame_free(&frame);
        if (ret < 0)
            break;

        state->sent += enc->frame_size;
        while ((ret = avcodec_receive_packet(enc, state->packet)) >= 0)
        {
            AudioTranscoderWrite(state, context);
            av_packet_unref(state->packet);
        }
        if (ret == AVERROR(EAGAIN))
            ret = 0;
    }
    return ret < 0 ? -1 : 0;
}

/* Takes one decoded frame of the track. The container mutex is released
 * while the output is written: track and frame are not used after that.
 * -1: nothing is written for this input until it changes or a seek. */
static int AudioTranscoderProcess(AudioTranscoder_t *state, Context_t *context, Track_t *track, AVFrame *decoded, uint32_t avIdx, int64_t packet_pts)
{
    const int in_channels = AudioTranscoderFrameChannels(decoded);
    uint8_t **converted = NULL;
    int64_t frame_pts;
    int out_samples;
    int ret;

    /* the set-up is for the first frame: another track, or a change from
     * 5.1 to stereo or of the rate inside the stream, needs a new one */
    if (state->stream != track->stream || state->target != track->transcode || in_channels != state->in_channels ||
        decoded->sample_rate != state->in_rate || decoded->format != state->in_format)
    {
        AudioTranscoderClose(state);
        state->stream = track->stream;
        state->target = track->transcode;
        state->in_channels = in_channels;
        state->in_rate = decoded->sample_rate;
        state->in_format = decoded->format;
        if (AudioTranscoderInit(state, decoded) < 0)
        {
            ffmpeg_err("audio transcoder set-up failed, no audio for this track\n");
            AudioTranscoderFree(state);
            state->failed = 1;
        }
    }
    if (state->failed)
        return -1;

    /* The packet went through calcPts(), which moves the clock; the frames
     * are only mapped with it. The count of the samples gives the time of
     * the packets, the stream is followed when it jumps or drifts away. */
    frame_pts = calcDts(avIdx, track->stream, wrapped_frame_get_best_effort_timestamp(decoded));
    if (frame_pts == (int64_t)INVALID_PTS_VALUE && state->base_pts == (int64_t)INVALID_PTS_VALUE)
        frame_pts = packet_pts;
    state->base_pts = AudioTranscoderBase(state->base_pts, state->sent + av_audio_fifo_size(state->fifo),
                                          state->encoder->sample_rate, frame_pts);

    out_samples = av_rescale_rnd(swr_get_delay(state->swr, decoded->sample_rate) + decoded->nb_samples,
                                 state->encoder->sample_rate, decoded->sample_rate, AV_ROUND_UP);
    ret = av_samples_alloc_array_and_samples(&converted, NULL, state->out_channels, out_samples,
                                             state->encoder->sample_fmt, 0);
    if (ret >= 0)
    {
        ret = swr_convert(state->swr, converted, out_samples,
                          (const uint8_t **)decoded->extended_data, decoded->nb_samples);
        if (ret > 0 && av_audio_fifo_write(state->fifo, (void **)converted, ret) < ret)
            ret = -1;
        av_freep(&converted[0]);
        av_freep(&converted);
    }
    if (ret >= 0)
        ret = AudioTranscoderEncode(state, context);
    if (ret < 0)
    {
        ffmpeg_err("audio transcoding failed, no audio for this track\n");
        AudioTranscoderFree(state);
        state->failed = 1;
        return -1;
    }
    return 0;
}
#else
/* not reached: audio_transcode_caps() is 0, no track is marked */
static void AudioTranscoderClose(AudioTranscoder_t *state)
{
    (void)state;
}

static int AudioTranscoderProcess(AudioTranscoder_t *state, Context_t *context, Track_t *track, AVFrame *decoded, uint32_t avIdx, int64_t packet_pts)
{
    (void)state; (void)context; (void)track; (void)decoded; (void)avIdx; (void)packet_pts;
    return -1;
}
#endif

static void FFMPEGThread(Context_t *context) 
{
    char *lastRadioMessage[IPTV_AV_CONTEXT_MAX_NUM] = {NULL, NULL};
    int pcm_export_failed = 0;
    dashAudioResetPending = 0;
    ls_default = STB_DREAMBOX == GetSTBType();
    ptsClock.preserve_timeline = context->playback->isDash;
    pts_clock_reset(&ptsClock, 0);
    if (avContextTab[0] && (use_custom_io[0] || avContextTab[0]->duration > 0) &&
        avContextTab[0]->start_time != AV_NOPTS_VALUE)
    {
        ptsClock.base = av_rescale_q(avContextTab[0]->start_time, AV_TIME_BASE_Q, (AVRational){1, 90000});
        ptsClock.base_ready = 1;
    }
    if (avContextTab[0])
        ffmpeg_printf(10, "playback timing: duration_us=%"PRId64" start_us=%"PRId64
                      " format=%s preserve_timeline=%d\n",
                      avContextTab[0]->duration, avContextTab[0]->start_time,
                      avContextTab[0]->iformat->name, ptsClock.preserve_timeline);
    char threadname[17];
    strncpy(threadname, __func__, sizeof(threadname));
    threadname[16] = 0;
    prctl (PR_SET_NAME, (unsigned long)&threadname);
    AVPacket   packet;
    off_t   lastSeek = -1;
    int64_t lastPts = -1;
    int64_t currentVideoPts = -1;
    int64_t currentAudioPts = -1;
    int64_t lastVideoWriteUs = 0;
    const AVStream *lastAudioStream = NULL;
    uint32_t lastAudioContextIdx = 0;
    
    /* lastVideoDts and lastAudioDts 
     * used in isTSLiveMode
     */
    int64_t lastVideoDts = -1;
    int64_t lastAudioDts = -1;
    
    int64_t multiContextLastPts[IPTV_AV_CONTEXT_MAX_NUM] = {INVALID_PTS_VALUE, INVALID_PTS_VALUE};
    /* the second input (-x) has ended: the playback goes on with the first */
    int8_t multiContextEnded[IPTV_AV_CONTEXT_MAX_NUM] = {0, 0};

    AacPtsDejitter_t aacPtsDejitter;
    aac_pts_dejitter_reset(&aacPtsDejitter);
    
    int64_t showtime = 0;
    int64_t bofcount = 0;
    int32_t       err = 0;
    AudioVideoOut_t avOut;
    
    g_context = context;

    SwrContext *swr = NULL;
    /* input format the resampler was set up for */
    int32_t swr_in_channels = 0;
    int32_t swr_in_rate = 0;
    int32_t swr_in_format = -1;
    /* the resampler has got a timestamp to count on from */
    int32_t swr_has_pts = 0;
    /* timestamp of the last packet the audio decoder has taken */
    int64_t decoder_pkt_pts = AV_NOPTS_VALUE;
    /* the PCM writer has to renew its header with the next frame it gets */
    int32_t renew_pcm_header = 0;
    AudioTranscoder_t audioTranscoder;
    memset(&audioTranscoder, 0, sizeof(audioTranscoder));
    AVFrame *decoded_frame = NULL;
    int32_t out_sample_rate = 44100;
#if HAVE_CH_LAYOUT
    AVChannelLayout out_channel_layout;
    av_channel_layout_default(&out_channel_layout, 2);
#else
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    int32_t out_channels = 2;
    uint32_t cAVIdx = 0;
    int64_t readErrorSince[IPTV_AV_CONTEXT_MAX_NUM] = {0, 0};
    /* TS live mode: the read error that has been reported, 0 after a packet */
    int liveReadError = 0;
    /* why reading has stopped, when it is not the end of the input: the
     * stall limit or a network error that outlasted its retries; sent as
     * FF_ERROR when playback ends on it, 0 after a packet */
    int endError = 0;

    // for seek
    int64_t seek_target_seconds = 0;
    bool do_seek_target_seconds = false;
    bool zero_seek = false;
    int64_t seek_from_pts = INVALID_PTS_VALUE;
    int64_t seek_offset_us = 0;
    /* where reading has started: 0 or the last seek target, -1 unknown */
    int64_t read_origin_us = 0;

    int64_t seek_target_bytes = 0;
    bool do_seek_target_bytes = false;
    int64_t prev_seek_time_sec = -1;
    void *stamp = context->playback->stamp;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
#ifdef __sh__
    Mpeg4P2Context *mpeg4p2_context = NULL;
#else
    Mpeg4P2Context *mpeg4p2_context = mpeg4p2_context_open();
#endif
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
    Flv2Mpeg4Context flv2mpeg4_context;
    memset(&flv2mpeg4_context, 0, sizeof(Flv2Mpeg4Context));
#endif
    ffmpeg_printf(10, "\n");
    while ( context->playback->isCreationPhase )
    {
        ffmpeg_printf(10, "Thread waiting for end of init phase...\n");
        usleep(1000);
    }
    ffmpeg_printf(10, "Running!\n");

    uint32_t bufferSize = 0;
    context->output->Command(context, OUTPUT_GET_BUFFER_SIZE, &bufferSize);
    ffmpeg_printf(10, "bufferSize [%u]\n", bufferSize);

    /* The start position goes through the seek of the loop below before the
     * first packet is read, so nothing from 0 is written. Not for live
     * streams (no duration) and not at or behind the end. Once only, a loop
     * restart starts at 0. */
    if (g_start_position_us > 0)
    {
        int64_t duration = avContextTab[0] ? avContextTab[0]->duration : AV_NOPTS_VALUE;
        if (context->playback->isTSLiveMode || duration == AV_NOPTS_VALUE || duration <= 0)
        {
            ffmpeg_err("start position ignored: the stream has no duration\n");
        }
        else if (g_start_position_us >= duration)
        {
            ffmpeg_err("start position ignored: %"PRId64" us is not before the end (%"PRId64" us)\n",
                       g_start_position_us, duration);
        }
        else
        {
            ffmpeg_printf(10, "start at %"PRId64" us\n", g_start_position_us);
            do_seek_target_seconds = true;
            seek_target_seconds = g_start_position_us;
            seek_offset_us = g_start_position_us;
        }
        g_start_position_us = 0;
    }

    int8_t isWaitingForFinish = 0;
    while ( context && context->playback && context->playback->isPlaying ) 
    {
        /* When user press PAUSE we call pause on AUDIO and VIDEO decoders, 
         * we will not wait here because we can still fill 
         * DVB drivers buffers at PAUSE time
         * 
         */
#ifdef __sh__
        /* ST DVB drivers skip data if they are written during pause 
         * so, we must wait here if there is not buffering queue
         */
        if (0 == bufferSize && context->playback->isPaused) 
        {
            ffmpeg_printf(20, "paused\n");
            reset_finish_timeout();
            usleep(10000);
            continue;
        }
#endif

        if (context->playback->isSeeking || context->playback->isAudioSwitching)
        {
            ffmpeg_printf(10, "seeking\n");
            reset_finish_timeout();
            usleep(10000);
            continue;
        }

        getMutex(__FILE__, __FUNCTION__,__LINE__);

        if (!context->playback || !context->playback->isPlaying ||
            context->playback->isSeeking || context->playback->isAudioSwitching)
        {
            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
            if(!isWaitingForFinish)
            {
                reset_finish_timeout();
            }
            continue;
        }

        getSeekMutex();
        if (g_do_seek_target_seconds) 
        {
            do_seek_target_seconds = g_do_seek_target_seconds;
            seek_target_seconds = g_seek_target_seconds;
            zero_seek = g_zero_seek;
            seek_from_pts = g_seek_from_pts;
            seek_offset_us = g_seek_offset_us;
            stamp = g_stamp;
            g_do_seek_target_seconds = false;
        }
        releaseSeekMutex();

        if (dashAudioResetPending)
        {
            dashAudioResetPending = 0;
            restart_audio_resampling = 1;
            currentVideoPts = currentAudioPts = -1;
            aac_pts_dejitter_reset(&aacPtsDejitter);
            wrapped_avcodec_flush_buffers(0);
        }
        if (do_seek_target_seconds || do_seek_target_bytes) 
        {
            /* The read again of a live TS (-v) goes on with its clock: the
             * stream goes on, what is read twice is skipped by its DTS. A
             * reset there put the DTS behind the last one written. */
            if (do_seek_target_seconds ? !zero_seek : !context->playback->isTSLiveMode)
            {
                pts_clock_reset(&ptsClock, 1);
                lastVideoDts = lastAudioDts = -1;
            }
            isWaitingForFinish = 0;
            seek_finish_timeout();
            if (do_seek_target_seconds)
            {
                /* A decoder that has not shown a frame of this playback yet can
                 * report the PTS of the frame it showed before: of the previous
                 * playback or of the position before the last seek. Such a
                 * position lies after all that has been written since: count
                 * from where the reading has started instead. */
                if (seek_from_pts != INVALID_PTS_VALUE && read_origin_us >= 0 &&
                    seek_from_pts > latestPts + 2 * 90000)
                {
                    ffmpeg_printf(10, "decoder position %"PRId64" not written yet (%"PRId64"), seek from %"PRId64" us\n",
                                  seek_from_pts, latestPts, read_origin_us);
                    seek_target_seconds = av_sat_add64(read_origin_us, seek_offset_us);
                    if (seek_target_seconds < 0)
                        seek_target_seconds = 0;
                }
                seek_from_pts = INVALID_PTS_VALUE;
                read_origin_us = seek_target_seconds;
                ffmpeg_printf(10, "seek_target_seconds[%"PRId64"]\n", seek_target_seconds);
                int64_t relative_us = seek_target_seconds;
                uint32_t i = 0;
                for(; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
                {
                    readErrorSince[i] = 0;
                    multiContextLastPts[i] = INVALID_PTS_VALUE;
                    multiContextEnded[i] = 0;
                    if(NULL != avContextTab[i])
                    {
                        int64_t target_us = relative_us;
                        if (i == 1)
                        {
                            prev_seek_time_sec = seek_target_seconds;
                        }
                        if (avContextTab[i]->start_time != AV_NOPTS_VALUE)
                        {
                            target_us = av_sat_add64(target_us, avContextTab[i]->start_time);
                        }
                        if (!(zero_seek && !use_custom_io[i] && avContextTab[i]->duration <= 0) &&
                            (zero_seek || fast_http_ts_seek(avContextTab[i], relative_us) < 0))
                        {
                            if (avformat_seek_file(avContextTab[i], -1, INT64_MIN, target_us, INT64_MAX, 0) < 0)
                                ffmpeg_err("timestamp seek failed for context %u\n", i);
                            else if (zero_seek)
                            {
                                pts_clock_reset(&ptsClock, 1);
                                lastVideoDts = lastAudioDts = -1;
                            }
                        }
                    }
                    else
                    {
                        break;
                    }
                }
                reset_finish_timeout();
                /*
                if (bufferSize > 0)
                {
                    context->output->Command(context, OUTPUT_CLEAR, NULL);
                }
                */
            } 
            else
            {
                container_ffmpeg_seek_bytes(seek_target_bytes);
                read_origin_us = -1;
            }
            do_seek_target_seconds = false;
            do_seek_target_bytes = false;
            if (OutputPacingLeadMs() > 0)
                OutputPacingReset();
            
            restart_audio_resampling = 1;
            currentVideoPts = -1;
            currentAudioPts = -1;
            latestPts = 0;
#ifdef HAVE_DREAMNEXTGEN
            seekWaitVideoKey = true;
            seekVideoKeyPts = INVALID_PTS_VALUE;
            seekVideoSkipped = 0;
#endif
            aac_pts_dejitter_reset(&aacPtsDejitter);

            // flush streams
            uint32_t i = 0;
            for(i=0; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
            {
                if(NULL != avContextTab[i])
                {
                    if (i != 1)
                    {
                        wrapped_avcodec_flush_buffers(i);
                    }
                }
                else
                {
                    break;
                }
            }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
            mpeg4p2_context_reset(mpeg4p2_context);
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
            flv2mpeg4_context_reset(&flv2mpeg4_context);
#endif
#ifdef HAVE_CLOSED_CAPTIONS
            cc_reset();
#endif
        }

#ifdef HAVE_DREAMNEXTGEN
        if (trickSpeed && trickStep && avContextTab[0])
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec < trickDueNs)
            {
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                usleep(10000);
                continue;
            }
            int64_t key = av_rescale(trickKeyPts, AV_TIME_BASE, 90000);
            /* a GOP longer than the step lands on the same keyframe: the step
             * doubles until the next one is in reach */
            int64_t target = key + av_rescale((int64_t)trickSpeed * TRICK_STEP_90K * (1LL << trickMiss), AV_TIME_BASE, 90000);
            if (avContextTab[0]->start_time != AV_NOPTS_VALUE)
            {
                key += avContextTab[0]->start_time;
                target += avContextTab[0]->start_time;
            }
            /* the next keyframe in the direction, not the last one again */
            int ret = trickSpeed > 0 ?
                avformat_seek_file(avContextTab[0], -1, key + 1, target, INT64_MAX, 0) :
                avformat_seek_file(avContextTab[0], -1, INT64_MIN, target, key - 1, 0);
            if (ret < 0)
            {
                /* the end or the start: the last picture stays */
                ffmpeg_printf(10, "trick: no keyframe beyond %"PRId64"\n", trickKeyPts);
                trickDueNs = INT64_MAX;
                E2iSendMsg("{\"PLAYBACK_TRICK_END\":{\"dir\":%d}}\n", trickSpeed > 0 ? 1 : -1);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                continue;
            }
            trickStep = false;
            seekWaitVideoKey = true;
            seekVideoKeyPts = INVALID_PTS_VALUE;
            seekVideoSkipped = 0;
            isWaitingForFinish = 0;
        }
#endif

        int ffmpegStatus = 0;
        if(!isWaitingForFinish)
        {
            if(NULL != avContextTab[1] && !multiContextEnded[1])
            {
                if (prev_seek_time_sec >= 0)
                {
                    if (multiContextLastPts[0] != INVALID_PTS_VALUE) {
                        int64_t target = av_rescale(multiContextLastPts[0], AV_TIME_BASE, 90000);
                        /* multiContextLastPts counts from the start of its context */
                        if (avContextTab[1]->start_time != AV_NOPTS_VALUE)
                        {
                            target = av_sat_add64(target, avContextTab[1]->start_time);
                        }
                        avformat_seek_file(avContextTab[1], -1, INT64_MIN, target, INT64_MAX, 0);
                        prev_seek_time_sec = -1;
                        wrapped_avcodec_flush_buffers(1);
                        cAVIdx = 1;
                    } else {
                        cAVIdx = 0;
                    }
                } else {
                    if (multiContextLastPts[0] != INVALID_PTS_VALUE && multiContextLastPts[1] != INVALID_PTS_VALUE) {
                        cAVIdx = multiContextLastPts[0] < multiContextLastPts[1] ? 0 : 1;
                    } else {
                        cAVIdx = !cAVIdx;
                    }
                }
            }
            else
            {
                cAVIdx = 0;
            }
#ifdef HAVE_DREAMNEXTGEN
            /* the keyframes come from the video context */
            if (trickSpeed)
            {
                cAVIdx = 0;
            }
#endif
        }

        if (bufferSize > 0)
        {
            stamp = context->playback->stamp;
            LinuxDvbBuffSetStamp(stamp);
        }

        if( !isWaitingForFinish )
        {
            readStallStartMs = read_stall_now_ms();
            readStallActiveMs = readStallLimitMs[cAVIdx];
            ffmpegStatus = av_read_frame(avContextTab[cAVIdx], &packet);
            if (ffmpegStatus < 0 && readStallActiveMs &&
                read_stall_now_ms() - readStallStartMs > readStallActiveMs)
            {
                ffmpeg_err("no packet for more than %u s, read ended (%d)\n", readStallActiveMs / 1000, ffmpegStatus);
            }
            readStallActiveMs = 0;
        }

        if (!isWaitingForFinish && (ffmpegStatus == 0) )
        {
            radio_metadata_update(avContextTab[cAVIdx],
                avContextTab[cAVIdx]->streams[packet.stream_index],
                &lastRadioMessage[cAVIdx], context->playback->isVideo);
            readErrorSince[cAVIdx] = 0;
            liveReadError = 0;
            endError = 0;
            int64_t pts = 0;
            int64_t dts = 0;
            Track_t *videoTrack = NULL;
            Track_t *audioTrack = NULL;
            Track_t *subtitleTrack = NULL;

            int32_t pid = stream_track_id(avContextTab[cAVIdx],
                                          avContextTab[cAVIdx]->streams[packet.stream_index]);
            
            multiContextLastPts[cAVIdx] = doCalcPts(avContextTab[cAVIdx]->start_time,
                avContextTab[cAVIdx]->streams[packet.stream_index]->time_base, packet.pts);
            ffmpeg_printf(200, "Ctx %d PTS: %"PRId64" PTS[1] %"PRId64"\n", cAVIdx, multiContextLastPts[cAVIdx], multiContextLastPts[1]);
            
            reset_finish_timeout();
            if(avContextTab[cAVIdx]->streams[packet.stream_index]->discard != AVDISCARD_ALL)
            {
                if (context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack) < 0)
                {
                    ffmpeg_err("error getting video track\n");
                }
                
                if (context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack) < 0)
                {
                    ffmpeg_err("error getting audio track\n");
                }
                
                if (context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &subtitleTrack) < 0)
                {
                    ffmpeg_err("error getting subtitle track\n");
                }
            }
            else
            {
                ffmpeg_printf(1, "SKIP DISCARDED PACKET packed_size[%d] stream_index[%d] pid[%d]\n", packet.size, (int)packet.stream_index, pid);
            }
            
            ffmpeg_printf(200, "packet.size %d - index %d\n", packet.size, pid);

#ifdef HAVE_DREAMNEXTGEN
            if (trickSpeed && !packet_matches_track(videoTrack, cAVIdx, avContextTab[cAVIdx]->streams[packet.stream_index]))
            {
                wrapped_packet_unref(&packet);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                continue;
            }
#endif

            if (packet_matches_track(videoTrack, cAVIdx,
                                     avContextTab[cAVIdx]->streams[packet.stream_index]))
            {
                hevc_hdr_update(context, avContextTab[cAVIdx]->streams[packet.stream_index], &packet);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
                AVCodecContext *codec_context = videoTrack->avCodecCtx;
                if (codec_context && codec_context->codec_id == AV_CODEC_ID_MPEG4 && NULL != mpeg4p2_context)
                {
#ifdef HAVE_DREAMNEXTGEN
                    /* the converter writes whole frames on its own, audio does not wait */
                    seekWaitVideoKey = false;
                    seekVideoKeyPts = INVALID_PTS_VALUE;
#endif
                    mpeg4p2_write_packet(context, mpeg4p2_context, videoTrack, cAVIdx, &currentVideoPts, &latestPts, &packet);
                    update_max_injected_pts(latestPts);
                }
                else
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
                if (get_codecpar(avContextTab[cAVIdx]->streams[packet.stream_index])->codec_id == AV_CODEC_ID_FLV1 &&
                    0 == strcmp(videoTrack->Encoding, "V_MPEG4") )
                {
#ifdef HAVE_DREAMNEXTGEN
                    seekWaitVideoKey = false;
                    seekVideoKeyPts = INVALID_PTS_VALUE;
#endif
                    flv2mpeg4_write_packet(context, &flv2mpeg4_context, videoTrack, cAVIdx, &currentVideoPts, &latestPts, &packet);
                    update_max_injected_pts(latestPts);
                }
                else
#endif
                {
                    AVStream *stream = videoTrack->stream;
                    bool skipPacket = false;
                    int first_packet = currentVideoPts == -1;
                    currentVideoPts = videoTrack->pts = pts = calcPts(cAVIdx, stream, packet.pts);
                    videoTrack->dts = dts = calcDts(cAVIdx, stream, packet.dts);
                    if (first_packet && stream)
                        ffmpeg_printf(10, "first video: raw_pts=%"PRId64" tb=%d/%d pts90=%"PRId64" dts90=%"PRId64"\n",
                                      packet.pts, stream->time_base.num,
                                      stream->time_base.den, pts, dts);
                    if (first_packet)
                        ffmpeg_printf(10, "video clock: raw90=%"PRId64" base90=%"PRId64
                                      " offset90=%"PRId64"\n",
                                      ptsClock.track[0].raw, ptsClock.base,
                                      ptsClock.track[0].offset);
                    if (pts == INVALID_PTS_VALUE)
                    {
                        wrapped_packet_unref(&packet);
                        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                        continue;
                    }
                    {
                        int64_t now = av_gettime_relative();
                        if (pts_clock_video_gap(now, lastVideoWriteUs, PlaybackLastContinue(),
                                                context->playback->isPaused))
                        {
                            ffmpeg_printf(10, "Reinitializing video output after a long gap\n");
                            if (context->output->video->Command(context, OUTPUT_SWITCH, "video") < 0)
                                ffmpeg_err("video output recovery failed\n");
                        }
                        lastVideoWriteUs = now;
                    }

                    if ((currentVideoPts != INVALID_PTS_VALUE) && (currentVideoPts > latestPts))
                    {
                        latestPts = currentVideoPts;
                        update_max_injected_pts(latestPts);
                    }

#ifdef HAVE_DREAMNEXTGEN
                    if (seekWaitVideoKey)
                    {
                        if (!(packet.flags & AV_PKT_FLAG_KEY))
                            seekVideoSkipped++;
                        if ((packet.flags & AV_PKT_FLAG_KEY) || seekVideoSkipped > 250)
                        {
                            seekWaitVideoKey = false;
                            seekVideoKeyPts = pts;
                            ffmpeg_printf(10, "first video %s after the seek, pts %"PRId64"\n", seekVideoSkipped > 250 ? "packet" : "keyframe", pts);
                        }
                        else
                        {
                            skipPacket = true;
                        }
                    }
#endif
                    
                    if (context->playback->isTSLiveMode)
                    {
                        if (dts != INVALID_PTS_VALUE)
                        {   
                            if (pts_clock_after(dts, lastVideoDts))
                            {
                                lastVideoDts = dts;
                            }
                            else
                            {
                                // skip already injected VIDEO packet
                                ffmpeg_printf(200, "skip already injected VIDEO packet\n");
                                skipPacket = true;
                            }
                        }
                        else
                        {
                            // skip VIDEO packet with unknown DTS
                            ffmpeg_printf(200, "skip VIDEO packet with unknown DTS\n");
                            skipPacket = true;
                        }
                    }
                    
                    if (skipPacket)
                    {   
                        wrapped_packet_unref(&packet);
                        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                        continue;
                    }
                    
                    ffmpeg_printf(200, "VideoTrack index = %d %"PRId64"\n",pid, currentVideoPts);

#ifdef HAVE_DREAMNEXTGEN
                    if (trickSpeed && pts != INVALID_PTS_VALUE &&
                        (trickSpeed > 0 ? pts <= trickKeyPts : pts >= trickKeyPts) && trickKeyPts != trickAnchorPts)
                    {
                        /* not beyond the last keyframe: the next seek goes further,
                         * at the start or the end the last picture stays */
                        trickStep = true;
                        if (++trickMiss > 8)
                        {
                            ffmpeg_printf(10, "trick: no keyframe beyond %"PRId64"\n", trickKeyPts);
                            trickDueNs = INT64_MAX;
                            E2iSendMsg("{\"PLAYBACK_TRICK_END\":{\"dir\":%d}}\n", trickSpeed > 0 ? 1 : -1);
                        }
                        wrapped_packet_unref(&packet);
                        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                        continue;
                    }
                    if (trickSpeed)
                    {
                        trickMiss = 0;
                        /* the distance in the stream, scaled by the speed */
                        int64_t dist = (pts == INVALID_PTS_VALUE) ? 0 : (pts - trickAnchorPts) * (trickSpeed > 0 ? 1 : -1);
                        if (dist < 0)
                        {
                            dist = 0;
                        }
                        dist /= abs(trickSpeed);
                        if (pts != INVALID_PTS_VALUE)
                        {
                            trickKeyPts = pts;
                        }
                        trickOutPts = pts = dts = (trickBasePts + dist) & 0x1FFFFFFFFLL;
                        trickDueNs = trickStartNs + av_rescale(dist, 1000000000LL, 90000);
                        trickStep = true;
                        ffmpeg_printf(10, "trick: keyframe %"PRId64" as %"PRId64"\n", trickKeyPts, pts);
                    }
#endif

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.dts        = dts;
                    avOut.extradata  = videoTrack->extraData;
                    avOut.extralen   = videoTrack->extraSize;
                    avOut.frameRate  = videoTrack->frame_rate;
                    avOut.timeScale  = videoTrack->TimeScale;
                    avOut.width      = videoTrack->width;
                    avOut.height     = videoTrack->height;
                    avOut.type       = "video";
                    avOut.infoFlags  = 0;
                    
                    if (avContextTab[cAVIdx]->iformat->flags & AVFMT_TS_DISCONT)
                    {
                        avOut.infoFlags = 1; // TS container
                    }

                    ls_on_video(context, pts);
                    if (Write(context->output->video->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("writing data to video device failed\n");
                    }
#ifdef HAVE_CLOSED_CAPTIONS
                    cc_video_packet(context, cAVIdx, stream, &packet, pts, dts);
#endif
                }
            }
            else if (packet_matches_track(audioTrack, cAVIdx,
                                          avContextTab[cAVIdx]->streams[packet.stream_index]))
            {
                AVStream *stream = audioTrack->stream;
                uint8_t skipPacket = 0;
                int64_t mapped_pts;
                int first_packet = currentAudioPts == -1;
                currentAudioPts = audioTrack->pts = pts = calcPts(cAVIdx, stream, packet.pts);
                mapped_pts = pts;
                dts = calcDts(cAVIdx, stream, packet.dts);
                if (first_packet && stream)
                    ffmpeg_printf(10, "first audio: raw_pts=%"PRId64" tb=%d/%d pts90=%"PRId64" dts90=%"PRId64"\n",
                                  packet.pts, stream->time_base.num,
                                  stream->time_base.den, pts, dts);
                if (currentAudioPts != INVALID_PTS_VALUE) ls_set_pts(&ls_last_a, currentAudioPts);

                if ((currentAudioPts != INVALID_PTS_VALUE) && (currentAudioPts > latestPts) && (!videoTrack))
                {
                    latestPts = currentAudioPts;
                    update_max_injected_pts(latestPts);
                }

#ifdef HAVE_DREAMNEXTGEN
                if (videoTrack && seekWaitVideoKey)
                {
                    skipPacket = 1;
                }
                else if (seekVideoKeyPts != INVALID_PTS_VALUE)
                {
                    if (pts != INVALID_PTS_VALUE && pts < seekVideoKeyPts)
                        skipPacket = 1;
                    else
                        seekVideoKeyPts = INVALID_PTS_VALUE;
                }
#endif
                
                if (context->playback->isTSLiveMode)
                {
                    if (dts != INVALID_PTS_VALUE)
                    {
                        if (pts_clock_after(dts, lastAudioDts))
                        {
                            lastAudioDts = dts;
                        }
                        else
                        {
                            // skip already injected AUDIO packet
                            ffmpeg_printf(200, "skip already injected AUDIO packet\n");
                            skipPacket = 1;
                        }
                    }
                    else
                    {
                        // skip AUDIO packet with unknown PTS
                        ffmpeg_printf(200, "skip AUDIO packet with unknown PTS\n");
                        skipPacket = 1;
                    }
                }
                
                if (skipPacket)
                {
                    wrapped_packet_unref(&packet);
                    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                    continue;
                }

                if (packet.data && packet.size > 0)
                {
                    audio_live_packet(audioTrack->Id, (int)get_codecpar(audioTrack->stream)->codec_id, packet.data, (size_t)packet.size);
                }

                if (audioTrack->inject_raw_pcm != 1 && audioTrack->inject_as_pcm != 1 &&
                    (get_codecpar(audioTrack->stream)->codec_id == AV_CODEC_ID_AAC ||
                     get_codecpar(audioTrack->stream)->codec_id == AV_CODEC_ID_AAC_LATM ||
                     get_codecpar(audioTrack->stream)->codec_id == AV_CODEC_ID_DTS))
                {
                    pts = aac_pts_dejitter(&aacPtsDejitter, &packet, audioTrack->stream, audioTrack->Id, cAVIdx, pts);
                }
                if (lastAudioStream != stream || lastAudioContextIdx != cAVIdx)
                {
                    ffmpeg_printf(10, "audio selection timing: context=%u stream=%d raw_pts=%"PRId64
                                  " tb=%d/%d pts90=%"PRId64" video_pts90=%"PRId64"\n",
                                  cAVIdx, stream->index, packet.pts, stream->time_base.num,
                                  stream->time_base.den, pts, currentVideoPts);
                    ffmpeg_printf(10, "audio selection clock: raw90=%"PRId64
                                  " video_raw90=%"PRId64" base90=%"PRId64
                                  " audio_offset90=%"PRId64" video_offset90=%"PRId64
                                  " before_aac90=%"PRId64" preserve_timeline=%d\n",
                                  ptsClock.track[1].raw, ptsClock.track[0].raw,
                                  ptsClock.base, ptsClock.track[1].offset,
                                  ptsClock.track[0].offset, mapped_pts,
                                  ptsClock.preserve_timeline);
                    lastAudioStream = stream;
                    lastAudioContextIdx = cAVIdx;
                }
                
                pcmPrivateData_t pcmExtradata;
#if HAVE_CH_LAYOUT
                pcmExtradata.channels              = get_codecpar(audioTrack->stream)->ch_layout.nb_channels;
#else
                pcmExtradata.channels              = get_codecpar(audioTrack->stream)->channels;
#endif
                pcmExtradata.bits_per_coded_sample = get_codecpar(audioTrack->stream)->bits_per_coded_sample;
                pcmExtradata.sample_rate           = get_codecpar(audioTrack->stream)->sample_rate;
                pcmExtradata.bit_rate              = get_codecpar(audioTrack->stream)->bit_rate;
                pcmExtradata.block_align           = get_codecpar(audioTrack->stream)->block_align;
                pcmExtradata.frame_size            = get_codecpar(audioTrack->stream)->frame_size;

                pcmExtradata.codec_id              = get_codecpar(audioTrack->stream)->codec_id;
                pcmExtradata.bResampling           = restart_audio_resampling;
                
                uint8_t *pAudioExtradata    = get_codecpar(audioTrack->stream)->extradata;
                uint32_t audioExtradataSize = get_codecpar(audioTrack->stream)->extradata_size;
                    
                ffmpeg_printf(200, "AudioTrack index = %d\n",pid);
                /* nothing of a transcoded track is kept for its next turn */
                if (audioTranscoder.stream && !audioTrack->transcode)
                    AudioTranscoderClose(&audioTranscoder);
                if (audioTrack->inject_raw_pcm == 1)
                {
                    ffmpeg_printf(200,"write audio raw pcm\n");
                    restart_audio_resampling = 0;

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = (uint8_t *) &pcmExtradata;
                    avOut.extralen   = sizeof(pcmExtradata);
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    if (Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("(raw pcm) writing data to audio device failed\n");
                    }
                }
                else if ((audioTrack->inject_as_pcm == 1 || audioTrack->transcode) && audioTrack->avCodecCtx)
                {
                    AVCodecContext *c = audioTrack->avCodecCtx;

                    if (restart_audio_resampling)
                    {
                        restart_audio_resampling = 0;
                        renew_pcm_header = 1;
                        decoder_pkt_pts = AV_NOPTS_VALUE;
                        if (swr)
                        {
                            swr_free(&swr);
                            swr = NULL;
                        }
                        if (decoded_frame)
                        {
                            wrapped_frame_free(&decoded_frame);
                            decoded_frame = NULL;
                        }
                        if (audioTranscoder.stream)
                            AudioTranscoderClose(&audioTranscoder);
                    }
#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                    while (packet.size > 0 || (!packet.size && !packet.data)) 
#else
                    while(packet.size > 0)
#endif
                    {
                        Track_t *selected = NULL;
                        context->manager->audio->Command(context, MANAGER_GET_TRACK, &selected);
                        if (!selected || selected->avCodecCtx != c ||
                            !packet_matches_track(selected, cAVIdx,
                                avContextTab[cAVIdx]->streams[packet.stream_index]))
                            break;
                        audioTrack = selected;
                        if(do_seek_target_seconds || do_seek_target_bytes)
                        {
                            break;
                        }
                        
                        if (!decoded_frame)
                        {
                            decoded_frame = wrapped_frame_alloc();
                            if (!decoded_frame)
                            {
                                ffmpeg_err("out of memory\n");
                                exit(1);
                            }
                        } 
                        else
                        {
                            wrapped_frame_unref(decoded_frame);
                        }
#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                        int ret = avcodec_send_packet(c, &packet);
                        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) 
                        {
                            restart_audio_resampling = 1;
                            break;
                        }
                        
                        if (ret >= 0)
                        {
                            packet.size = 0;
                            decoder_pkt_pts = packet.pts;
                        }
                        
                        ret = avcodec_receive_frame(c, decoded_frame);
                        if (ret < 0)
                        {
                            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) 
                            {
                                /* only this frame is broken: the decoder goes
                                 * on with the next one, and a packet it has
                                 * refused is still to be sent */
                                ffmpeg_printf(10, "avcodec_receive_frame: %d\n", ret);
                                continue;
                            }
                            else
                            {
                                continue;
                            }
                        }
#else
                        int32_t got_frame = 0;
                        int32_t len = avcodec_decode_audio4(c, decoded_frame, &got_frame, &packet);
                        if (len < 0) 
                        {
                            ffmpeg_err("avcodec_decode_audio4: %d\n", len);
                            break;
                        }

                        packet.data += len;
                        packet.size -= len;
                        
                        if (!got_frame)
                        {
                            continue;
                        }
#endif
                        if (audioTrack->transcode)
                        {
                            /* the reset above asked for a new PCM header:
                             * this track has none */
                            renew_pcm_header = 0;
                            if (AudioTranscoderProcess(&audioTranscoder, context, audioTrack, decoded_frame, cAVIdx, pts) < 0)
                                break;
                            continue;
                        }
                        int32_t e = 0;
#if HAVE_CH_LAYOUT
                        const int32_t frame_channels = decoded_frame->ch_layout.nb_channels;
#else
                        const int32_t frame_channels = decoded_frame->channels;
#endif
                        audio_live_frame(audioTrack->Id, frame_channels);
                        /* the resampler is set up for the format of the first
                         * frame: a stream that changes its channels or rate
                         * (5.1 to stereo at a programme change) needs a new one */
                        if (swr && (frame_channels != swr_in_channels || decoded_frame->sample_rate != swr_in_rate || decoded_frame->format != swr_in_format))
                        {
                            ffmpeg_printf(10, "audio format changed, new resampler\n");
                            swr_free(&swr);
                            swr = NULL;
                            renew_pcm_header = 1;
                        }
                        if (!swr) 
                        {
                            swr_in_channels = frame_channels;
                            swr_in_rate = decoded_frame->sample_rate;
                            swr_in_format = decoded_frame->format;
                            swr_has_pts = 0;
                            if(insert_pcm_as_lpcm)
                            {
                                out_sample_rate = 48000;
                            }
                            else
                            {
                                int32_t rates[] = { 48000, 96000, 192000, 44100, 88200, 176400, 0 };
                                int32_t *rate = rates;
                                int32_t in_rate = c->sample_rate;
                                while (*rate && ((*rate / in_rate) * in_rate != *rate) && (in_rate / *rate) * *rate != in_rate)
                                {
                                    rate++;
                                }
                                out_sample_rate = *rate ? *rate : 44100;
                            }

                            swr = swr_alloc();
                            if (!swr)
                            {
                                ffmpeg_err("swr_alloc failed\n");
                                break;
                            }
#if HAVE_CH_LAYOUT
                            out_channels = c->ch_layout.nb_channels;
                            if( !av_channel_layout_check(&c->ch_layout) )
                            {
                                av_channel_layout_default( &c->ch_layout, out_channels );
                            }

                            av_channel_layout_copy(&out_channel_layout, &c->ch_layout);
#else
                            out_channels = c->channels;

                            if (c->channel_layout == 0)
                            {
                                c->channel_layout = av_get_default_channel_layout( c->channels );
                            }
                            out_channel_layout = c->channel_layout;
#endif

                            uint8_t downmix = stereo_software_decoder && out_channels > 2 ? 1 : 0;
#ifdef __sh__
                            // player2 won't play mono
                            if (out_channel_layout == AV_CH_LAYOUT_MONO)
                            {
                                downmix = 1;
                            }
#endif

#if HAVE_CH_LAYOUT
                            if(downmix)
                            {
                                av_channel_layout_default(&out_channel_layout, 2);
                                out_channels = 2;
                            }

                            av_opt_set_chlayout(swr, "in_chlayout", &c->ch_layout,	0);
                            av_opt_set_chlayout(swr, "out_chlayout", &out_channel_layout,	0);
#else
                            if(downmix)
                            {
                                out_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
                                out_channels = 2;
                            }

                            av_opt_set_int(swr, "in_channel_layout",    c->channel_layout,	0);
                            av_opt_set_int(swr, "out_channel_layout",   out_channel_layout,	0);
#endif
                            av_opt_set_int(swr, "in_sample_rate",		c->sample_rate,		0);
                            av_opt_set_int(swr, "out_sample_rate",		out_sample_rate,	0);
                            av_opt_set_int(swr, "in_sample_fmt",		c->sample_fmt,		0);
                            av_opt_set_int(swr, "out_sample_fmt",		AV_SAMPLE_FMT_S16,	0);
        

                            e = swr_init(swr);
                            if (e < 0) 
                            {
#if HAVE_CH_LAYOUT
                                char icl[128];
                                char ocl[128];
                                av_channel_layout_describe(&c->ch_layout, icl, sizeof(icl));
                                av_channel_layout_describe(&out_channel_layout, ocl, sizeof(ocl));
                                ffmpeg_err("swr_init: %d (icl=%s ocl=%s isr=%d osr=%d isf=%d osf=%d\n",
                                    -e, icl, ocl, c->sample_rate, out_sample_rate, c->sample_fmt, AV_SAMPLE_FMT_S16);
#else
                                ffmpeg_err("swr_init: %d (icl=%d ocl=%d isr=%d osr=%d isf=%d osf=%d\n",
                                    -e, (int32_t)c->channel_layout, (int32_t)out_channel_layout, c->sample_rate, out_sample_rate, c->sample_fmt, AV_SAMPLE_FMT_S16);
#endif
                                swr_free(&swr);
                                swr = NULL;
                                continue;
                            }
                        }
                        
                        uint8_t *output[8] = {NULL};
                        int32_t in_samples = decoded_frame->nb_samples;
                        int32_t out_samples = av_rescale_rnd(swr_get_delay(swr, c->sample_rate) + in_samples, out_sample_rate, c->sample_rate, AV_ROUND_UP);
                        e = av_samples_alloc(&output[0], NULL, out_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
                        if (e < 0) 
                        {
                            ffmpeg_err("av_samples_alloc: %d\n", -e);
                            continue;
                        }
                        int64_t best_effort_ts = wrapped_frame_get_best_effort_timestamp(decoded_frame);
                        /* a decoder that makes several frames of one packet
                         * (WMA Pro) can leave all of them without a timestamp:
                         * a new resampler then starts from the packet's one */
                        if (best_effort_ts == AV_NOPTS_VALUE && !swr_has_pts)
                        {
                            best_effort_ts = decoder_pkt_pts;
                        }
                        int64_t next_in_pts = INT64_MIN;
                        int64_t next_out_pts = AV_NOPTS_VALUE;
                        if (best_effort_ts != AV_NOPTS_VALUE)
                        {
                            next_in_pts = av_rescale(best_effort_ts,
                                         ((AVStream*) audioTrack->stream)->time_base.num * (int64_t)out_sample_rate * c->sample_rate,
                                         ((AVStream*) audioTrack->stream)->time_base.den);
                            swr_has_pts = 1;
                        }
                        /* a frame without a timestamp goes on from the samples
                         * converted since the last one that had it */
                        if (swr_has_pts)
                        {
                            next_out_pts = av_rescale(swr_next_pts(swr, next_in_pts),
                                         ((AVStream*) audioTrack->stream)->time_base.den,
                                         ((AVStream*) audioTrack->stream)->time_base.num * (int64_t)out_sample_rate * c->sample_rate);
                        }
                        
                        /* the decoder gives its frames out behind the packets
                         * the clock has seen (WMA Pro: up to three packets of
                         * 1.5 s): map them like a DTS, they must not move the
                         * clock back as a jump */
                        currentAudioPts = audioTrack->pts = pts = calcDts(cAVIdx, audioTrack->stream, next_out_pts);
                        out_samples = swr_convert(swr, &output[0], out_samples, (const uint8_t **) decoded_frame->extended_data, in_samples);
                        if (out_samples < 0)
                        {
                            ffmpeg_err("PCM resampling failed: %d\n", out_samples);
                            av_freep(&output[0]);
                            continue;
                        }
                        /* dual mono: the decoder of the box may not get the
                         * PCM channel selection, so it is made here */
                        if (2 == out_channels)
                        {
                            pcm_select_channel((int16_t *)output[0], out_samples, pcm_audio_channel);
                        }

                        //////////////////////////////////////////////////////////////////////
                        // Update pcmExtradata according to decode parameters
#if HAVE_CH_LAYOUT
                        pcmExtradata.channels              = out_channel_layout.nb_channels;
#else
                        pcmExtradata.channels              = av_get_channel_layout_nb_channels(out_channel_layout);
#endif
                        pcmExtradata.bits_per_coded_sample = 16;
                        pcmExtradata.sample_rate           = out_sample_rate;
                        // The data described by the sample format is always in native-endian order
#ifdef WORDS_BIGENDIAN
                        pcmExtradata.codec_id       = AV_CODEC_ID_PCM_S16BE;
#else
                        pcmExtradata.codec_id       = AV_CODEC_ID_PCM_S16LE;
#endif

                        //////////////////////////////////////////////////////////////////////

                        avOut.data       = output[0];
                        avOut.len        = out_samples * sizeof(int16_t) * out_channels;

                        avOut.pts        = pts;
                        avOut.extradata  = (unsigned char *) &pcmExtradata;
                        avOut.extralen   = sizeof(pcmExtradata);
                        avOut.frameRate  = 0;
                        avOut.timeScale  = 0;
                        avOut.width      = 0;
                        avOut.height     = 0;
                        avOut.type       = "audio";

                        /* the packet that carried the request may have given
                         * no frame: keep it until one goes to the writer */
                        if (renew_pcm_header && !context->playback->BackWard)
                        {
                            renew_pcm_header = 0;
                            pcmExtradata.bResampling = 1;
                        }
                        if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                        {
                            ffmpeg_err("writing data to audio device failed\n");
                        }
                        if (pcm_audio_export_enabled && !pcm_export_failed && !context->playback->BackWard)
                        {
                            int result = pcm_export_write(&pcmExport, output[0], avOut.len,
                                pcmExtradata.sample_rate, pcmExtradata.channels,
                                pcmExtradata.codec_id == AV_CODEC_ID_PCM_S16BE);
                            if (result < 0)
                            {
                                ffmpeg_err("PCM export disabled for this session: %s\n", strerror(errno));
                                pcm_export_failed = 1;
                                pcm_export_close(&pcmExport);
                            }
                            else if (result && pcmExport.dropped_chunks == 1)
                                ffmpeg_printf(0, "PCM export dropping chunks: reader absent, slow or disconnected\n");
                        }
                        av_freep(&output[0]);
                    }
                }
                else if (audioTrack->have_aacheader == 1)
                {
                    ffmpeg_printf(200, "write audio aac\n");
                    ffmpeg_printf(200, "> %hhx %hhx %hhx %hhx %x %hhx %hhx\n", packet.data[0], packet.data[1], packet.data[2], packet.data[3], packet.data[4], packet.data[5], packet.data[6]);

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = audioTrack->aacbuf;
                    avOut.extralen   = audioTrack->aacbuflen;
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("(aac) writing data to audio device failed\n");
                    }
                } 
                else if (pcmExtradata.codec_id == AV_CODEC_ID_VORBIS || pcmExtradata.codec_id == AV_CODEC_ID_OPUS ||
                         pcmExtradata.codec_id == AV_CODEC_ID_WMAV1 || pcmExtradata.codec_id == AV_CODEC_ID_WMAV2 ||
                         pcmExtradata.codec_id == AV_CODEC_ID_WMAPRO || pcmExtradata.codec_id == AV_CODEC_ID_WMALOSSLESS) {
                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = (uint8_t *) &pcmExtradata;
                    avOut.extralen   = sizeof(pcmExtradata);
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    pcmExtradata.private_data = pAudioExtradata;
                    pcmExtradata.private_size = audioExtradataSize;

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0) {
                        ffmpeg_err("writing data to audio device failed\n");
                    }
                }
                else
                {
                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = pAudioExtradata;
                    avOut.extralen   = audioExtradataSize;
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("writing data to audio device failed\n");
                    }
                }
            }
#ifdef HAVE_CLOSED_CAPTIONS
            else if (packet_matches_track(subtitleTrack, cAVIdx,
                                          avContextTab[cAVIdx]->streams[packet.stream_index]) &&
                     get_codecpar(subtitleTrack->stream)->codec_id == AV_CODEC_ID_EIA_608)
            {
                cc_stream_packet(context, pid, &packet,
                                 calcPts(cAVIdx, subtitleTrack->stream, packet.pts));
            }
#endif
            else if (packet_matches_track(subtitleTrack, cAVIdx,
                                          avContextTab[cAVIdx]->streams[packet.stream_index]))
            {
                int64_t duration = -1;
                AVStream *stream = subtitleTrack->stream;
                int64_t subPts = packet.pts;
                /* A subtitle file of its own (-x) counts from the start of the
                 * main input, which need not be 0 (TS, HLS): its cues are put
                 * on the timestamps of the main input. Cues with MPEG-TS
                 * timestamps (WebVTT with an HLS X-TIMESTAMP-MAP) have them. */
                if (cAVIdx != 0 && subPts != AV_NOPTS_VALUE && stream->pts_wrap_bits > 33 &&
                    avContextTab[0]->start_time != AV_NOPTS_VALUE)
                {
                    subPts = av_sat_add64(subPts, av_rescale_q(avContextTab[0]->start_time,
                                                               AV_TIME_BASE_Q, stream->time_base));
                }
                int64_t pts = calcPts(cAVIdx, stream, subPts);

                if (packet.duration != 0)
                {
                    // duration in milliseconds
                    duration = (int64_t)av_rescale(packet.duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den); 
                }
                else if(get_packet_duration(&packet) != 0 && get_packet_duration(&packet) != AV_NOPTS_VALUE )
                {
                    // duration in milliseconds
                    duration = (int64_t)av_rescale(get_packet_duration(&packet), (int64_t)stream->time_base.num * 1000, stream->time_base.den); 
                }

                /* the cue that fake_last_subtitle adds at the end of a WebVTT
                 * stream (pts INT64_MAX - 2) has no time: it is not sent */
                if ((duration > 0 || duration == -1) && pts != INVALID_PTS_VALUE)
                {
                    SubtitleOut_t subOut;
                    memset(&subOut, 0, sizeof(subOut));
                    subOut.trackId = pid;
                    subOut.data = (uint8_t *)packet.data;
                    subOut.len = packet.size;
                    subOut.pts = pts;
                    subOut.durationMS = duration;
                    subOut.extradata = get_codecpar(stream)->extradata;
                    subOut.extralen  = get_codecpar(stream)->extradata_size;
                    subOut.width     = get_codecpar(stream)->width;
                    subOut.height    = get_codecpar(stream)->height;
                    if (Write(context->output->subtitle->Write, context, &subOut, pts) < 0)
                    {
                        ffmpeg_err("writing data to teletext fifo failed\n");
                    }
                }
            }
        }
        else 
        {
            int repeatedReadError = 0;

            if (ffmpegStatus < 0 && ffmpegStatus != AVERROR_EOF &&
                ffmpegStatus != AVERROR_EXIT && !PlaybackDieNow(0) &&
                avContextTab[cAVIdx]->url &&
                (!strncmp(avContextTab[cAVIdx]->url, "http://", 7) ||
                 !strncmp(avContextTab[cAVIdx]->url, "https://", 8)))
            {
                int64_t now = av_gettime_relative();
                if (!readErrorSince[cAVIdx])
                {
                    readErrorSince[cAVIdx] = now;
                    ffmpeg_err("Network read failed (%d), retrying for up to 10 seconds\n", ffmpegStatus);
                }
                if (network_read_retry(readErrorSince[cAVIdx], now))
                {
                    if (avContextTab[cAVIdx]->pb)
                    {
                        avContextTab[cAVIdx]->pb->eof_reached = 0;
                        avContextTab[cAVIdx]->pb->error = 0;
                    }
                    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                    usleep(100000);
                    continue;
                }
                endError = ffmpegStatus;
            }
            if (cAVIdx > 0 && !isWaitingForFinish && ffmpegStatus < 0 &&
                ffmpegStatus != AVERROR_EXIT && !PlaybackDieNow(0))
            {
                /* the end of the second input is not the end of the playback */
                ffmpeg_printf(10, "context %u ended (%d), going on with context 0\n", cAVIdx, ffmpegStatus);
                multiContextEnded[cAVIdx] = 1;
                readStallFired = 0;
                endError = 0;
                cAVIdx = 0;
                wrapped_packet_unref(&packet);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                continue;
            }
            if (readStallFired)
            {
                /* the read was ended by the stall limit, whatever the
                 * demuxer has made of the interruption */
                readStallFired = 0;
                endError = AVERROR(ETIMEDOUT);
            }
            if( 0 != ffmpegStatus )
            {
                static char errbuf[256];

                /* a dead live source fails again every 100 ms */
                if (context->playback->isTSLiveMode)
                {
                    repeatedReadError = ffmpegStatus == liveReadError;
                    liveReadError = ffmpegStatus;
                }

                if( !repeatedReadError && 0 == av_strerror(ffmpegStatus, errbuf, sizeof(errbuf)) )
                {
                    /* In this way we inform user about error within the core
                     */
                    printf("{\"log\":\"Frame read error: '%s'\"}\n", errbuf);
                }
                
                /*
                if( ffmpegStatus == AVERROR(EAGAIN) )
                {
                    continue;
                }
                */
                ffmpegStatus = 0;
            }
            
            if(!is_finish_timeout() && !context->playback->isTSLiveMode)
            {
                isWaitingForFinish = 1;
                update_finish_timeout();
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                usleep(100000);
                continue;
            }
            else
            {
                uint8_t bEndProcess = 1;
                if (context->playback->isTSLiveMode)
                {
                    seek_target_bytes = 0;
                    do_seek_target_bytes = true;
                    bEndProcess = 0;
                }
                else if( 1 == context->playback->isLoopMode )
                {
                    int64_t tmpLength = 0;
                    if( 0 == container_ffmpeg_get_length(context, &tmpLength) && tmpLength > 0 && get_play_pts() > 0)
                    {
                        seek_target_seconds = 0;
                        do_seek_target_seconds = 1;

                        bEndProcess = 0;
                        context->output->Command(context, OUTPUT_CLEAR, NULL);
                        context->output->Command(context, OUTPUT_PLAY, NULL);
                        printf("{\"log\":\"Loop mode: jump to the start.\"}\n");
                    }
                }
                
                // av_read_frame failed
                if (!repeatedReadError)
                {
                    ffmpeg_err("no data ->end of file reached ? \n");
                }
                wrapped_packet_unref(&packet);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                if( bEndProcess )
                {
                    if (endError && !PlaybackDieNow(0))
                    {
                        /* not the end of the input: the front end can tell
                         * a lost connection from a stream that has ended */
                        char error[512];
                        av_strerror(endError, error, sizeof(error));
                        E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", error, endError);
                    }
                    break; // while
                }
                else
                {
                    /* a dead live source fails again at once */
                    if (context->playback->isTSLiveMode)
                    {
                        usleep(100000);
                    }
                    continue;
                }
            }
        }
        wrapped_packet_unref(&packet);
        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    } /* while */

    if (swr)
    {
        swr_free(&swr);
    }
    
    if (decoded_frame)
    {
        wrapped_frame_free(&decoded_frame);
    }
    AudioTranscoderClose(&audioTranscoder);
    
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
    mpeg4p2_context_close(mpeg4p2_context);
#endif

    {
        unsigned int i;
        for (i = 0; i < IPTV_AV_CONTEXT_MAX_NUM; i++)
            av_free(lastRadioMessage[i]);
    }
    hevc_hdr_detector_reset(&hevcHdrDetector);
    hevcHdrStream = NULL;
#ifdef HAVE_CLOSED_CAPTIONS
    cc_close();
#endif
    if (pcm_audio_export_enabled)
    {
        ffmpeg_printf(0, "PCM export dropped %"PRIu64" chunks\n", pcmExport.dropped_chunks);
        pcm_export_close(&pcmExport);
    }
    context->playback->isPlaying = 0;
    PlaybackDieNow(1);
    ffmpeg_printf(10, "terminating\n");
    hasPlayThreadStarted = 0;
}

/* **************************** */
/* Container part for ffmpeg    */
/* **************************** */

static int32_t terminating = 0;
static int32_t interrupt_cb(void *ctx)
{
    PlaybackHandler_t *p = (PlaybackHandler_t *)ctx;
    /* rw_timeout bounds one wait on a socket, not the time a demuxer spends
     * in one read: HLS skips a segment that fails and loads the next one,
     * http reconnects, all without returning */
    uint32_t limit = readStallActiveMs;
    if (limit)
    {
        uint32_t now = read_stall_now_ms();
        AVIOContext *pb = readStallPb;
        if (pb && !readStallFired && (uint32_t)pb->bytes_read != readStallBytes)
        {
            /* stream probing: data still arrives */
            readStallBytes = (uint32_t)pb->bytes_read;
            readStallStartMs = now;
        }
        if (p->isPaused)
        {
            /* the time in pause does not count */
            readStallStartMs = now;
        }
        else if (now - readStallStartMs > limit)
        {
            readStallFired = 1;
            return 1;
        }
    }
    return p->abortRequested || PlaybackDieNow(0);
}

#ifdef USE_CUSTOM_IO
typedef struct CustomIOCtx_t 
{ 
    FILE *pFile;
    FILE *pMoovFile;
    int64_t iOffset;
    
    char *szFile;
    uint64_t iFileSize;
    char *szMoovAtomFile;
    uint64_t iMoovAtomOffset;
    /* reads in a row that SAM_ReadFuncWait() has waited for */
    uint32_t iOpenWaits;
    /* all reads that it has waited for on this input */
    uint32_t iOpenWaitsTotal;
} CustomIOCtx_t;

CustomIOCtx_t* custom_io_tab[IPTV_AV_CONTEXT_MAX_NUM] = {NULL, NULL};

int SAM_ReadFunc(void *ptr, uint8_t *buffer, int lSize)
{
    CustomIOCtx_t *io = (CustomIOCtx_t *)ptr;
    int ret = 0;
    
    /* the end-of-file indicator is sticky (glibc 2.28): without clearing
     * it fread does not try again in a file that is still growing */
    clearerr(io->pFile);
    if (io->pMoovFile)
    {
        clearerr(io->pMoovFile);
    }

    if (!io->pMoovFile)
    {
        ret = (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pFile );
    }
    else
    {
        if (io->iOffset < io->iMoovAtomOffset)
        {
            ret = (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pFile );
            buffer += ret;
            lSize -= ret;
        }
        
        if (io->iOffset + ret >= io->iMoovAtomOffset)
        {
            if (ret)
            {
                if (fseeko(io->pMoovFile, io->iOffset + ret - io->iMoovAtomOffset, SEEK_SET))
                {
                    // something goes wrong
                    ffmpeg_err("fseeko on moov atom file fail \n");
                    lSize = 0;
                }
            }
            
            ret += (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pMoovFile );
        }
        
        io->iOffset += ret;
    }
    if (ret == 0 && !progressive_playback)
    {
        if (ferror(io->pFile) || (io->pMoovFile && ferror(io->pMoovFile)))
            return AVERROR(errno ? errno : EIO);
        return AVERROR_EOF;
    }
    return ret;
}

/* Read function while an input is opened in progressive playback. At the
 * current end of a growing file SAM_ReadFunc() returns 0, and libavformat
 * asks again at once: avio_read(), matroska_resync() and avi_sync() loop
 * without a pause until the download has delivered more. Wait 100 ms
 * before the retry, for at most 10 seconds in a row and 60 seconds in all
 * per input; after that, and when the player is closing, the 0 is passed
 * on as SAM_ReadFunc() gives it. */
#define SAM_OPEN_WAITS_MAX 100
#define SAM_OPEN_WAITS_TOTAL_MAX 600
static int SAM_ReadFuncWait(void *ptr, uint8_t *buffer, int lSize)
{
    CustomIOCtx_t *io = (CustomIOCtx_t *)ptr;
    int ret = SAM_ReadFunc(ptr, buffer, lSize);

    if (ret == 0 && io->iOpenWaits < SAM_OPEN_WAITS_MAX &&
        io->iOpenWaitsTotal < SAM_OPEN_WAITS_TOTAL_MAX && !PlaybackDieNow(0))
    {
        io->iOpenWaits += 1;
        io->iOpenWaitsTotal += 1;
        usleep(100000);
        ret = SAM_ReadFunc(ptr, buffer, lSize);
    }
    if (ret > 0)
    {
        io->iOpenWaits = 0;
    }
    return ret;
}

// whence: SEEK_SET, SEEK_CUR, SEEK_END (like fseek) and AVSEEK_SIZE
int64_t SAM_SeekFunc(void *ptr, int64_t pos, int whence)
{   
    CustomIOCtx_t *io = (CustomIOCtx_t *)ptr;
    int64_t ret = -1;
    if (!io->pMoovFile)
    {
        if (whence == AVSEEK_SIZE)
        {
            struct stat st;
            if (fstat(fileno(io->pFile), &st) < 0)
                return AVERROR(errno);
            return st.st_size;
        }
        else
        {
            ret = (int64_t)fseeko(io->pFile, (off_t)pos, whence);
            if(0 == ret)
            {
                ret = (int64_t)ftello(io->pFile);
            }
        }
    }
    else
    {
        switch(whence)
        {
            case SEEK_SET:
                ret = pos;
                break;
            case SEEK_CUR:
                ret = io->iOffset + pos;
                break;
            case SEEK_END:
                ret = io->iFileSize + pos;
                break;
            case AVSEEK_SIZE:
                return io->iFileSize;
            default:
                return -1;
        }
        
        if (ret >= 0 && ret <= io->iFileSize)
        {
            if (ret < io->iMoovAtomOffset)
            {
                if(!fseeko(io->pFile, (off_t)ret, SEEK_SET))
                    io->iOffset = ret;
                else
                    ret = -1;
            }
            else
            {
                if(!fseeko(io->pMoovFile, (off_t)(ret - io->iMoovAtomOffset), SEEK_SET))
                    io->iOffset = ret;
                else
                    ret = -1;
            }
        }
        else
        {
            ret = -1;
        }
    }
    return ret;
}

AVIOContext* container_ffmpeg_get_avio_context(CustomIOCtx_t *custom_io, size_t avio_ctx_buffer_size)
{
    if(strstr(custom_io->szFile, "file://") == custom_io->szFile)
        custom_io->szFile += 7;
    
    custom_io->pFile = fopen(custom_io->szFile, "rb");
    if(NULL == custom_io->pFile)
    {
        return NULL;
    }
    
    if (custom_io->szMoovAtomFile && custom_io->szMoovAtomFile[0] != '\0')
    {
        if(strstr(custom_io->szMoovAtomFile, "file://") == custom_io->szMoovAtomFile)
            custom_io->szMoovAtomFile += 7;
        
        custom_io->pMoovFile = fopen(custom_io->szMoovAtomFile, "rb");
        if(NULL == custom_io->pMoovFile)
        {
            fclose(custom_io->pFile);
            return NULL;
        }
    }
    
    AVIOContext *avio_ctx = NULL;
    uint8_t *avio_ctx_buffer = NULL;
    
    avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
    if (avio_ctx_buffer)
    {
        avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, custom_io,
                                      progressive_playback ? &SAM_ReadFuncWait : &SAM_ReadFunc, NULL, &SAM_SeekFunc);
    }
    if (!avio_ctx) 
    {
        av_free(avio_ctx_buffer);
        if (custom_io->pMoovFile)
        {
            fclose(custom_io->pMoovFile);
            custom_io->pMoovFile = NULL;
        }
        fclose(custom_io->pFile);
        custom_io->pFile = NULL;
        return NULL;
    }
    return avio_ctx;
}

/* Release what container_ffmpeg_get_avio_context() has set up for an input.
 * libavformat does not close an I/O context that the caller has supplied. */
static void container_ffmpeg_free_custom_io(int32_t AVIdx, AVIOContext **pb)
{
    if (*pb)
    {
        av_freep(&(*pb)->buffer);
        av_freep(pb);
    }
    if (custom_io_tab[AVIdx])
    {
        if (custom_io_tab[AVIdx]->pMoovFile)
        {
            fclose(custom_io_tab[AVIdx]->pMoovFile);
        }
        if (custom_io_tab[AVIdx]->pFile)
        {
            fclose(custom_io_tab[AVIdx]->pFile);
        }
        free(custom_io_tab[AVIdx]);
        custom_io_tab[AVIdx] = NULL;
    }
    use_custom_io[AVIdx] = 0;
}
#endif

/* The http protocol of FFmpeg inflates a gzip answer only when built with
 * zlib and fails the request otherwise. Only a build configured with
 * --enable-zlib counts; an autodetected zlib does not show in the string. */
static int ffmpeg_has_zlib(void)
{
    const char *cfg = avformat_configuration();
    return cfg && strstr(cfg, "--enable-zlib") && !strstr(cfg, "--disable-zlib");
}

/* The HLS segments this build plays directly, the bits of "hlsseg" in
 * EPLAYER3_EXTENDED. HLS_SEG_ANY_NAME: segments named .js, .png, .html or
 * without an extension; extension_picky is off for http input below, and
 * FFmpeg before 7.1.1 does not check the names. HLS_SEG_IMAGE_HEAD: a PNG,
 * JPEG, GIF or WebP image in front of the MPEG-TS data of every segment is
 * skipped; the hls demuxer of the linked FFmpeg has skip_image_head. */
#define HLS_SEG_ANY_NAME   1
#define HLS_SEG_IMAGE_HEAD 2

int32_t hls_segment_caps(void)
{
    int32_t caps = HLS_SEG_ANY_NAME;
#if LIBAVFORMAT_VERSION_MAJOR >= 58
    /* demuxers are found without av_register_all() */
    const AVInputFormat *hls = av_find_input_format("hls");
    if (hls && hls->priv_class &&
        av_opt_find((void *)&hls->priv_class, "skip_image_head", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ))
    {
        caps |= HLS_SEG_IMAGE_HEAD;
    }
#endif
    return caps;
}

/* The watchdog ended a call of the open phase although the context is still
 * there. Its reads were cut off at an unknown point, so it is given up like
 * a context that did not open. Closed before the watchdog is taken off: the
 * close then does not wait for the network either. Only http(s) contexts
 * have a limit, so there is no custom IO to release here. */
static int32_t open_stalled(int32_t AVIdx, const char *what)
{
    char error[512];

    ffmpeg_err("%s not finished after %u s, ended\n", what, readStallLimitMs[AVIdx] / 1000);
    readStallPb = NULL;
    avformat_close_input(&avContextTab[AVIdx]);
    read_stall_disarm();
    av_strerror(AVERROR(ETIMEDOUT), error, 512);
    E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", error, AVERROR(ETIMEDOUT));
    return cERR_CONTAINER_FFMPEG_OPEN;
}

int32_t container_ffmpeg_init_av_context(Context_t *context, char *filename, uint64_t fileSize, char *moovAtomFile, uint64_t moovAtomOffset, int32_t AVIdx)
{
    int32_t err = 0;
    AVInputFormat *fmt = NULL;
    AVIOContext *custom_pb = NULL;
    avContextTab[AVIdx] = avformat_alloc_context();
    if (!avContextTab[AVIdx])
    {
        ffmpeg_err("cannot allocate the format context\n");
        return cERR_CONTAINER_FFMPEG_NOMEM;
    }
    avContextTab[AVIdx]->interrupt_callback.callback = interrupt_cb;
    avContextTab[AVIdx]->interrupt_callback.opaque = context->playback;

#ifdef USE_CUSTOM_IO
    if(0 == strstr(filename, "://") || 
       0 == strncmp(filename, "file://", 7))
    {
        AVIOContext *avio_ctx = NULL;
        custom_io_tab[AVIdx] = calloc(1, sizeof(CustomIOCtx_t));
        if (!custom_io_tab[AVIdx])
        {
            ffmpeg_err("cannot allocate custom IO context\n");
            avformat_free_context(avContextTab[AVIdx]);
            avContextTab[AVIdx] = NULL;
            return cERR_CONTAINER_FFMPEG_NOMEM;
        }
        
        custom_io_tab[AVIdx]->szFile = filename;
        custom_io_tab[AVIdx]->iFileSize = fileSize;
        custom_io_tab[AVIdx]->szMoovAtomFile = moovAtomFile;
        custom_io_tab[AVIdx]->iMoovAtomOffset = moovAtomOffset;
        
        errno = 0;
        avio_ctx = container_ffmpeg_get_avio_context(custom_io_tab[AVIdx], 4096);
        if(avio_ctx)
        {
            avContextTab[AVIdx]->pb = avio_ctx;
            custom_pb = avio_ctx;
            use_custom_io[AVIdx] = 1;
        }
        else
        {
            /* reported like an input that FFmpeg cannot open */
            char error[512];
            int32_t ioErr = AVERROR(errno ? errno : EIO);
            ffmpeg_err("cannot open %s (%d)\n", filename, ioErr);
            av_strerror(ioErr, error, sizeof(error));
            E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", error, ioErr);
            free(custom_io_tab[AVIdx]);
            custom_io_tab[AVIdx] = NULL;
            avformat_free_context(avContextTab[AVIdx]);
            avContextTab[AVIdx] = NULL;
            return cERR_CONTAINER_FFMPEG_OPEN;
        }
    }
#endif

    AVDictionary *avio_opts = NULL;
    AVDictionary **pavio_opts = NULL;
    av_dict_copy(&avio_opts, g_avio_opts, 0);
    readStallLimitMs[AVIdx] = 0;

    eRTMPProtoImplType rtmpProtoImplType = RTMP_NONE;
    uint8_t numOfRTMPImpl = 0;
    if (0 == strncmp(filename, "ffrtmp", 6))
    {
        filename = filename + 2;
        rtmpProtoImplType = RTMP_NATIVE;
    }
    
    if (1 == rtmp_proto_impl)
    {
        rtmpProtoImplType = RTMP_NATIVE;
    } 
    else if (2 == rtmp_proto_impl)
    {
        rtmpProtoImplType = RTMP_LIBRTMP;
    }

    if (0 == strncmp(filename, "rtmp://", 7) ||
        0 == strncmp(filename, "rtmpe://", 8) ||
        0 == strncmp(filename, "rtmps://", 8) ||
        0 == strncmp(filename, "rtmpt://", 8) ||
        0 == strncmp(filename, "rtmpte://", 9) ||
        0 == strncmp(filename, "rtmpts://", 9))
    {
        /* At first we need to check which protocol 
         * implementations we have
         */
        void *opaque = NULL;
        const char *protoName = NULL;
        uint8_t haveNativeProto = 0;
        
        while (protoName = avio_enum_protocols(&opaque, 1))
        {
            if (0 == strcmp("rtmp", protoName))
            {
                ++numOfRTMPImpl;
            }
            else if (0 == strcmp("ffrtmp", protoName))
            {
                // ffmpeg has patch to have both native and librtmp implementations
                ++numOfRTMPImpl;
                haveNativeProto = 2;
            }
            else if (0 == strncmp("rtmpts", protoName, 6))
            {
                // rtmpts is only available in native implementation
                // rtmpts is listed after rtmp
                haveNativeProto = 1;
            }
        }
        
        if (haveNativeProto > 0)
        {
            if (numOfRTMPImpl > 1) // we have both
            {
                if (rtmpProtoImplType == RTMP_NONE)
                {
                    /* if we have both impl, we will prefer native
                     * unless uri contain param wich can be understandable
                     * only by librtmp
                     */
                    if (strstr(filename, " token=") || 
                        strstr(filename, " jtv="))
                    {
                        rtmpProtoImplType = RTMP_LIBRTMP;
                    }
                    else
                    {
                        rtmpProtoImplType = RTMP_NATIVE;
                    }
                }
            }
            else
            {
                rtmpProtoImplType = RTMP_NATIVE;
            }
        }
        else
        {
            rtmpProtoImplType = RTMP_LIBRTMP;
        }
        
        if (RTMP_NATIVE == rtmpProtoImplType)
        {
            char *baseUri = strdup(filename);
            char *token  = NULL;

            if (!baseUri)
            {
                ffmpeg_err("cannot allocate the rtmp uri\n");
                av_dict_free(&avio_opts);
                avformat_free_context(avContextTab[AVIdx]);
                avContextTab[AVIdx] = NULL;
                return cERR_CONTAINER_FFMPEG_NOMEM;
            }
            
            // check if uri have additional params
            if ((token = strtok(baseUri, " ")) != NULL )
            {
                char *conn = malloc(strlen(filename));
                char *swfUrl = malloc(strlen(filename));
                char *swfVfy = malloc(strlen(filename));
                char *poseq, *key, *value;
                if (!conn || !swfUrl || !swfVfy)
                {
                    ffmpeg_err("cannot allocate the rtmp parameters\n");
                    free(conn);
                    free(swfUrl);
                    free(swfVfy);
                    free(baseUri);
                    av_dict_free(&avio_opts);
                    avformat_free_context(avContextTab[AVIdx]);
                    avContextTab[AVIdx] = NULL;
                    return cERR_CONTAINER_FFMPEG_NOMEM;
                }
                conn[0] = '\0';
                swfUrl[0] = '\0';
                swfVfy[0] = '\0';
                token = NULL;
                while((token = strtok(token, " ")) != NULL)
                {
                    if ((poseq = strchr(token, '=')) != NULL)
                    {
                        *poseq = '\0';
                        key = token;
                        value = poseq + 1;
                        ffmpeg_printf(20, "rtmp_key = \"%s\", rtmp_value = \"%s\"\n", key, value);
                        /* translate librtmp connection parameters to ffmpeg ones routin provided by @mx3L
                         *
                         * librtmp parameters     - https://rtmpdump.mplayerhq.hu/librtmp.3.html
                         * ffmpeg rtmp parameters - https://ffmpeg.org/ffmpeg-protocols.html#rtmp
                         */
                        if (!strcasecmp(key, "app"))
                        {
                            av_dict_set(&avio_opts, "rtmp_app", value, 0);
                        }
                        else if (!strcasecmp(key, "conn"))
                        {
                            if (conn[0] != '\0')
                            {
                                strcat(conn, " ");
                            }
                            strcat(conn, value);
                        }
                        else if (!strcasecmp(key, "buffer"))
                        {
                            av_dict_set(&avio_opts, "rtmp_buffer", value, 0);
                        }
                        else if (!strcasecmp(key, "flashVer"))
                        {
                            av_dict_set(&avio_opts, "rtmp_flashver", value, 0);
                        }
                        else if (!strcasecmp(key, "live"))
                        {
                            av_dict_set(&avio_opts, "rtmp_live", value, 0);
                        }
                        else if (!strcasecmp(key, "pageUrl"))
                        {
                            av_dict_set(&avio_opts, "rtmp_pageurl", value, 0);
                        }
                        else if (!strcasecmp(key, "playpath"))
                        {
                            av_dict_set(&avio_opts, "rtmp_playpath", value, 0);
                        }
                        else if (!strcasecmp(key, "subscribe"))
                        {
                            av_dict_set(&avio_opts, "rtmp_subscribe", value, 0);
                        }
                        else if (!strcasecmp(key, "swfUrl"))
                        {
                            strcpy(swfUrl, value);
                        }
                        // ffmpeg expects this value to contain url to player swf
                        // not a 1|0|TRUE like librtmp
                        else if (!strcasecmp(key, "swfVfy"))
                        {
                            strcpy(swfVfy, value);
                        }
                        else if (!strcasecmp(key, "tcUrl"))
                        {
                            av_dict_set(&avio_opts, "rtmp_tcurl", value, 0);
                        }
                        // timeout is ment for incoming connections
                        else if (!strcasecmp(key, "timeout"))
                        {
                        }
                        else
                        {
                            // threat as direct options
                            // for example rtmp_swfhash, rtmp_swfsize
                            av_dict_set(&avio_opts, key, value, 0);
                        }
                    }
                    token = NULL;
                }
                
                if (conn[0] != '\0')
                {
                    av_dict_set(&avio_opts, "rtmp_conn", conn, 0);
                }
                free(conn);

                if (swfUrl[0] != '\0')
                {
                    if (swfVfy[0] == '1' || !strncasecmp(swfVfy, "true", 4))
                    {
                        av_dict_set(&avio_opts, "rtmp_swfverify", swfUrl, 0);
                    }
                    else
                    {
                        av_dict_set(&avio_opts, "rtmp_swfurl", swfUrl, 0);
                    }
                }
                free(swfUrl);
                free(swfVfy);
            }
            
            if (2 == haveNativeProto)
            {
                filename = malloc(strlen(baseUri) + 2 + 1);
                if (!filename)
                {
                    ffmpeg_err("cannot allocate the rtmp uri\n");
                    free(baseUri);
                    av_dict_free(&avio_opts);
                    avformat_free_context(avContextTab[AVIdx]);
                    avContextTab[AVIdx] = NULL;
                    return cERR_CONTAINER_FFMPEG_NOMEM;
                }
                strncpy(filename, "ff", 2);
                strcpy(filename+2, baseUri);
                free(baseUri);
                // memory leak, only once, so does not matter
            }
            else
            {
                filename = baseUri;
                // memory leak, only once, so does not matter
            }
        }
    }
    else if(0 == strncmp(filename, "http://", 7) || 
            0 == strncmp(filename, "https://", 8))
    {
    	char num[16];

    	sprintf( num, "%u000", context->playback->httpTimeout );
        av_dict_set(&avio_opts, "timeout", num, 0); // default is 10s
        av_dict_set(&avio_opts, "rw_timeout", num, 0);
        /* a read that returns nothing for 3x the timeout, at least 60 s, is
         * ended; a timeout of 0 (none) leaves that off as well */
        if (context->playback->httpTimeout)
        {
            uint64_t limit = (uint64_t)context->playback->httpTimeout * 3;
            readStallLimitMs[AVIdx] = limit < 60000 ? 60000 : limit > INT32_MAX ? INT32_MAX : (uint32_t)limit;
        }
        av_dict_set(&avio_opts, "reconnect", "1", 0);
        /* the default User-Agent of the FFmpeg 6.1-derived companion tree;
         * libavformat without that change sends "Lavf/<version>", which some
         * servers refuse. -u (or -f user_agent=) replaces it, and a
         * User-Agent line given with -h goes before both, as in libavformat. */
        if (!av_dict_get(avio_opts, "user_agent", NULL, 0))
        {
            av_dict_set(&avio_opts, "user_agent", DEFAULT_HTTP_USER_AGENT, 0);
        }
        if (context->playback->isTSLiveMode) // special mode for live TS stream with skip packet 
        {
            av_dict_set(&avio_opts, "seekable", "0", 0);
            av_dict_set(&avio_opts, "reconnect_at_eof", "1", 0);
            av_dict_set(&avio_opts, "reconnect_streamed", "1", 0);
        }

        if (strstr(filename, ".m3u"))
        {
            /* a large live playlist is reloaded every few seconds and comes
             * much smaller with gzip; FFmpeg asks for it only when told.
             * Not without zlib, not over an Accept-Encoding of the caller. */
            AVDictionaryEntry *enc = av_dict_get(avio_opts, "headers", NULL, 0);
            if (ffmpeg_has_zlib() && (!enc || !enc->value || !av_stristr(enc->value, "Accept-Encoding:")))
            {
                if (enc && enc->value && enc->value[0])
                {
                    size_t elen = strlen(enc->value);
                    if (elen < 2 || enc->value[elen - 2] != '\r' || enc->value[elen - 1] != '\n')
                    {
                        av_dict_set(&avio_opts, "headers", "\r\n", AV_DICT_APPEND);
                    }
                }
                av_dict_set(&avio_opts, "headers", "Accept-Encoding: gzip\r\n", AV_DICT_APPEND);
            }
        }
        /* segments behind tracking redirects carry no file extension
         * (FAST channels), others are named .jpg or .php: the content
         * decides. For every http url, a playlist is found by its content
         * type too (.php?id=, a token path), not only by .m3u in its name.
         * Options of the HLS demuxer only, the others leave them unused. */
        av_dict_set(&avio_opts, "allowed_segment_extensions", "ALL", 0);
        av_dict_set(&avio_opts, "extension_picky",          "0", 0);
        /* such segments sometimes carry a picture in front of the TS data
         * (image CDNs): read from the TS data on, see hls_segment_caps() */
        av_dict_set(&avio_opts, "skip_image_head",          "1", 0);
        /* a segment that fails to open is asked for again before the
         * demuxer skips it (default: skipped at the first failure) */
        av_dict_set(&avio_opts, "seg_max_retry",            "3", 0);

        if( strncmp(filename, "http://127.0.0.1", 16) == 0 )
        {
            /* when using with ArchivCZSK, then indicate DRM support */
            /* headers passed with -h need not end with CRLF: add the separator,
               otherwise the new header is glued to the previous one and never
               reaches the server */
            AVDictionaryEntry *hdr = av_dict_get(avio_opts, "headers", NULL, 0);
            if (hdr && hdr->value && hdr->value[0])
            {
                size_t hlen = strlen(hdr->value);
                if (hlen < 2 || hdr->value[hlen - 2] != '\r' || hdr->value[hlen - 1] != '\n')
                {
                    av_dict_set(&avio_opts, "headers", "\r\n", AV_DICT_APPEND);
                }
            }
            av_dict_set(&avio_opts, "headers", "X-DRM-Api-Level: 1\r\n", AV_DICT_APPEND);
        }
    }
    
    int hls_preselect = g_sel_program_id <= 0 && !g_h264_max_width && !g_h264_max_height &&
                        !g_hls_all_variants;
    if (hls_preselect)
        av_dict_set(&avio_opts, "hls_quality_mode", g_quality_mode == 1 ? "1" : "2", 0);
    if (g_default_audio_only)
        av_dict_set(&avio_opts, "hls_audio_default_only", "1", 0);
    pavio_opts = &avio_opts;
    
    av_dict_set(&avio_opts, "dash_subtitle_default_discard", "1", 0);
    /* http only (limit 0 otherwise). With reconnect=1 one read of a server
     * that went silent is retried by http.c for more than three minutes,
     * and the demuxers read more than once. The whole call is bounded:
     * while it runs the context can be freed under the callback. */
    read_stall_arm(readStallLimitMs[AVIdx], NULL);
    if ((err = avformat_open_input(&avContextTab[AVIdx], filename, fmt, pavio_opts)) != 0)
    {
        if (rtmp_proto_impl == 0 && //err == AVERROR_UNKNOWN && 
            rtmpProtoImplType == RTMP_NATIVE &&
            numOfRTMPImpl > 1)
        {
            // retry with librtmp
            err = avformat_open_input(&avContextTab[AVIdx], filename+2, fmt, pavio_opts);
            // filename2 - another memory leak, and also only once, so does not matter 
        }
        
        if (err != 0)
        {
            char error[512];

            read_stall_disarm();
            if (readStallFired)
            {
                ffmpeg_err("open not finished after %u s, ended (%d)\n", readStallLimitMs[AVIdx] / 1000, err);
                /* not what the demuxer made of the interruption */
                err = AVERROR(ETIMEDOUT);
            }
            ffmpeg_err("avformat_open_input failed %d (%s)\n", err, filename);
            av_strerror(err, error, 512);
            E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", error, err);

            if(avio_opts != NULL)
            {
                av_dict_free(&avio_opts);
            }
#ifdef USE_CUSTOM_IO
            /* avformat_open_input() has freed the context, but not the I/O
             * context that it was given */
            if (custom_pb)
            {
                container_ffmpeg_free_custom_io(AVIdx, &custom_pb);
            }
#endif
            return cERR_CONTAINER_FFMPEG_OPEN;
        }
    }
    /* the options no (de)muxer or protocol has taken are left in here: the
     * HLS demuxer takes hls_quality_mode, with any other input it stays */
    if (av_dict_get(avio_opts, "hls_quality_mode", NULL, 0))
        hls_preselect = 0;
    av_dict_free(&avio_opts);

    if (readStallFired)
    {
        /* a demuxer can swallow the interruption and report success */
        return open_stalled(AVIdx, "open");
    }

    if (AVIdx == 0)
        context->playback->isDash = avContextTab[0]->iformat &&
            !strcmp(avContextTab[0]->iformat->name, "dash");

    //ffmpeg5: error: assignment of member 'flags' in read-only object -> so commented out next line
    //avContextTab[AVIdx]->iformat->flags |= AVFMT_SEEK_TO_PTS;
    avContextTab[AVIdx]->flags |= AVFMT_FLAG_GENPTS;

    /* HLS streams have no parser of their own: what the segment header does
     * not carry (MPEG-TS: picture size, frame rate, audio layout) is only
     * filled in by avformat_find_stream_info() */
    if (context->playback->noprobe && !is_hls_input(avContextTab[AVIdx]))
    {
        wrapped_set_max_analyze_duration(avContextTab[AVIdx], 1);
    }

    ffmpeg_printf(20, "find_streaminfo\n");

    /* A slow source can take long here while data keeps coming, so the time
     * without a byte counts. HLS and DASH read through connections of their
     * own, which cannot be seen from here: for them it stays the time of
     * the whole call. */
    read_stall_arm(readStallLimitMs[AVIdx], avContextTab[AVIdx]->pb);
    if (avformat_find_stream_info(avContextTab[AVIdx], NULL) < 0) 
    {
        ffmpeg_err("Error avformat_find_stream_info\n");
    }

    if (readStallFired)
    {
        /* also when the call reports no error: the duration estimate at the
         * end of it has none to report */
        return open_stalled(AVIdx, "stream info");
    }
    read_stall_disarm();

#ifdef USE_CUSTOM_IO
    /* opened: from here on the plain read function, as without the wait */
    if (avContextTab[AVIdx] != NULL && avContextTab[AVIdx]->pb != NULL &&
        avContextTab[AVIdx]->pb->read_packet == SAM_ReadFuncWait)
    {
        avContextTab[AVIdx]->pb->read_packet = SAM_ReadFunc;
    }
#endif

    /* The demuxer took hls_quality_mode and the one variant it opened has no
     * picture here: open the input once more with all variants. */
    if (hls_preselect && hls_preselection_without_writer(avContextTab[AVIdx]))
    {
        ffmpeg_printf(1, "no writer for the video of the preselected variant, opening all variants\n");
#ifdef USE_CUSTOM_IO
        if (use_custom_io[AVIdx])
        {
            /* as in container_ffmpeg_stop() */
            container_ffmpeg_free_custom_io(AVIdx, &(avContextTab[AVIdx]->pb));
        }
#endif
        avformat_close_input(&avContextTab[AVIdx]);
        g_hls_all_variants = 1;
        err = container_ffmpeg_init_av_context(context, filename, fileSize, moovAtomFile, moovAtomOffset, AVIdx);
        g_hls_all_variants = 0;
        return err;
    }

//for buffered io
    if(avContextTab[AVIdx] != NULL && avContextTab[AVIdx]->pb != NULL && !context->playback->isTSLiveMode)
    {
        /* ffmpeg_read_wrapper() has one read function for all inputs: once
         * the first input reads through it, a second input with another
         * kind of I/O (local file plus http) must not replace that function */
        if (AVIdx > 0 && avContextTab[0] && avContextTab[0]->pb &&
            avContextTab[0]->pb->read_packet == ffmpeg_read_wrapper &&
            avContextTab[AVIdx]->pb->read_packet != ffmpeg_real_read_org)
        {
            return 0;
        }
        ffmpeg_real_read_org = avContextTab[AVIdx]->pb->read_packet;
        
        if(0 ==AVIdx && strstr(filename, "://") != 0 && 
           strncmp(filename, "file://", 7) != 0)
        {
            if(ffmpeg_buf_size > 0 && ffmpeg_buf_size > FILLBUFDIFF + FILLBUFPAKET)
            {
                if(avContextTab[AVIdx] != NULL && avContextTab[AVIdx]->pb != NULL)
                {
                    ffmpeg_buf = av_malloc(ffmpeg_buf_size);

                    if(ffmpeg_buf != NULL)
                    {
                        ffmpeg_printf(10, "buffer size=%d\n", ffmpeg_buf_size);
                        
                        ffmpeg_read_org = avContextTab[AVIdx]->pb->read_packet;
                        avContextTab[AVIdx]->pb->read_packet = ffmpeg_read;
                        ffmpeg_seek_org = avContextTab[AVIdx]->pb->seek;
                        avContextTab[AVIdx]->pb->seek = ffmpeg_seek;
                        ffmpeg_buf_read = ffmpeg_buf;
                        ffmpeg_buf_write = ffmpeg_buf;

                        //fill buffer
                        ffmpeg_filler(context, -1, NULL, 0);
                        ffmpeg_start_fillerTHREAD(context);
                    }
                }
            }
        }
        else if (progressive_playback)
        {
            avContextTab[AVIdx]->pb->read_packet = ffmpeg_read_wrapper;
        }
    }
//for buffered io (end)
    
    return 0;
}

int32_t container_ffmpeg_init(Context_t *context, PlayFiles_t *playFilesNames)
{
    int32_t err = 0;

    ffmpeg_printf(10, ">\n");

    if (playFilesNames == NULL) 
    {
        ffmpeg_err("playFilesNames NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }
    
    if (playFilesNames->szFirstFile == NULL) 
    {
        ffmpeg_err("playFilesNames->szFirstFile NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }

    if (context == NULL) 
    {
        ffmpeg_err("context NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }

    ffmpeg_printf(10, "filename %s\n", playFilesNames->szFirstFile);
    if(playFilesNames->szSecondFile)
    {
        ffmpeg_printf(10, "second filename %s\n", playFilesNames->szSecondFile);
    }

    if (isContainerRunning) 
    {
        ffmpeg_err("ups already running?\n");
        return cERR_CONTAINER_FFMPEG_RUNNING;
    }

    if (pcm_audio_export_enabled && pcm_export_init(&pcmExport, PCM_EXPORT_PATH) < 0)
    {
        ffmpeg_err("cannot initialize PCM export: %s\n", strerror(errno));
        return cERR_CONTAINER_FFMPEG_INIT;
    }
    /* initialize ffmpeg */
    wrapped_register_all();
    avformat_network_init();

    if(AV_DEBUG_LEVEL > 0)
    {
        av_log_set_level( AV_DEBUG_LEVEL );
    }
    else
    {
        av_log_set_callback( ffmpeg_silen_callback );
    }

    context->playback->abortRequested = 0;
    int32_t res = container_ffmpeg_init_av_context(context, playFilesNames->szFirstFile, playFilesNames->iFirstFileSize, \
                                                   playFilesNames->szFirstMoovAtomFile, playFilesNames->iFirstMoovAtomOffset, 0);

    if(0 == res && playFilesNames->szSecondFile && playFilesNames->szSecondFile[0] != '\0')
    {
        res = container_ffmpeg_init_av_context(context, playFilesNames->szSecondFile, playFilesNames->iSecondFileSize, \
                                               playFilesNames->szSecondMoovAtomFile, playFilesNames->iSecondMoovAtomOffset, 1);
    }
    
    if(0 != res)
    {
        /* the PCM export and the network layer are set up, the first input
         * may be open, and container_ffmpeg_stop() works on a running
         * container only: let it release what init has set up */
        isContainerRunning = 1;
        container_ffmpeg_stop(context);
        return res;
    }
    
    terminating = 0;
    latestPts = 0;
    isContainerRunning = 1;
    res = container_ffmpeg_update_tracks(context, playFilesNames->szFirstFile, 1);
    return res;
}

int32_t container_ffmpeg_update_tracks(Context_t *context, char *filename, int32_t initial)
{
    Track_t *currAudioTrack = NULL;
    Track_t *currSubtitleTrack = NULL;
    uint32_t addedVideoTracksCount = 0;
    
    if (terminating)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }
    
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    
    if (context->manager->subtitle)
    {
        context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &currSubtitleTrack);
    }

    if (context->manager->audio)
    {
        context->manager->audio->Command(context, MANAGER_GET_TRACK, &currAudioTrack);
    }

    if (context->manager->video)
    {
        context->manager->video->Command(context, MANAGER_INIT_UPDATE, NULL);
    }
    
    if (context->manager->audio)
    {
        context->manager->audio->Command(context, MANAGER_INIT_UPDATE, NULL);
    }
    
    if (context->manager->subtitle)
    {
        context->manager->subtitle->Command(context, MANAGER_INIT_UPDATE, NULL);
    }

    ffmpeg_printf(20, "dump format\n");
    av_dump_format(avContextTab[0], 0, filename, 0);
    
    
    uint32_t cAVIdx = 0;
    for(cAVIdx=0; cAVIdx<IPTV_AV_CONTEXT_MAX_NUM; cAVIdx+=1)
    {
        if(NULL == avContextTab[cAVIdx])
        {
            break;
        }
        AVFormatContext *avContext = avContextTab[cAVIdx];
        select_subtitle_stream(avContext, currSubtitleTrack && currSubtitleTrack->AVIdx == cAVIdx ?
                               currSubtitleTrack->Id : -1);
        uint32_t *stream_index = NULL;
        uint32_t nb_stream_indexes = 0;
        
        ffmpeg_printf(1, "cAVIdx[%u]: number of streams: %u\n", cAVIdx, avContext->nb_streams);
        
        if (avContext->nb_programs > 0)
        {
            uint32_t n = 0;
            ffmpeg_printf(1, "cAVIdx[%u]: stream with multi programs: num of programs %u\n", cAVIdx, avContext->nb_programs);
            AVProgram *selected = select_program(avContext);
            if (!selected)
            {
                ffmpeg_err("no playable program matches the selection and video limits\n");
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                return cERR_CONTAINER_FFMPEG_STREAM;
            }
            stream_index = selected->stream_index;
            nb_stream_indexes = selected->nb_stream_indexes;
        }
        int dash_video = -1;
        int auto_dash = g_sel_program_id <= 0 && avContext->iformat &&
                        !strcmp(avContext->iformat->name, "dash");
        if (auto_dash)
            dash_video = select_dash_video(avContext);
        
        int32_t n = 0;
        for (n = 0; n < avContext->nb_streams; n++) 
        {
            Track_t track;
            AVStream *stream = avContext->streams[n];
            int previous_id = stream->id;
            int32_t version = 0;
            char *encoding = NULL;
            if (get_codecpar(stream)->codec_type == AVMEDIA_TYPE_VIDEO &&
                ((auto_dash && n != dash_video) ||
                 (g_sel_program_id <= 0 && !video_allowed(stream))))
            {
                stream->discard = AVDISCARD_ALL;
                continue;
            }
            
            if (nb_stream_indexes > 0 && stream_index != NULL)
            {
                uint32_t isStreamFromSelProg = 0;
                uint32_t m = 0;
                for (m = 0; m < nb_stream_indexes; m++)
                {
                    if (n == stream_index[m]) 
                    {
                        isStreamFromSelProg = 1;
                        break;
                    }
                }
                
                if (!isStreamFromSelProg) {
                    stream->discard = AVDISCARD_ALL;
                    ffmpeg_printf(1, "cAVIdx[%u]: add DISCARD flag to  stream index[%d]\n", cAVIdx, stream->index);
                    continue; // skip this stream
                }
            }

            encoding = Codec2Encoding((int32_t)get_codecpar(stream)->codec_id, (int32_t)get_codecpar(stream)->codec_type, \
                                      (uint8_t *)get_codecpar(stream)->extradata, \
                                      (int)get_codecpar(stream)->extradata_size, \
                                      (int)get_codecpar(stream)->profile, &version);
            
            if(encoding != NULL && !strncmp(encoding, "A_IPCM", 6) && insert_pcm_as_lpcm)
            {
                encoding = "A_LPCM";
            }

            if (encoding != NULL)
            {
               ffmpeg_printf(1, "%d. encoding = %s - version %d\n", n, encoding, version);
            }

            ffmpeg_printf(10, "track identity: context=%u format=%s index=%d previous_id=%d id=%d type=%d\n",
                          cAVIdx, avContext->iformat ? avContext->iformat->name : "unknown",
                          stream->index, previous_id, stream_track_id(avContext, stream),
                          get_codecpar(stream)->codec_type);

            /* some values in track are unset and therefor copyTrack segfaults.
             * so set it by default to NULL!
             */
            memset(&track, 0, sizeof(track));
            track.AVIdx = cAVIdx; 

            switch (get_codecpar(stream)->codec_type) 
            {
            case AVMEDIA_TYPE_VIDEO:
                ffmpeg_printf(10, "CODEC_TYPE_VIDEO %d\n", get_codecpar(stream)->codec_type);
                // do not discard any stream from second context
                stream->discard = 0 == cAVIdx ? AVDISCARD_ALL : AVDISCARD_DEFAULT; /* by default we discard all video streams */
                if (encoding != NULL) 
                {
                    track.type           = eTypeES;
                    track.version        = version;

                    track.width          = get_codecpar(stream)->width;
                    track.height         = get_codecpar(stream)->height;
                    
                    /* We will return here PAR (Pixel Aspect Ratio) client need to calculate DAR(Display Aspect Ratio)
                     * example: PAR 64:45 DAR 16:9
                     *          Resolution 720x576
                     * Display aspect ratio = (720*64)/(576*45) = 16/9
                     * 0:1 is the value for invalid/unset aspect ratio -> https://trac.ffmpeg.org/ticket/3798
                     */
                    track.aspect_ratio_num = stream->sample_aspect_ratio.num;
                    track.aspect_ratio_den = stream->sample_aspect_ratio.den;
                    if (0 == track.aspect_ratio_num  || 0 == track.aspect_ratio_den)
                    {
                        track.aspect_ratio_num = get_codecpar(stream)->sample_aspect_ratio.num;
                        track.aspect_ratio_den = get_codecpar(stream)->sample_aspect_ratio.den;
                    }

                    track.extraData      = get_codecpar(stream)->extradata;
                    track.extraSize      = get_codecpar(stream)->extradata_size;

                    track.aacbuf         = 0;
                    track.have_aacheader = -1;
                    
                    AVRational rateRational = get_frame_rate(stream);
                    if (rateRational.den!=0)
                    {
                        track.frame_rate = (uint32_t)(1000 * (int64_t)(rateRational.num) / (int64_t)(rateRational.den));
                    }
                    
                    /* fixme: revise this */
                    if (track.frame_rate < 23970)
                    {
                        track.TimeScale = 1001;
                    }
                    else
                    {
                        track.TimeScale = 1000;
                    }
                    
                    ffmpeg_printf(10, "bit_rate       [%"PRId64"]\n", get_codecpar(stream)->bit_rate);
                    ffmpeg_printf(10, "time_base.den  [%d]\n", stream->time_base.den);
                    ffmpeg_printf(10, "time_base.num  [%d]\n", stream->time_base.num);
                    ffmpeg_printf(10, "width          [%d]\n", get_codecpar(stream)->width);
                    ffmpeg_printf(10, "height         [%d]\n", get_codecpar(stream)->height);
                    ffmpeg_printf(10, "frame_rate num [%d]\n", rateRational.num);
                    ffmpeg_printf(10, "frame_rate den [%d]\n", rateRational.den);

                    ffmpeg_printf(10, "frame_rate     [%u]\n", track.frame_rate);
                    ffmpeg_printf(10, "TimeScale      [%u]\n", track.TimeScale);

                    track.Name      = "und";
                    track.Encoding  = encoding;
                    track.stream    = stream;
                    track.Id        = stream_track_id(avContext, stream);

                    track.duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den); 
                    if(stream->duration == AV_NOPTS_VALUE || 0 == strncmp(avContext->iformat->name, "dash", 4)) 
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        track.duration = context_duration_ms(avContext);
                    }
                    
                    if (context->manager->video)
                    {
                        if (get_codecpar(stream)->codec_id == AV_CODEC_ID_MPEG4)
                        {
                            track.avCodecCtx = wrapped_avcodec_get_context(cAVIdx, stream);
                        }
                        ffmpeg_printf(1, "cAVIdx[%u]: MANAGER_ADD track VIDEO\n", cAVIdx);
                        if( context->manager->video->Command(context, MANAGER_ADD, &track) < 0) 
                        {
                            /* konfetti: fixme: is this a reason to return with error? */
                            ffmpeg_err("failed to add track %d\n", n);
                        }
                        else
                        {
                            if (addedVideoTracksCount == 0) /* at now we can handle only first video track */
                            {
                                stream->discard = AVDISCARD_DEFAULT;
                            }
                            addedVideoTracksCount += 1;
                        }
                    }
                }
                else 
                {
                    ffmpeg_err("codec type video but codec unknown %d\n", get_codecpar(stream)->codec_id);
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ffmpeg_printf(10, "CODEC_TYPE_AUDIO %d\n",get_codecpar(stream)->codec_type);
                // do not discard any stream from second context
                stream->discard = 0 == cAVIdx ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
                if (encoding != NULL)
                {
                    AVDictionaryEntry *lang;
                    track.type = eTypeES;

                    lang = av_dict_get(stream->metadata, "language", NULL, 0);

                    track.Name = lang ? lang->value : "und";

                    ffmpeg_printf(10, "Language %s\n", track.Name);
                    track.Title = stream_title(avContext, stream);

                    track.Encoding       = encoding;
                    track.Description    = Codec2AudioDescription((int32_t)get_codecpar(stream)->codec_id, (int)get_codecpar(stream)->profile);
#if HAVE_CH_LAYOUT
                    track.channels       = get_codecpar(stream)->ch_layout.nb_channels;
#else
                    track.channels       = get_codecpar(stream)->channels;
#endif
                    track.stream         = stream;
                    track.Id             = stream_track_id(avContext, stream);
                    track.aacbuf         = 0;
                    track.have_aacheader = -1;

                    track.duration       = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                    if(stream->duration == AV_NOPTS_VALUE) 
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        track.duration = context_duration_ms(avContext);
                    }
                    
                    if (AudioTranscoderSetupTrack(&track, cAVIdx, stream, encoding))
                    {
                        ffmpeg_printf(10, " Handle transcode = %d\n", track.transcode);
                    }
                    else if(pcm_audio_export_enabled || !strncmp(encoding, "A_IPCM", 6) || !strncmp(encoding, "A_LPCM", 6))
                    {
                        track.inject_as_pcm = 1;
                        track.avCodecCtx = wrapped_avcodec_get_context(cAVIdx, stream);
                        if (!track.avCodecCtx && pcm_audio_export_enabled)
                        {
                            ffmpeg_err("cannot allocate software audio decoder for PCM export\n");
                            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                            return cERR_CONTAINER_FFMPEG_NOMEM;
                        }
                        if (track.avCodecCtx)
                        {
                            ffmpeg_printf(10, " Handle inject_as_pcm = %d\n", track.inject_as_pcm);

                            const AVCodec *codec = avcodec_find_decoder(get_codecpar(stream)->codec_id);

                            int errorCode = codec ?
                                (avcodec_is_open(track.avCodecCtx) ? 0 : avcodec_open2(track.avCodecCtx, codec, NULL)) :
                                AVERROR_DECODER_NOT_FOUND;
                            if(codec != NULL && !errorCode)
                            {
                               ffmpeg_printf(10, "AVCODEC__INIT__SUCCESS\n");
                            }
                            else
                            {
                               ffmpeg_err("AVCODEC__INIT__FAILED error[%d]\n", errorCode);
                               if (pcm_audio_export_enabled)
                               {
                                   releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                                   return cERR_CONTAINER_FFMPEG_INIT;
                               }
                            }
                        }
                    }
                    else if(!strncmp(encoding, "A_PCM", 5))
                    {
                        track.inject_raw_pcm = 1;
                    }
                    else if (get_codecpar(stream)->codec_id == AV_CODEC_ID_AAC_LATM)
                    {
                        const char marker[] = "LATM";
                        track.aacbuflen = sizeof(marker)/sizeof(char);
                        track.aacbuf = malloc(track.aacbuflen);
                        if (!track.aacbuf)
                        {
                            ffmpeg_err("cannot allocate the AAC header\n");
                            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                            return cERR_CONTAINER_FFMPEG_NOMEM;
                        }
                        memcpy(track.aacbuf, marker, track.aacbuflen);
                        
                        ffmpeg_printf(10, "AV_CODEC_ID_AAC_LATM no extradata ACC header should be available in each frame\n");
                        track.have_aacheader = 1;
                    }
                    else if(!strncmp(encoding, "A_AAC_LATM", 10))
                    {
                        ffmpeg_printf(10, "AV_CODEC_ID_AAC_LATM extradata will be used in aac writter\n");
                    }
                    else if (get_codecpar(stream)->codec_id == AV_CODEC_ID_AAC)
                    {
                        /* HLS can carry raw AAC in fMP4; its writer detects ADTS per packet. */
                        if( 0 == strncmp(avContext->iformat->name, "mpegts", 6) )
                        {
                            const char marker[] = "ADTS";
                            track.aacbuflen = sizeof(marker)/sizeof(char);
                            track.aacbuf = malloc(track.aacbuflen);
                            if (!track.aacbuf)
                            {
                                ffmpeg_err("cannot allocate the AAC header\n");
                                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                                return cERR_CONTAINER_FFMPEG_NOMEM;
                            }
                            memcpy(track.aacbuf, marker, track.aacbuflen);
                            
                            ffmpeg_printf(10, "AV_CODEC_ID_AAC no extradata ACC header should be available in each frame\n");
                            track.have_aacheader = 1;
                        }
                        else
                        {
                            ffmpeg_printf(10, "Create AAC ExtraData\n");
                            ffmpeg_printf(10, "get_codecpar(stream)->extradata_size %d\n", get_codecpar(stream)->extradata_size);
                            //Hexdump(get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);

                            /*  extradata:
                                13 10 56 e5 9d 48 00 (anderen cops)
                                object_type: 00010 2 = LC
                                sample_rate: 011 0 6 = 24000
                                chan_config: 0010 2 = Stereo
                                000 0
                                1010110 111 = 0x2b7
                                00101 = SBR
                                1
                                0011 = 48000
                                101 01001000 = 0x548
                                ps = 0
                                0000000
                            */

                            int32_t object_type = 2; // LC
                            int32_t sample_index = aac_get_sample_rate_index(get_codecpar(stream)->sample_rate);
#if HAVE_CH_LAYOUT
                            int32_t chan_config = get_chan_config(get_codecpar(stream)->ch_layout.nb_channels);
#else
                            int32_t chan_config = get_chan_config(get_codecpar(stream)->channels);
#endif
                            ffmpeg_printf(1,"aac object_type %d\n", object_type);
                            ffmpeg_printf(1,"aac sample_index %d\n", sample_index);
                            ffmpeg_printf(1,"aac chan_config %d\n", chan_config);
                            
                            int off = -1;
                            if (get_codecpar(stream)->extradata_size >= 2)
                            {
                                MPEG4AudioConfig m4ac;
                                off = avpriv_mpeg4audio_get_config(&m4ac, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size * 8, 1);
                                if (off >= 0)
                                {
                                    object_type  = m4ac.object_type;
                                    sample_index = m4ac.sampling_index;
                                    if (sample_index == 0x0f)
                                    {
                                        sample_index = aac_get_sample_rate_index(m4ac.sample_rate);
                                    }
                                    chan_config  = m4ac.chan_config;
                                }
                            }
                            
                            ffmpeg_printf(1,"aac object_type %d\n", object_type);
                            ffmpeg_printf(1,"aac sample_index %d\n", sample_index);
                            ffmpeg_printf(1,"aac chan_config %d\n", chan_config);

                            if (off >= 0 && chan_config == 0) { // channel config must be send in the inband PCE
                                track.aacbuf = malloc(AAC_HEADER_LENGTH + MAX_PCE_SIZE);
                                if (!track.aacbuf)
                                {
                                    ffmpeg_err("cannot allocate the AAC header\n");
                                    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                                    return cERR_CONTAINER_FFMPEG_NOMEM;
                                }

                                GetBitContext gb;
                                PutBitContext pb;
                                init_put_bits(&pb, track.aacbuf + AAC_HEADER_LENGTH, MAX_PCE_SIZE);
                                init_get_bits8(&gb, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);
                                skip_bits_long(&gb, off + 3);

                                put_bits(&pb, 3, 5); //ID_PCE
                                track.aacbuflen = AAC_HEADER_LENGTH + (avpriv_copy_pce_data(&pb, &gb) + 3) / 8;
                                flush_put_bits(&pb);
                            }
                            else {
                                track.aacbuflen = AAC_HEADER_LENGTH;
                                track.aacbuf = malloc(AAC_HEADER_LENGTH+1);
                                if (!track.aacbuf)
                                {
                                    ffmpeg_err("cannot allocate the AAC header\n");
                                    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                                    return cERR_CONTAINER_FFMPEG_NOMEM;
                                }
                            }

                            // https://wiki.multimedia.cx/index.php/ADTS
                            object_type -= 1; //ADTS - profile, the MPEG-4 Audio Object Type minus 1

                            track.aacbuf[0] = 0xFF;
                            track.aacbuf[1] = 0xF1;
                            //track.aacbuf[1] |=0x8;
                            track.aacbuf[2] = ((object_type & 0x03) << 6)  | (sample_index << 2) | ((chan_config >> 2) & 0x01);
                            track.aacbuf[3] = (chan_config & 0x03) << 6;
                            //track.aacbuf[3]|= 0x10;
                            track.aacbuf[4] = 0x00;
                            track.aacbuf[5] = 0x1F;
                            track.aacbuf[6] = 0xFC;

                            //printf("AAC_HEADER -> ");
                            //Hexdump(track.aacbuf,7);
                            track.have_aacheader = 1;
                        }
                        /*
                        else
                        {
                            ffmpeg_err("AV_CODEC_ID_AAC extradata not available\n");
                        }
                        */

                    }
#ifdef __sh__
                    else if(get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAV1
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAV2
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAPRO
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMALOSSLESS) //if (get_codecpar(stream)->extradata_size > 0)
                    {
                        ffmpeg_printf(10,"Create WMA ExtraData\n");

                        // type_specific_data
                        uint16_t codec_id = 0;
                        switch(get_codecpar(stream)->codec_id)
                        {
                            //TODO: What code for lossless ?
                            case AV_CODEC_ID_WMALOSSLESS:
                                codec_id = 0x163; // WMA_LOSSLESS
                                break;
                            case AV_CODEC_ID_WMAPRO:
                                codec_id = 0x162; // WMA_VERSION_9_PRO
                                break;
                            case AV_CODEC_ID_WMAV2:
                                codec_id = 0x161; // WMA_VERSION_2_9
                                break;
                            case AV_CODEC_ID_WMAV1:
                            default:
                                codec_id = 0x160; // WMA_VERSION_1
                                break;
                        }
                        
                        track.aacbuflen = 104 + get_codecpar(stream)->extradata_size;
                        track.aacbuf = malloc(track.aacbuflen);
                        if (!track.aacbuf)
                        {
                            ffmpeg_err("cannot allocate the AAC header\n");
                            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                            return cERR_CONTAINER_FFMPEG_NOMEM;
                        }
                        memset (track.aacbuf, 0, track.aacbuflen);
                        
                        uint8_t ASF_Stream_Properties_Object[16] =
                        {0x91,0x07,0xDC,0xB7,0xB7,0xA9,0xCF,0x11,0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65};
                        
                        memcpy(track.aacbuf + 0, ASF_Stream_Properties_Object, 16); // ASF_Stream_Properties_Object
                        memcpy(track.aacbuf + 16, &track.aacbuflen, 4); //FrameDateLength

                        uint32_t sizehi = 0;
                        memcpy(track.aacbuf + 20, &sizehi, 4); // sizehi (not used)

                        uint8_t ASF_Audio_Media[16] =
                        {0x40,0x9E,0x69,0xF8,0x4D,0x5B,0xCF,0x11,0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B};
                        
                        memcpy(track.aacbuf + 24, ASF_Audio_Media, 16); //ASF_Audio_Media

                        uint8_t ASF_Audio_Spread[16] =
                        {0x50,0xCD,0xC3,0xBF,0x8F,0x61,0xCF,0x11,0x8B,0xB2,0x00,0xAA,0x00,0xB4,0xE2,0x20};
                        
                        memcpy(track.aacbuf + 40, ASF_Audio_Spread, 16); //ASF_Audio_Spread

                        memset(track.aacbuf + 56, 0, 4); // time_offset (not used)
                        memset(track.aacbuf + 60, 0, 4); // time_offset_hi (not used)

                        uint8_t type_specific_data_length = 18 + get_codecpar(stream)->extradata_size;
                        memcpy(track.aacbuf + 64, &type_specific_data_length, 4); //type_specific_data_length

                        uint8_t error_correction_data_length = 8;
                        memcpy(track.aacbuf + 68, &error_correction_data_length, 4); //error_correction_data_length

                        uint16_t flags = 1; // stream_number
                        memcpy(track.aacbuf + 72, &flags, 2); //flags

                        uint32_t reserved = 0;
                        memcpy(track.aacbuf + 74, &reserved, 4); // reserved

                        memcpy(track.aacbuf + 78, &codec_id, 2); //codec_id

                        uint16_t number_of_channels = get_codecpar(stream)->channels;
                        memcpy(track.aacbuf + 80, &number_of_channels, 2); //number_of_channels

                        uint32_t samples_per_second = get_codecpar(stream)->sample_rate;
                        ffmpeg_printf(1, "samples_per_second = %d\n", samples_per_second);
                        memcpy(track.aacbuf + 82, &samples_per_second, 4); //samples_per_second

                        uint32_t average_number_of_bytes_per_second = get_codecpar(stream)->bit_rate / 8;
                        ffmpeg_printf(1, "average_number_of_bytes_per_second = %d\n", average_number_of_bytes_per_second);
                        memcpy(track.aacbuf + 86, &average_number_of_bytes_per_second, 4); //average_number_of_bytes_per_second

                        uint16_t block_alignment = get_codecpar(stream)->block_align;
                        ffmpeg_printf(1, "block_alignment = %d\n", block_alignment);
                        memcpy(track.aacbuf + 90, &block_alignment, 2); //block_alignment

#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                        enum AVSampleFormat sample_fmt = get_codecpar(stream)->format;
#else
                        enum AVSampleFormat sample_fmt = get_codecpar(stream)->sample_fmt;
#endif
                        uint16_t bits_per_sample = sample_fmt>=0 ? (sample_fmt+1)*8 : 8;
                        ffmpeg_printf(1, "bits_per_sample = %d (%d)\n", bits_per_sample, sample_fmt);
                        memcpy(track.aacbuf + 92, &bits_per_sample, 2); //bits_per_sample

                        memcpy(track.aacbuf + 94, &get_codecpar(stream)->extradata_size, 2); //bits_per_sample

                        memcpy(track.aacbuf + 96, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);

                        ffmpeg_printf(1, "aacbuf:\n");
                        track.have_aacheader = 1;
                    }
#endif
                    if (context->manager->audio)
                    {
                        ffmpeg_printf(1, "cAVIdx[%u]: MANAGER_ADD track AUDIO\n", cAVIdx);
                        if (context->manager->audio->Command(context, MANAGER_ADD, &track) < 0)
                        {
                            /* konfetti: fixme: is this a reason to return with error? */
                            ffmpeg_err("failed to add track %d\n", n);
                            /* the manager has not taken the codec header (more
                             * than TRACKWRAP tracks, or no memory for its list) */
                            free(track.aacbuf);
                            track.aacbuf = NULL;
                        }
                    }
                }
                else //encoding != NULL
                {
                    ffmpeg_err("codec type audio but codec unknown %d\n", get_codecpar(stream)->codec_id);
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
            {
                if (get_codecpar(stream)->codec_id != AV_CODEC_ID_SSA &&
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 3, 100)
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_ASS &&
#endif
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_SUBRIP && 
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_TEXT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_SRT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_WEBVTT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_MOV_TEXT &&
#ifdef HAVE_CLOSED_CAPTIONS
                    (get_codecpar(stream)->codec_id != AV_CODEC_ID_EIA_608 || !cc_decoder_available()) &&
#endif
                    ((get_codecpar(stream)->codec_id != AV_CODEC_ID_HDMV_PGS_SUBTITLE &&
                      get_codecpar(stream)->codec_id != AV_CODEC_ID_DVB_SUBTITLE &&
                      get_codecpar(stream)->codec_id != AV_CODEC_ID_XSUB) ||
                     !GetGraphicSubPath() || !GetGraphicSubPath()[0]))
                {
                    ffmpeg_printf(10, "subtitle with not supported codec codec_id[%u]\n", (uint32_t)get_codecpar(stream)->codec_id);
                }
                else if (context->manager->subtitle)
                {
                    AVDictionaryEntry *lang = NULL;
                    memset(&track, 0, sizeof(track));
                    track.AVIdx = cAVIdx;
                    track.is_forced = subtitle_is_forced(stream);

                    ffmpeg_printf(10, "CODEC_TYPE_SUBTITLE %d\n", get_codecpar(stream)->codec_type);
                    
                    lang = av_dict_get(stream->metadata, "language", NULL, 0);
                    track.Name = lang ? lang->value : "und";
                    ffmpeg_printf(10, "Language %s\n", track.Name);
                    track.Title = stream_title(avContext, stream);

                    track.Encoding       = encoding;
                    track.stream         = stream;
                    track.Id             = stream_track_id(avContext, stream);
                    track.duration       = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                    
                    if(stream->duration == AV_NOPTS_VALUE) 
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        track.duration = context_duration_ms(avContext);
                    }

                    ffmpeg_printf(1, "subtitle codec %d\n", get_codecpar(stream)->codec_id);
                    ffmpeg_printf(1, "subtitle width %d\n", get_codecpar(stream)->width);
                    ffmpeg_printf(1, "subtitle height %d\n", get_codecpar(stream)->height);
                    ffmpeg_printf(1, "subtitle stream %p\n", stream);

                    ffmpeg_printf(10, "FOUND SUBTITLE %s\n", track.Name);
                    
                    if (context->manager->subtitle->Command(context, MANAGER_ADD, &track) < 0)
                    {
                        ffmpeg_err("failed to add subtitle track %d\n", n);
                    }
                }
                break;
            }
            case AVMEDIA_TYPE_UNKNOWN:
            case AVMEDIA_TYPE_DATA:
            case AVMEDIA_TYPE_ATTACHMENT:
            case AVMEDIA_TYPE_NB:
            default:
                stream->discard = AVDISCARD_ALL;
                ffmpeg_err("not handled or unknown codec_type %d\n", get_codecpar(stream)->codec_type);
             break;
            }
        } /* for */
    
    }
#ifdef HAVE_CLOSED_CAPTIONS
    cc_add_track(context);
#endif
    
    if (context->manager->audio)
    {
        Track_t *Tracks = NULL;
        int32_t TrackCount = 0;
        int32_t selTrackIdx = -1;
        
        context->manager->audio->Command(context, MANAGER_REF_LIST, &Tracks);
        context->manager->audio->Command(context, MANAGER_REF_LIST_SIZE, &TrackCount);
        if (Tracks && TrackCount)
        {
            int32_t i;
            for (i=0; i < TrackCount; ++i)
            {
                if (Tracks[i].pending || Tracks[i].Id < 0)
                    continue;
                
                if (selTrackIdx == -1)
                    selTrackIdx = i;
                    
                /* The HLS demuxer adds the streams of renditions with a URI
                 * before those of the variant, so a DEFAULT=YES rendition
                 * muxed into the variant is never the first track. */
                if (!currAudioTrack && is_hls_input(avContextTab[Tracks[i].AVIdx]) &&
                    (((AVStream *)Tracks[i].stream)->disposition & AV_DISPOSITION_DEFAULT))
                {
                    selTrackIdx = i;
                    break;
                }
                
                if (currAudioTrack && currAudioTrack->Id == Tracks[i].Id)
                {
                    selTrackIdx = i;
                    break;
                }
            }
            
            if (selTrackIdx > -1)
            {
                ((AVStream*)Tracks[selTrackIdx].stream)->discard = AVDISCARD_DEFAULT;
                if (!currAudioTrack || currAudioTrack->Id != Tracks[selTrackIdx].Id )
                {
                    context->manager->audio->Command(context, MANAGER_SET, &Tracks[selTrackIdx].Id);
                }
            }
        }
    }
    
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_play(Context_t *context)
{
    int32_t error = 0;
    int32_t ret = 0;
    pthread_attr_t attr;

    ffmpeg_printf(10, "\n");

    if ( context && context->playback && context->playback->isPlaying ) 
    {
        ffmpeg_printf(10, "is Playing\n");
    }
    else 
    {
        ffmpeg_printf(10, "is NOT Playing\n");
    }

    if (hasPlayThreadStarted == 0) 
    {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        /* before the thread exists: it clears the flag when it ends */
        hasPlayThreadStarted = 1;
        if((error = pthread_create(&PlayThread, &attr, (void *)&FFMPEGThread, context)) != 0) 
        {
            ffmpeg_printf(10, "Error creating thread, error:%d:%s\n", error,strerror(error));

            hasPlayThreadStarted = 0;
            ret = cERR_CONTAINER_FFMPEG_ERR;
        }
        else 
        {
            ffmpeg_printf(10, "Created thread\n");
        }
    }
    else 
    {
        ffmpeg_printf(10, "A thread already exists!\n");
        ret = cERR_CONTAINER_FFMPEG_ERR;
    }

    ffmpeg_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t container_ffmpeg_stop(Context_t *context) 
{
    int32_t ret = cERR_CONTAINER_FFMPEG_NO_ERROR;
    int32_t wait_time = 200; // Allow up to one second for the worker to stop.

    ffmpeg_printf(10, "\n");

    if (!isContainerRunning)
    {
        ffmpeg_err("Container not running\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    
    if (context->playback)
    {
        context->playback->isPlaying = 0;
    }

    while ( (hasPlayThreadStarted != 0) && (--wait_time) > 0 ) 
    {
        ffmpeg_printf(10, "Waiting for ffmpeg thread to terminate itself, will try another %d times\n", wait_time);
        usleep(5000);
    }

    if (wait_time == 0) 
    {
        /* force close */
        ffmpeg_err( "Timeout waiting for thread!\n");
        ret = cERR_CONTAINER_FFMPEG_ERR;
        /* to speed up close - we are in separate process for the moment this process will 
         * be closed and whole resources will be free by the system  
         */ 
        return ret;
    }

    hasPlayThreadStarted = 0;
    terminating = 1;

    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    
    pcm_export_close(&pcmExport);
    free_all_stored_avcodec_context();
    
    uint32_t i = 0;
    for(i=0; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
    {
        if(NULL != avContextTab[i])
        {
            if(0 != use_custom_io[i])
            {
                /*
                 * Free custom IO independently to avoid segfault/bus error 
                 * avformat_close_input do not expect custom io, so it try
                 * to release incorrectly
                 */
                container_ffmpeg_free_custom_io(i, &(avContextTab[i]->pb));
            }
            avformat_close_input(&avContextTab[i]);
            avContextTab[i] = NULL;
        }
        else
        {
            break;
        }
    }

    if(g_avio_opts != NULL)
    {
        av_dict_free(&g_avio_opts);
    }

    isContainerRunning = 0;
    context->playback->isDash = 0;
    dashAudioResetPending = 0;
    avformat_network_deinit();
    ffmpeg_buf_free();

    releaseMutex(__FILE__, __FUNCTION__,__LINE__);

    ffmpeg_printf(10, "ret %d\n", ret);
    return ret;
}

static int32_t container_ffmpeg_seek_bytes(off_t pos) 
{
    int32_t flag = AVSEEK_FLAG_BYTE;
    off_t current_pos = avio_tell(avContextTab[0]->pb);

    ffmpeg_printf(20, "seeking to position %"PRId64" (bytes)\n", pos);

    if (current_pos > pos)
    {
        flag |= AVSEEK_FLAG_BACKWARD;
    }

    if (avformat_seek_file(avContextTab[0], -1, INT64_MIN, pos, INT64_MAX, flag) < 0)
    {
        ffmpeg_err( "Error seeking\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    ffmpeg_printf(30, "current_pos after seek %"PRId64"\n", avio_tell(avContextTab[0]->pb));

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

#ifdef HAVE_DREAMNEXTGEN
/* Start, change or end the trick play: *arg is the speed, 0 ends it and
 * returns the stream PTS of the last keyframe to go on from. */
static int32_t container_ffmpeg_trick(Context_t *context, int64_t *arg)
{
    int32_t speed = (int32_t)*arg;
    unsigned long long screen = 0;
    Track_t *videoTrack = NULL;
    struct timespec ts;

    if (speed)
    {
        context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
        /* the MPEG-4 part 2 and FLV converters write frames of their own */
        if (!videoTrack || videoTrack->AVIdx != 0 || !videoTrack->stream ||
            get_codecpar(videoTrack->stream)->codec_id == AV_CODEC_ID_MPEG4 ||
            get_codecpar(videoTrack->stream)->codec_id == AV_CODEC_ID_FLV1)
        {
            ffmpeg_err("trick: no video track it can wind\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }
        if (!trickSpeed && (context->output->Command(context, OUTPUT_PTS, &screen) < 0 || !screen))
        {
            ffmpeg_err("trick: no picture on screen yet\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }
    }

    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (speed)
    {
        if (trickSpeed)
        {
            /* a new speed goes on from the last keyframe */
            trickAnchorPts = trickKeyPts;
            trickBasePts = trickOutPts;
        }
        else
        {
            trickAnchorPts = trickKeyPts = trickBasePts = trickOutPts = (int64_t)(screen & 0x1FFFFFFFFULL);
            /* what is queued plays in real time */
            context->output->Command(context, OUTPUT_CLEAR, NULL);
        }
        trickSpeed = speed;
        trickStartNs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        trickDueNs = 0;
        trickMiss = 0;
        trickStep = true;
    }
    else
    {
        *arg = trickSpeed ? trickKeyPts : INVALID_PTS_VALUE;
        trickSpeed = 0;
        trickStep = false;
    }
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    ffmpeg_printf(10, "trick: speed %d from %"PRId64"\n", speed, trickAnchorPts);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

/* The stream PTS of a picture of the trick play. */
static int32_t container_ffmpeg_trick_pts(int64_t *pts)
{
    int32_t speed = trickSpeed;

    if (!speed)
    {
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    int64_t dist = (*pts - trickBasePts) & 0x1FFFFFFFFLL;
    /* a picture from before the last change of the speed */
    if (dist >= (1LL << 32))
    {
        dist = 0;
    }
    int64_t pts_stream = trickAnchorPts + dist * speed;
    *pts = pts_stream < 0 ? 0 : pts_stream & 0x1FFFFFFFFLL;
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}
#endif

static int32_t container_ffmpeg_seek(Context_t *context, int64_t sec, uint8_t absolute)
{
    int zero_seek = !absolute && sec == 0;
    int64_t target_us;
    Track_t *videoTrack = NULL;
    Track_t *audioTrack = NULL;
    Track_t *current = NULL;
    int64_t from_pts = INVALID_PTS_VALUE;
    int64_t read_us = -1;

    if (sec > INT64_MAX / AV_TIME_BASE || sec < INT64_MIN / AV_TIME_BASE)
    {
        ffmpeg_err("seek target is out of range\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    target_us = sec * AV_TIME_BASE;
    /* The playback layer holds isSeeking until the output has been reset. */
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    /* An input that cannot seek (http without ranges, a pipe) only goes
     * forward, by reading: the seek of the play thread fails for a target
     * before the read position, after the output has been cleared, and the
     * playback jumps ahead. Such a seek is refused here. The 0 s seek of a
     * track switch stays as it is. */
    if (!zero_seek && avContextTab[0] && avContextTab[0]->pb && !use_custom_io[0] &&
        !(avContextTab[0]->pb->seekable & AVIO_SEEKABLE_NORMAL) &&
        !is_hls_input(avContextTab[0]) && !context->playback->isDash)
    {
        read_us = av_rescale(latestPts, AV_TIME_BASE, 90000);
    }
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    if (!absolute) 
    {
        ffmpeg_printf(10, "seeking %"PRId64" sec\n", sec);

        int64_t length = 0;

        context->playback->Command(context, PLAYBACK_LENGTH, (void*)&length);

        int64_t currPts = -1;
        int ret = context->playback->Command(context, PLAYBACK_PTS, &currPts);
        if (ret != 0 || currPts < 0 || currPts == INVALID_PTS_VALUE)
        {
            ffmpeg_err("fail to get current PTS\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }
        int64_t cur_sec = currPts / 90000;

//        ffmpeg_err("cur_sec = %lld\n", (long long) cur_sec);
//        ffmpeg_err("length = %lld\n", (long long) length);

        /* 
         * - 0s relative seek is used when switching subtitle or audio tracks to properly flush/reload queues
         * - in some types of live streams this can result in hang or seek behind the end of the stream
         * - try to detect such streams - mostly they have incorrect (negative) length or current position is behind the end
         * - in these cases just ignore seek command
         */

        if (sec == 0 && cur_sec > length)
        {
            ffmpeg_err("sec = 0 ignoring\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }

        target_us = av_sat_add64(target_us,
            av_rescale_q(currPts, (AVRational){1, 90000}, AV_TIME_BASE_Q));
        from_pts = currPts;
    }
    
    ffmpeg_printf(10, "goto %"PRId64" us\n", target_us);
    if (target_us < 0)
        target_us = 0;
    if (read_us >= 0 && target_us < read_us)
    {
        ffmpeg_err("the input cannot seek back to %"PRId64" us, read up to %"PRId64" us\n", target_us, read_us);
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
    context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

    if (videoTrack != NULL)
    {
        current = videoTrack;
    }
    else if (audioTrack != NULL)
    {
        current = audioTrack;
    }
    else
    {
        ffmpeg_err( "no track available to seek\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    if (!context->playback || !context->playback->isPlaying)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }

    getSeekMutex();
    g_zero_seek = zero_seek;
    g_seek_target_seconds = target_us;
    g_seek_from_pts = from_pts;
    g_seek_offset_us = sec * AV_TIME_BASE;
    g_do_seek_target_seconds = true;
    g_stamp = context->playback->stamp;
    releaseSeekMutex();

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_get_length(Context_t *context, int64_t *length) 
{
    ffmpeg_printf(50, "\n");
    Track_t * videoTrack = NULL;
    Track_t * audioTrack = NULL;
    Track_t * current = NULL;

    if (length == NULL) 
    {
        ffmpeg_err( "null pointer passed\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    context->container->selectedContainer->Command(context, CONTAINER_UPDATE_DURATION, NULL);
    context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
    context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

    if (videoTrack != NULL)
    {
        current = videoTrack;
    }
    else if (audioTrack != NULL)
    {
        current = audioTrack;
    }
    
    *length = 0;

    if (current != NULL) 
    {
        if (current->duration <= 0)
        {
            return cERR_CONTAINER_FFMPEG_NO_ERROR;
        }
        else
        {
            *length = current->duration / 1000;
        }
    }
    else 
    {
        if (avContextTab[0] != NULL)
        {
            *length = context_duration_ms(avContextTab[0]) / 1000;
        } 
        else
        {
           ffmpeg_err( "no Track not context ->no problem :D\n");
           return cERR_CONTAINER_FFMPEG_ERR;
        }
    }

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_switch_audio(Context_t *context, int32_t *arg)
{
    int64_t sec = 0;
    int32_t ret;
    ffmpeg_printf(10, "track %d\n", *arg);
    /* PLAYBACK_SEEK is refused while paused or in trick play. The switch then
     * takes effect at the read position, as it does on streams without duration. */
    if (!context->playback->isDash &&
        (!avContextTab[0] || use_custom_io[0] || avContextTab[0]->duration > 0) &&
        !context->playback->isPaused && !context->playback->isForwarding &&
        !context->playback->BackWard && !context->playback->SlowMotion)
    {
        ret = context->playback->Command(context, PLAYBACK_SEEK, &sec);
        if (ret != 0)
            return ret;
    }
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    if (context->manager->audio)
    {
        Track_t *Tracks = NULL;
        int32_t TrackCount = 0;
        
        context->manager->audio->Command(context, MANAGER_REF_LIST, &Tracks);
        context->manager->audio->Command(context, MANAGER_REF_LIST_SIZE, &TrackCount);
        if (Tracks && TrackCount)
        {
            int32_t i;
            for (i=0; i < TrackCount; ++i)
            {
                // do not discard any stream from second context
                if (Tracks[i].AVIdx == 0) {
                    ((AVStream*)Tracks[i].stream)->discard = Tracks[i].Id == *arg ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
                }
            }
        }
    }
    if (context->playback->isDash)
        dashAudioResetPending = 1;
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_switch_subtitle(Context_t *context, int32_t *arg)
{
    ffmpeg_printf(10, "track %d\n", *arg);
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    if (context->manager->subtitle)
    {
        Track_t *selected = NULL;
        int i;
        context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &selected);
        for (i = 0; i < IPTV_AV_CONTEXT_MAX_NUM; i++)
            if (avContextTab[i])
                select_subtitle_stream(avContextTab[i],
                    selected && selected->AVIdx == i ? selected->Id : -1);
    }
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);

    /* subtitles off: nothing has to be read again, a seek would only
     * clear the audio and video buffers */
    if (*arg < 0)
        return cERR_CONTAINER_FFMPEG_NO_ERROR;

    /* This is made to flush inside the buffer because
     * subtitles frame was already read and ignored
     * we seek to force ffmpeg to read once again the same data
     * but now we will not ignore subtitle frame
     */
    int64_t sec = 0;
    context->playback->Command(context, PLAYBACK_SEEK, (void*)&sec);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

/* konfetti comment: I dont like the mechanism of overwriting
 * the pointer in infostring. This lead in most cases to
 * user errors, like it is in the current version (libeplayer2 <-->e2->servicemp3.cpp)
 * From e2 there is passed a tag=strdup here and we overwrite this
 * strdupped tag. This lead to dangling pointers which are never freed!
 * I do not free the string here because this is the wrong way. The mechanism
 * should be changed, or e2 should pass it in a different way...
 */
static int32_t container_ffmpeg_get_info(Context_t* context, char ** infoString)
{
    Track_t *videoTrack = NULL;
    Track_t *audioTrack = NULL;
    char     *meta = NULL;

    ffmpeg_printf(20, ">\n");

    if (avContextTab[0] != NULL)
    {
        if ((infoString == NULL) || (*infoString == NULL))
        {
            ffmpeg_err("infostring NULL\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }

        ffmpeg_printf(20, "%s\n", *infoString);

        context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
        context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

        if ((meta = searchMeta(avContextTab[0]->metadata, *infoString)) == NULL)
        {
            if (audioTrack != NULL)
            {
                AVStream* stream = audioTrack->stream;
                meta = searchMeta(stream->metadata, *infoString);
            }

            if ((meta == NULL) && (videoTrack != NULL))
            {
                AVStream* stream = videoTrack->stream;
                meta = searchMeta(stream->metadata, *infoString);
            }
        }

        if (meta != NULL)
        {
            *infoString = strdup(meta);
        }
        else
        {
            ffmpeg_printf(1, "no metadata found for \"%s\"\n", *infoString);
            *infoString = strdup("not found");
        }
    } 
    else
    {
        ffmpeg_err("avContext NULL\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}


static int32_t container_ffmpeg_update_duration(Context_t *context)
{
    if (terminating)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }

    getMutex(__FILE__, __FUNCTION__,__LINE__);
    ffmpeg_printf(10, "Updating durations\n");

    uint32_t cAVIdx = 0;
    for(cAVIdx=0; cAVIdx<IPTV_AV_CONTEXT_MAX_NUM; cAVIdx+=1)
    {
        if(NULL == avContextTab[cAVIdx])
        {
            break;
        }

        ffmpeg_printf(10, "Updating duration for context #%u\n", cAVIdx);
        AVFormatContext *avContext = avContextTab[cAVIdx];

        int32_t n;

        for (n = 0; n < avContext->nb_streams; n++)
        {
            ffmpeg_printf(10, "Updating duration for stream #%d\n", n);
            Track_t *track = NULL;
            AVStream *stream = avContext->streams[n];

            /* some values in track are unset and therefor copyTrack segfaults.
             * so set it by default to NULL!
             */

            switch (get_codecpar(stream)->codec_type)
            {
                case AVMEDIA_TYPE_VIDEO:
                {
                    ffmpeg_printf(10, "Requesting video track\n");
                    context->manager->video->Command(context, MANAGER_GET_TRACK, &track);

                    if( track && track->stream == stream )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                        if(stream->duration == AV_NOPTS_VALUE || 0 == strncmp(avContext->iformat->name, "dash", 4))
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = context_duration_ms(avContext);
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Video track not found\n");
                    }
                }
                break;

                case AVMEDIA_TYPE_AUDIO:
                {
                    context->manager->audio->Command(context, MANAGER_GET_TRACK, &track);

                    if( track && track->stream == stream )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);

                        if(stream->duration == AV_NOPTS_VALUE)
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = context_duration_ms(avContext);
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Audio track not found\n");
                    }
                }
                break;

                case AVMEDIA_TYPE_SUBTITLE:
                {
                    context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &track);
                    if( track && track->stream == stream )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);

                        if(stream->duration == AV_NOPTS_VALUE)
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = context_duration_ms(avContext);
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Subtitle track not found\n");
                    }

                    break;
                }
            }
        } /* for */
    }

    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t Command(void  *_context, ContainerCmd_t command, void *argument)
{
    Context_t  *context = (Context_t*) _context;
    int ret = cERR_CONTAINER_FFMPEG_NO_ERROR;

    ffmpeg_printf(50, "Command %d\n", command);

    if(command != CONTAINER_SET_BUFFER_SEEK_TIME && 
       command != CONTAINER_SET_BUFFER_SIZE && 
       command != CONTAINER_GET_BUFFER_SIZE && 
       command != CONTAINER_GET_BUFFER_STATUS &&
       command != CONTAINER_STOP_BUFFER && 
       command != CONTAINER_INIT && !avContextTab[0])
    {
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    
    
    switch(command)
    {
    case CONTAINER_INIT:
    {
        PlayFiles_t *playFilesNames = (PlayFiles_t *)argument;
        ret = container_ffmpeg_init(context, playFilesNames);
        break;
    }
    case CONTAINER_PLAY:  
    {
        ret = container_ffmpeg_play(context);
        break;
    }
    case CONTAINER_STOP:
    {
        ret = container_ffmpeg_stop(context);
        break;
    }
    case CONTAINER_SEEK: 
    {
        ret = container_ffmpeg_seek(context, (int64_t)*((int64_t*)argument), 0);
        break;
    }
    case CONTAINER_SEEK_ABS: 
    {
        ret = container_ffmpeg_seek(context, (int64_t)*((int64_t*)argument), -1);
        break;
    }
    case CONTAINER_LENGTH: 
    {
        int64_t length = 0;
        ret = container_ffmpeg_get_length(context, &length);
        *((int64_t*)argument) = (int64_t)length;
        break;
    }
    case CONTAINER_SWITCH_AUDIO: 
    {
        ret = container_ffmpeg_switch_audio(context, (int32_t*) argument);
        break;
    }
    case CONTAINER_SWITCH_SUBTITLE: 
    {
        ret = container_ffmpeg_switch_subtitle(context, (int32_t*) argument);
        break;
    }
    case CONTAINER_INFO:
    {
        ret = container_ffmpeg_get_info(context, (char **)argument);
        break;
    }
    case CONTAINER_STATUS:
    {
        *((int32_t*)argument) = hasPlayThreadStarted;
        break;
    }
    case CONTAINER_LAST_PTS:
    {
        *((int64_t*)argument) = latestPts;
        break;
    }
    case CONTAINER_SET_BUFFER_SIZE:
    {
        ret = container_set_ffmpeg_buf_size((int32_t *) argument);
        break;
    }
    case CONTAINER_GET_BUFFER_SIZE:
    {
        int32_t size = 0;
        ret = container_get_ffmpeg_buf_size(&size);
        *((int32_t*)argument) = size;
        break;
    }   
#ifdef HAVE_DREAMNEXTGEN
    case CONTAINER_TRICK:
    {
        ret = container_ffmpeg_trick(context, (int64_t *)argument);
        break;
    }
    case CONTAINER_TRICK_PTS:
    {
        ret = container_ffmpeg_trick_pts((int64_t *)argument);
        break;
    }
#endif
    case CONTAINER_UPDATE_DURATION:
    {
        ret = container_ffmpeg_update_duration(context);
        break;
    }
    default:
        ffmpeg_err("ContainerCmd %d not supported!\n", command);
        ret = cERR_CONTAINER_FFMPEG_ERR;
        break;
    }

    ffmpeg_printf(50, "exiting with value %d\n", ret);
    return ret;
}

static char *FFMPEG_Capabilities[] = {"aac", "avi", "mkv", "mp4", "ts", "mov", "flv", "flac", "mp3", "mpg", "m2ts", "vob", "evo", "wmv","wma", "asf", "mp2", "m4v", "m4a", "fla", "divx", "dat", "mpeg", "trp", "mts", "vdr", "ogg", "wav", "wtv", "asx", "mvi", "png", "jpg", "jpeg", "ra", "ram", "rm", "3gp", "amr", "rmvb", "rm", "webm", "opus", "m3u8", "mpd", "ac4",
                                     "ac3", "eac3", "ec3", "dts", "dtshd", "thd", "mlp", "mka", "m4b", "aif", "aiff", "ape", NULL };

Container_t FFMPEGContainer = {
    "FFMPEG",
    &Command,
    FFMPEG_Capabilities
};
