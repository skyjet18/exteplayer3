#include <assert.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

/* The play thread reads from the demuxer and decodes with the decoder of the
 * test: software decoded audio and the timestamps it goes out with. */
static int test_read_frame(AVFormatContext *av, AVPacket *packet);
static int test_send_packet(AVCodecContext *codec, const AVPacket *packet);
static int test_receive_frame(AVCodecContext *codec, AVFrame *frame);
#define av_read_frame test_read_frame
#define avcodec_send_packet test_send_packet
#define avcodec_receive_frame test_receive_frame
#include "../container/container_ffmpeg.c"
#undef av_read_frame
#undef avcodec_send_packet
#undef avcodec_receive_frame
/* libavutil/internal.h, which comes with the container, sets NDEBUG. */
#undef NDEBUG
#include <assert.h>

#define MS(n) (INT64_C(90) * (n))

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return 0;
}

void E2iSendMsg(const char *format, ...)
{
    (void)format;
}

int64_t PlaybackLastContinue(void)
{
    return 0;
}

void LinuxDvbBuffSetStamp(void *stamp)
{
    (void)stamp;
}

/* The play thread picks the live sync default of the box. */
stb_type_t GetSTBType(void)
{
    return STB_UNKNOWN;
}

/* The end of the file asks for the position: there is none. */
static int32_t playback_command(void *context, PlaybackCmd_t command, void *argument)
{
    (void)context;
    (void)argument;
    assert(command == PLAYBACK_PTS);
    return -1;
}

static PlaybackHandler_t playback = {.Command = playback_command};

/* Packets of one second; the decoder refuses the one stamped bad_packet. */
static int packets, next_packet;
static int64_t bad_packet;
/* Frames of a packet, whether they carry its timestamp, and how many packets
 * the decoder takes before it gives out the frames of the first. */
static int frames_per_packet, stamped, held_packets;
static int64_t queued[64];
static int queued_frames, taken_packets;

static int test_read_frame(AVFormatContext *av, AVPacket *packet)
{
    (void)av;
    if (next_packet == packets)
    {
        playback.isPlaying = 0;
        return AVERROR_EOF;
    }
    assert(!av_new_packet(packet, 64));
    packet->pts = packet->dts = 1000 * next_packet++;
    packet->duration = 1000;
    packet->stream_index = 0;
    return 0;
}

static int test_send_packet(AVCodecContext *codec, const AVPacket *packet)
{
    int i;
    (void)codec;
    if (!held_packets && queued_frames)
        return AVERROR(EAGAIN);
    if (packet->pts == bad_packet)
        return AVERROR_INVALIDDATA;
    for (i = 0; i < frames_per_packet; i++)
    {
        assert(queued_frames < 64);
        queued[queued_frames++] = stamped && !i ? packet->pts : AV_NOPTS_VALUE;
    }
    taken_packets++;
    return 0;
}

static int test_receive_frame(AVCodecContext *codec, AVFrame *frame)
{
    if (!queued_frames || taken_packets <= held_packets)
        return AVERROR(EAGAIN);
    frame->format = codec->sample_fmt;
    frame->sample_rate = codec->sample_rate;
    frame->nb_samples = codec->sample_rate / frames_per_packet;
    assert(!av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout));
#if !HAVE_CH_LAYOUT
    frame->channels = codec->channels;
    frame->channel_layout = codec->channel_layout;
#endif
    assert(!av_frame_get_buffer(frame, 0));
    assert(!av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
                                   frame->ch_layout.nb_channels, frame->format));
    frame->pts = frame->best_effort_timestamp = queued[0];
    memmove(queued, queued + 1, --queued_frames * sizeof(queued[0]));
    return 0;
}

static int64_t written[64];
static int writes;

static int32_t audio_write(void *context, void *data)
{
    (void)context;
    assert(writes < 64);
    written[writes++] = ((AudioVideoOut_t *)data)->pts;
    return 0;
}

static int32_t video_write(void *context, void *data)
{
    (void)context;
    (void)data;
    assert(!"video written");
    return -1;
}

