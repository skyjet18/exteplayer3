#ifndef MANAGER_H_
#define MANAGER_H_

#include <stdio.h>
#include <stdint.h>
#include "strbuffer.h"
 
typedef enum {
    MANAGER_ADD,
    MANAGER_LIST,
    MANAGER_GET,
    MANAGER_GETNAME,
    MANAGER_SET,
    MANAGER_GETENCODING,
    MANAGER_DEL,
    MANAGER_GET_TRACK,
    MANAGER_GET_TRACK_DESC,
    MANAGER_INIT_UPDATE,
    MANAGER_UPDATED_TRACK_INFO,
    MANAGER_REGISTER_UPDATED_TRACK_INFO,
    MANAGER_REF_LIST,
    MANAGER_REF_LIST_SIZE,
} ManagerCmd_t;

typedef enum {
    eTypeES,
    eTypePES
} eTrackTypeEplayer;

/* Audio stream values for front ends: 0 or "" when unknown. */
typedef struct AudioStreamInfo_s {
    int                   sample_rate;      /* Hz */
    int                   bit_rate;         /* bit/s */
    int                   bits_per_sample;  /* lossless codecs only */
    char                  channel_layout[40];
} AudioStreamInfo_t;

/* Video stream values for front ends: 0, "" or NULL when unknown. */
typedef struct VideoStreamInfo_s {
    char                  profile_level[48];
    int                   bit_depth;
    int                   bit_rate;         /* bit/s */
    int                   dv_profile;       /* Dolby Vision, 0 = none */
    const char           *color_primaries;  /* a constant string or NULL */
} VideoStreamInfo_t;

typedef struct Track_s {
    char *                Name;
    char *                Encoding;
    int32_t               Id;
    int32_t               AVIdx;

    /* new field for ffmpeg - add at the end so no problem
    * can occur with not changed srt saa container
    */
    char                  *language;

    /* length of track */
    int64_t               duration;
    uint32_t              frame_rate;
    uint32_t              TimeScale;
    int32_t               version;
    long long int         pts;
    long long int         dts;
    
    /* for later use: */
    eTrackTypeEplayer     type;
    int                   width;
    int                   height;
    int32_t               aspect_ratio_num;
    int32_t               aspect_ratio_den;

    /* stream from ffmpeg */
    void               *  stream;
    /* AVCodecContext  for steam */
    void               *  avCodecCtx;
    /* codec extra data (header or some other stuff) */
    void               *  extraData;
    int		              extraSize;

    uint8_t*              aacbuf;
    unsigned int          aacbuflen;
    int                   have_aacheader;
    /* audio: name of the codec for display, a constant string or NULL */
    const char           *Description;
    /* audio: channel count from the codec parameters, 0 if unknown */
    int                   channels;
    /* audio: rate, bit rate, sample size, channel layout of the stream */
    AudioStreamInfo_t     audio_info;
    /* video: profile and level, bit depth, bit rate, Dolby Vision, colour */
    VideoStreamInfo_t     video_info;
    /* audio, subtitle: title of the track, owned like Name, NULL if none */
    char                 *Title;

    /* If player2 or the elf do not support decoding of audio codec set this.
     * AVCodec is than used for softdecoding and stream will be injected as PCM */
    int                   inject_as_pcm;
    int                   inject_raw_pcm;
    /* Not 0: the track is decoded and encoded again before it is written,
     * to AC3 (1), to AAC-LC (2) or to E-AC-3 (4). The output is set up for that codec,
     * Encoding keeps the codec of the stream. */
    int                   transcode;

    int                   pending;
    int                   is_forced;
} Track_t;

typedef struct TrackDescription_s 
{
    int                   Id;
    char *                Name;
    char *                Encoding;
    /* audio only: a constant string (not to be freed) or NULL */
    const char *          Description;
    /* audio only: 0 if unknown */
    int                   channels;
    /* audio only */
    AudioStreamInfo_t     audio_info;
    /* video only */
    VideoStreamInfo_t     video_info;
    unsigned int          frame_rate;
    int                   width;
    int                   height;
    int32_t               aspect_ratio_num;
    int32_t               aspect_ratio_den;
    int                   progressive;
    int                   is_forced;
    /* audio, subtitle: to be freed like Name, NULL if the track has none */
    char *                Title;
    
} TrackDescription_t;

typedef struct Manager_s 
{
    char * Name;
    int (* Command) (/*Context_t*/void  *, ManagerCmd_t, void *);
    strbuffer_t *tracks_cache;
    char ** Capabilities;

} Manager_t;

typedef struct ManagerHandler_s 
{
    char *Name;
    Manager_t *audio;
    Manager_t *video;
    Manager_t *subtitle;
} ManagerHandler_t;

void freeTrack(Track_t* track);
void copyTrack(Track_t* to, Track_t* from);
void replaceTrack(Track_t* to, Track_t* from);

#endif
