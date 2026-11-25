#ifndef OUTPUT_H_
#define OUTPUT_H_

#include <stdio.h>

typedef enum {
    OUTPUT_INIT,
    OUTPUT_ADD,
    OUTPUT_DEL,
    OUTPUT_CAPABILITIES,
    OUTPUT_PLAY,
    OUTPUT_STOP,
    OUTPUT_PAUSE,
    OUTPUT_OPEN,
    OUTPUT_CLOSE,
    OUTPUT_FLUSH,
    OUTPUT_CONTINUE,
    OUTPUT_FASTFORWARD,
    OUTPUT_AVSYNC,
    OUTPUT_CLEAR,
    OUTPUT_PTS,
    OUTPUT_SWITCH,
    OUTPUT_SLOWMOTION,
    OUTPUT_AUDIOMUTE,
    OUTPUT_REVERSE,
    OUTPUT_DISCONTINUITY_REVERSE,
    OUTPUT_GET_FRAME_COUNT,
    OUTPUT_GET_PROGRESSIVE,
    OUTPUT_SET_BUFFER_SIZE,
    OUTPUT_GET_BUFFER_SIZE,
    OUTPUT_GET_VIDEO_INFO,
    OUTPUT_AUDIO_CHANNEL,
} OutputCmd_t;

/* The argument of OUTPUT_AUDIO_CHANNEL points to an int with one of these:
 * the values of enigma2's iAudioChannelSelection (LEFT, STEREO, RIGHT). */
typedef enum
{
    OUTPUT_AUDIO_CHANNEL_LEFT = 0,
    OUTPUT_AUDIO_CHANNEL_STEREO = 1,
    OUTPUT_AUDIO_CHANNEL_RIGHT = 2,
} OutputAudioChannel_t;

typedef struct
{
    uint8_t         *data;
    uint32_t         len;

    uint8_t         *extradata;
    uint32_t         extralen;
    
    int64_t          pts;
    int64_t          dts;
     
    uint32_t         frameRate;
    uint32_t         timeScale;
    
    uint32_t         width;
    uint32_t         height;
    
    uint32_t         infoFlags;
    
    char            *type;
} AudioVideoOut_t;

typedef struct
{
    uint32_t          trackId;
    uint8_t          *data;
    uint32_t          len;

    uint8_t          *extradata;
    uint32_t          extralen;

    int64_t           pts;
    int64_t           dts;
    int64_t           durationMS; // duration in milliseconds

    uint32_t         width;
    uint32_t         height;

    char             *type;
} SubtitleOut_t;


typedef struct Output_s 
{
    char * Name;
    int32_t (* Command) (/*Context_t*/void  *, OutputCmd_t, void *);
    int32_t (* Write) (/*Context_t*/void  *, void *);
    char ** Capabilities;

} Output_t;

extern Output_t LinuxDvbOutput;
extern Output_t SubtitleOutput;

/* only forced subtitle cues (--forced-only, so1/so0) */
void subtitle_forced_only_set(int on);
int subtitle_forced_only_get(void);
#ifdef HAVE_DREAMNEXTGEN
extern Output_t DreamVideoOutput;
#endif

/* The stop of the DVB decoders right before the _exit() of a fallback of
 * the stop signals (main/exteplayer.c): bounded, see linuxdvb_mipsel.c. */
#define LINUXDVB_FALLBACK_NOTHING     0 /* no device open any more */
#define LINUXDVB_FALLBACK_DONE        1 /* stopped and closed */
#define LINUXDVB_FALLBACK_PARTIAL     2 /* a device left to the end of the process */
#define LINUXDVB_FALLBACK_TIMEOUT     3 /* not done within the wait */
#define LINUXDVB_FALLBACK_NOT_STARTED 4 /* no thread for it */
int LinuxDvbFallbackStop(void);

typedef struct OutputHandler_s 
{
    char *Name;
    Output_t *audio;
    Output_t *video;
    Output_t *subtitle;
    int32_t (* Command) (/*Context_t*/void  *, OutputCmd_t, void *);
} OutputHandler_t;

#endif