static int32_t subtitle_write(void *context, void *data)
{
    (void)context;
    (void)data;
    assert(!"subtitle written");
    return -1;
}

static int32_t output_command(void *context, OutputCmd_t command, void *argument)
{
    (void)context;
    if (command == OUTPUT_GET_BUFFER_SIZE)
        *(uint32_t *)argument = 0;
    return 0;
}

static Track_t audio_track;

static int audio_manager(void *context, ManagerCmd_t command, void *argument)
{
    (void)context;
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = &audio_track;
    return 0;
}

static int no_track(void *context, ManagerCmd_t command, void *argument)
{
    (void)context;
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = NULL;
    return 0;
}

static Output_t audio_out = {.Write = audio_write};
static Output_t video_out = {.Write = video_write};
static Output_t subtitle_out = {.Write = subtitle_write};
static OutputHandler_t output = {.audio = &audio_out, .video = &video_out,
                                 .subtitle = &subtitle_out, .Command = output_command};
static Manager_t audio_mgr = {.Command = audio_manager};
static Manager_t other_mgr = {.Command = no_track};
static ManagerHandler_t manager = {.audio = &audio_mgr, .video = &other_mgr, .subtitle = &other_mgr};
static Context_t context = {.playback = &playback, .output = &output, .manager = &manager};

/* A WMA Pro file of 48 kHz stereo, played with the software decoder. */
static void play(int count)
{
    AVFormatContext *av = avformat_alloc_context();
    AVCodecContext *codec = avcodec_alloc_context3(NULL);
    AVStream *stream;

    assert(av && codec);
    stream = avformat_new_stream(av, NULL);
    assert(stream);
    av->iformat = av_find_input_format("asf");
    stream->id = 1;
    stream->time_base = (AVRational){1, 1000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_WMAPRO;
    stream->codecpar->sample_rate = 48000;
    av_channel_layout_default(&stream->codecpar->ch_layout, 2);
    codec->sample_rate = 48000;
    codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&codec->ch_layout, 2);
#if !HAVE_CH_LAYOUT
    codec->channels = 2;
    codec->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    audio_track = (Track_t){.Encoding = "A_IPCM", .Id = 1, .stream = stream,
                            .avCodecCtx = codec, .inject_as_pcm = 1};
    avContextTab[0] = av;
    pts_clock_reset(&ptsClock, 0);
    packets = count;
    next_packet = queued_frames = taken_packets = writes = 0;
    playback.isPlaying = 1;

    FFMPEGThread(&context);

    avContextTab[0] = NULL;
    avcodec_free_context(&codec);
    avformat_free_context(av);
}

/* The decoder gives each packet out as two frames without a timestamp, as
 * the WMA Pro decoder does, and refuses a cut packet. The output starts from
 * the packet the frames came from, at the start and after the bad packet,
 * instead of going out without a timestamp. */
static void frames_without_timestamp(void)
{
    static const int64_t expected[] = {
        MS(0), MS(500), MS(1000), MS(1500), MS(2000), MS(2500), MS(3000), MS(3500),
        MS(5000), MS(5500), MS(6000), MS(6500), MS(7000)
    };
    int i;

    frames_per_packet = 2;
    stamped = 0;
    held_packets = 0;
    bad_packet = 4000;
    play(8);
    assert(writes == (int)(sizeof(expected) / sizeof(expected[0])));
    for (i = 0; i < writes; i++)
        assert(written[i] == expected[i]);
}

/* The decoder gives the frames of a packet out four packets later: more
 * than the 3 s the clock takes for a jump. The frames keep their own time
 * instead of being anchored as a jump to the packets read meanwhile. */
static void frames_behind_packets(void)
{
    int i;

    frames_per_packet = 1;
    stamped = 1;
    held_packets = 4;
    bad_packet = AV_NOPTS_VALUE;
    play(10);
    assert(writes == 6);
    for (i = 0; i < writes; i++)
        assert(written[i] == MS(1000 * i));
}

int main(void)
{
    frames_without_timestamp();
    frames_behind_packets();
    return 0;
}
