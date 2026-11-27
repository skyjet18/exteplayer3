#include <assert.h>
#include "../container/container_ffmpeg.c"

static char notification[4096];
static int notifications;
void E2iSendMsg(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(notification, sizeof(notification), format, args);
    va_end(args);
    notifications++;
}

/* misc.c is not linked: the box type is the one of the case. */
static stb_type_t stb_type = STB_OTHER;
stb_type_t GetSTBType(void) { return stb_type; }

/* Neither is external/ffmpeg/src/mpeg4audio.c; Codec2Encoding() asks it for
 * AAC only: the configuration of the case, when there is one. */
static MPEG4AudioConfig aac_config;
static int aac_config_set;
int avpriv_mpeg4audio_get_config(MPEG4AudioConfig *c, const uint8_t *buf,
                                 int bit_size, int sync_extension)
{
    (void)buf;
    (void)bit_size;
    (void)sync_extension;
    if (!aac_config_set)
        abort();
    *c = aac_config;
    return 16;
}

static Track_t seek_track;
static Track_t live_tracks[2];

static int live_audio_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    if (command == MANAGER_REF_LIST)
        *(Track_t **)argument = live_tracks;
    else {
        assert(command == MANAGER_REF_LIST_SIZE);
        *(int32_t *)argument = 2;
    }
    return 0;
}

static int seek_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = &seek_track;
    return 0;
}

static int seek_playback(void *opaque, PlaybackCmd_t command, void *argument)
{
    if (command == PLAYBACK_LENGTH)
        *(int64_t *)argument = 100;
    else {
        assert(command == PLAYBACK_PTS);
        *(int64_t *)argument = 2013750;
    }
    return 0;
}

/* PLAYBACK_SEEK of an audio switch: relative 0, from the playback position */
static int switch_seeks, switch_seek_error;

static int switch_playback(void *opaque, PlaybackCmd_t command, void *argument)
{
    assert(command == PLAYBACK_SEEK && *(int64_t *)argument == 0);
    switch_seeks++;
    return switch_seek_error;
}

static Track_t *subtitle_selected;
static int subtitle_seeks;

static int subtitle_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    assert(command == MANAGER_GET_TRACK);
    *(Track_t **)argument = subtitle_selected;
    return 0;
}

static int subtitle_playback(void *opaque, PlaybackCmd_t command, void *argument)
{
    assert(command == PLAYBACK_SEEK && *(int64_t *)argument == 0);
    subtitle_seeks++;
    return 0;
}

/* The audio PTS dejitter with DTS core frames of 512 samples (NBLKS 15) at a
 * core rate of 48 kHz (SFREQ 13), 10.667 ms or 960 ticks apart, stamped with
 * up to 5 ms of jitter. The stream has the rate of the decoder output: 96 kHz
 * for DTS 96/24 and DTS-HD over that core, 192 kHz for DTS-HD MA 192 kHz.
 * The PTS that go out are the frame positions; with the stream rate they
 * advanced by 480 (or 240) ticks a frame and jumped back to the stream PTS
 * every 200 ms. */
static void dts_dejitter_case(int little_endian, int stream_rate, uint8_t sfreq, int smoothed)
{
    AacPtsDejitter_t dejitter;
    AVStream stream = {0};
    uint8_t frame[32] = {0x7F, 0xFE, 0x80, 0x01, 0xFC, 0x3C, 0x00, 0x00};
    AVPacket packet = {0};
    int i;

    frame[8] = (uint8_t)(sfreq << 2);
    if (little_endian) {
        for (i = 0; i < (int)sizeof(frame); i += 2) {
            uint8_t byte = frame[i];
            frame[i] = frame[i + 1];
            frame[i + 1] = byte;
        }
    }
    stream.codecpar = avcodec_parameters_alloc();
    assert(stream.codecpar);
    stream.codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream.codecpar->codec_id = AV_CODEC_ID_DTS;
    stream.codecpar->sample_rate = stream_rate;
    packet.data = frame;
    packet.size = sizeof(frame);
    aac_pts_dejitter_ms = 200;
    aac_pts_dejitter_reset(&dejitter);
    for (i = 0; i < 300; i++) {
        int64_t position = 900000 + (int64_t)i * 960;
        int64_t stamped = position + (i % 2) * 450;
        int64_t pts = aac_pts_dejitter(&dejitter, &packet, &stream, 1, 0, stamped);
        assert(pts == (smoothed ? position : stamped));
    }
    avcodec_parameters_free(&stream.codecpar);
}

#if HAVE_CH_LAYOUT && defined(AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK)
/* One 7.1.4 frame (the layout of the AC-4 decoder) through swresample the
 * way the PCM path sets it up: front left at value fl, or every channel at
 * value all. Gives the front left of the S16 output and its lowest sample
 * (a wrapped sum would turn negative). */
static void pcm_714_downmix(int out_ch, double maxval, float fl, float all, int *out_fl, int *lowest)
{
    enum { N = 256 };
    AVChannelLayout in = AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK, out;
    static float planes[12][N];
    static int16_t dst[N * 8];
    const uint8_t *src[12];
    uint8_t *d[1] = {(uint8_t *)dst};
    SwrContext *swr = swr_alloc();
    int i, k, n;

    assert(swr);
    av_channel_layout_default(&out, out_ch);
    av_opt_set_chlayout(swr, "in_chlayout", &in, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out, 0);
    av_opt_set_int(swr, "in_sample_rate", 48000, 0);
    av_opt_set_int(swr, "out_sample_rate", 48000, 0);
    av_opt_set_int(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    av_opt_set_int(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    if (maxval > 0)
        av_opt_set_double(swr, "rematrix_maxval", maxval, 0);
    assert(swr_init(swr) == 0);
    for (i = 0; i < 12; i++) {
        for (k = 0; k < N; k++)
            planes[i][k] = i == 0 ? fl + all : all;
        src[i] = (const uint8_t *)planes[i];
    }
    n = swr_convert(swr, d, N, src, N);
    assert(n == N);
    *out_fl = dst[0];
    *lowest = 0;
    for (k = 0; k < n * out_ch; k++)
        if (dst[k] < *lowest)
            *lowest = dst[k];
    swr_free(&swr);
    av_channel_layout_uninit(&out);
}
#endif

/* AC-4: the software decoder on every receiver; more than 8 decoded channels
 * go to the PCM writer as 7.1 at the level of the source. */
static void ac4_pcm_case(void)
{
    int32_t version = 0;
    static const stb_type_t types[] = {STB_VUPLUS, STB_DREAMBOX, STB_HISILICON, STB_OTHER};
    unsigned i;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 21, 100)
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        stb_type = types[i];
        assert(!strcmp(Codec2Encoding(AV_CODEC_ID_AC4, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), "A_IPCM"));
    }
    stb_type = STB_OTHER;
#else
    (void)version;
    (void)types;
    (void)i;
#endif
    assert(pcm_out_channels(1) == 1);
    assert(pcm_out_channels(2) == 2);
    assert(pcm_out_channels(6) == 6);
    assert(pcm_out_channels(8) == 8);
    assert(pcm_out_channels(12) == 8);
    assert(pcm_out_channels(24) == 8);
    assert(pcm_rematrix_maxval(2) == 0.0);
    assert(pcm_rematrix_maxval(6) == 0.0);
    assert(pcm_rematrix_maxval(8) == 0.0);
    assert(pcm_rematrix_maxval(12) == 3.0);

#if HAVE_CH_LAYOUT && defined(AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK)
    {
        int fl_default, fl_kept, lowest;
        const int quarter = 8192; /* 0.25 in S16 */

        /* 7.1: the swresample default scales the front left down,
         * 3.0 keeps it */
        pcm_714_downmix(pcm_out_channels(12), 0, 0.25f, 0, &fl_default, &lowest);
        pcm_714_downmix(pcm_out_channels(12), pcm_rematrix_maxval(12), 0.25f, 0, &fl_kept, &lowest);
        assert(fl_default < quarter - 2000);
        assert(fl_kept >= quarter - 2 && fl_kept <= quarter + 2);
        /* stereo (-s): the coefficients of a front output add up to more
         * than 3, so 3.0 still scales, but three times less than the
         * default */
        pcm_714_downmix(2, 0, 0.25f, 0, &fl_default, &lowest);
        pcm_714_downmix(2, pcm_rematrix_maxval(12), 0.25f, 0, &fl_kept, &lowest);
        assert(fl_default < quarter / 2);
        assert(fl_kept >= 3 * fl_default - 3 && fl_kept <= quarter + 2);
        /* all twelve channels at full scale: the sum saturates, it does
         * not wrap around */
        pcm_714_downmix(pcm_out_channels(12), pcm_rematrix_maxval(12), 0, 1.0f, &fl_kept, &lowest);
        assert(fl_kept == 32767 && lowest >= 0);
        pcm_714_downmix(2, pcm_rematrix_maxval(12), 0, 1.0f, &fl_kept, &lowest);
        assert(fl_kept == 32767 && lowest >= 0);
    }
#endif
}

/* The byte counters of "BR": -1 until a track had a packet, empty packets
 * do not count, 32 bit wrap. */
static void bitrate_bytes_case(void)
{
    int64_t v, a;

    container_ffmpeg_get_bitrate_bytes(&v, &a);
    assert(v == -1 && a == -1);
    bitrate_bytes_add(&bitrate_audio_bytes, &bitrate_have_audio, 0);
    bitrate_bytes_add(&bitrate_audio_bytes, &bitrate_have_audio, -5);
    container_ffmpeg_get_bitrate_bytes(&v, &a);
    assert(v == -1 && a == -1);
    bitrate_bytes_add(&bitrate_audio_bytes, &bitrate_have_audio, 1000);
    bitrate_bytes_add(&bitrate_audio_bytes, &bitrate_have_audio, 24);
    container_ffmpeg_get_bitrate_bytes(&v, &a);
    assert(v == -1 && a == 1024);
    bitrate_video_bytes = UINT32_MAX - 99;
    bitrate_bytes_add(&bitrate_video_bytes, &bitrate_have_video, 100);
    container_ffmpeg_get_bitrate_bytes(&v, &a);
    assert(v == 0 && a == 1024);
    bitrate_bytes_add(&bitrate_video_bytes, &bitrate_have_video, 4096);
    container_ffmpeg_get_bitrate_bytes(&v, &a);
    assert(v == 4096 && a == 1024);
    bitrate_video_bytes = bitrate_audio_bytes = 0;
    bitrate_have_video = bitrate_have_audio = 0;
}

int main(void)
{
    CustomIOCtx_t io = {0};
    bitrate_bytes_case();
    ac4_pcm_case();
    uint8_t bytes[8];
    dts_dejitter_case(0, 48000, 13, 1);
    dts_dejitter_case(0, 96000, 13, 1);
    dts_dejitter_case(1, 96000, 13, 1);
    dts_dejitter_case(0, 192000, 13, 1);
    /* no valid core rate: the stream PTS go out as they are */
    dts_dejitter_case(0, 96000, 0, 0);
    {
        AVStream video = {.id = 11}, audio = {.id = 11};
        Track_t selected_video = {.Id = 11, .AVIdx = 0, .stream = &video};
        Track_t selected_audio = {.Id = 11, .AVIdx = 0, .stream = &audio};
        AVFormatContext av = {0};
        AVInputFormat format = {.name = "hls,applehttp"};
        /* The old ID-only video-first dispatch consumes this audio packet. */
        assert(selected_video.Id == audio.id);
        assert(!packet_matches_track(&selected_video, 0, &audio));
        assert(packet_matches_track(&selected_audio, 0, &audio));
        assert(!packet_matches_track(&selected_audio, 1, &audio));
        assert(!packet_matches_track(NULL, 0, &audio));
        av.iformat = &format;
        assert(is_hls_input(&av));
        format.name = "hls";
        assert(is_hls_input(&av));
        format.name = "applehttp";
        assert(is_hls_input(&av));
        format.name = "dash";
        assert(!is_hls_input(&av));
        format.name = "not-hls";
        assert(!is_hls_input(&av));
        av.iformat = NULL;
        assert(!is_hls_input(&av));
    }
    assert(network_read_retry(100, 100));
    assert(network_read_retry(100, 100 + 10 * AV_TIME_BASE - 1));
    assert(!network_read_retry(100, 100 + 10 * AV_TIME_BASE));
    assert(!network_read_retry(100, 99));
    {
        AVFormatContext av = {0};
        AVStream stream = {0};
        char *last = NULL, *message;
        char control[] = {'"', '\\', '\n', 1, '\t', 0};
        message = json_escape_metadata(control);
        assert(!strcmp(message, "\\\"\\\\\\u000a\\u0001\\u0009"));
        av_free(message);
        message = json_escape_metadata("\xc3\xa9\xff");
        assert(!strcmp(message, "\xc3\xa9\\u00ff"));
        av_free(message);
        message = radio_icy_message("StreamTitle='Artist - Title';StreamUrl='';");
        assert(!strcmp(message, "{\"m_t\":{\"rt\":\"Artist - Title\",\"t\":\"Title\",\"a\":\"Artist\"}}\n"));
        av_free(message);
        message = radio_icy_message("StreamTitle='';");
        assert(!strcmp(message, "{\"m_t\":{\"rt\":\"\",\"t\":\"\",\"a\":\"\"}}\n"));
        av_free(message);
        assert(!radio_icy_message("StreamTitle='broken"));
        av_dict_set(&av.metadata, "title", "Song", 0);
        av_dict_set(&av.metadata, "artist", "Artist", 0);
        radio_metadata_update(&av, NULL, &last, 1);
        assert(!notifications && !last);
        radio_metadata_update(&av, NULL, &last, 0);
        assert(notifications == 1 && strstr(notification, "\"rt\":\"Artist - Song\""));
        av.event_flags = AVFMT_EVENT_FLAG_METADATA_UPDATED;
        radio_metadata_update(&av, NULL, &last, 0);
        assert(notifications == 1 && !av.event_flags);
        av_dict_free(&av.metadata);
        av.event_flags = AVFMT_EVENT_FLAG_METADATA_UPDATED;
        radio_metadata_update(&av, NULL, &last, 0);
        assert(notifications == 2 && strstr(notification, "\"t\":\"\""));
        av_dict_set(&stream.metadata, "TIT2", "Stream tag", 0);
        stream.event_flags = AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        radio_metadata_update(&av, &stream, &last, 0);
        assert(notifications == 3 && strstr(notification, "Stream tag") && !stream.event_flags);
        av_freep(&last);
        radio_metadata_update(&av, &stream, &last, 0);
        assert(notifications == 3);
        stream.event_flags = AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        radio_metadata_update(&av, &stream, &last, 0);
        assert(notifications == 4);
        av_free(last);
        av_dict_free(&stream.metadata);
    }
    {
        AVStream stream = {0};
        assert(!subtitle_is_forced(&stream));
        av_dict_set(&stream.metadata, "language", "eng", 0);
        assert(!subtitle_is_forced(&stream));
        av_dict_set(&stream.metadata, "title", "English [FORCED]", 0);
        assert(subtitle_is_forced(&stream));
        av_dict_set(&stream.metadata, "title", "unforced", 0);
        assert(!subtitle_is_forced(&stream));
        stream.disposition = AV_DISPOSITION_FORCED;
        assert(subtitle_is_forced(&stream));
        av_dict_free(&stream.metadata);
    }
    {
        AVFormatContext *av = avformat_alloc_context();
        AVInputFormat adaptive = {.name = "dash"}, embedded = {.name = "matroska"};
        AVStream *one = avformat_new_stream(av, NULL);
        AVStream *two = avformat_new_stream(av, NULL);
        one->codecpar->codec_type = two->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        one->id = 1;
        two->id = 2;
        av->iformat = &adaptive;
        select_subtitle_stream(av, 2);
        assert(one->discard == AVDISCARD_ALL && two->discard == AVDISCARD_DEFAULT);
        select_subtitle_stream(av, -1);
        assert(one->discard == AVDISCARD_ALL && two->discard == AVDISCARD_ALL);
        av->iformat = &embedded;
        one->discard = two->discard = AVDISCARD_DEFAULT;
        select_subtitle_stream(av, -1);
        assert(one->discard == AVDISCARD_DEFAULT && two->discard == AVDISCARD_DEFAULT);
        av->iformat = NULL;
        avformat_free_context(av);
    }
    {
        /* Switching subtitles on seeks by 0 to read the cues again; off
         * still reaches the streams, but reads nothing again: no seek. */
        AVFormatContext *av = avformat_alloc_context();
        AVInputFormat adaptive = {.name = "dash"};
        AVStream *stream;
        Track_t track = {.Id = 1, .AVIdx = 0};
        PlaybackHandler_t playback = {.Command = subtitle_playback, .isPlaying = 1};
        Manager_t subtitles = {.Command = subtitle_manager};
        ManagerHandler_t managers = {.subtitle = &subtitles};
        Context_t context = {.playback = &playback, .manager = &managers};
        int32_t id = 1;
        assert(av);
        stream = avformat_new_stream(av, NULL);
        assert(stream);
        stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        stream->id = 1;
        av->iformat = &adaptive;
        avContextTab[0] = av;
        subtitle_selected = &track;
        assert(!container_ffmpeg_switch_subtitle(&context, &id));
        assert(stream->discard == AVDISCARD_DEFAULT && subtitle_seeks == 1);
        subtitle_selected = NULL;
        id = -1;
        assert(!container_ffmpeg_switch_subtitle(&context, &id));
        assert(stream->discard == AVDISCARD_ALL && subtitle_seeks == 1);
        avContextTab[0] = NULL;
        av->iformat = NULL;
        avformat_free_context(av);
    }
    assert(ts_seek_position(500, 1000, 188000, 188) == 94000);
    assert(ts_seek_position(500, 1000, 192000, 192) == 96000);
    assert(ts_seek_position(500, 1000, 204000, 204) == 102000);
    assert(ts_seek_position(1000, 1000, 188000, 188) == 187812);
    assert(ts_seek_position(500, AV_NOPTS_VALUE, 188000, 188) == -1);
    assert(ts_seek_position(-1, 1000, 188000, 188) == -1);
    assert(ts_seek_position(500, 1000, 188000, 189) == -1);
    {
        AVFormatContext av = {0};
        av.duration = AV_NOPTS_VALUE;
        assert(!context_duration_ms(&av));
        av.duration = -1000;
        assert(!context_duration_ms(&av));
        av.duration = 1234567;
        assert(context_duration_ms(&av) == 1234);
    }
    io.pFile = tmpfile();
    assert(io.pFile);
    assert(fwrite("abc", 1, 3, io.pFile) == 3);
    rewind(io.pFile);
    assert(SAM_SeekFunc(&io, 0, AVSEEK_SIZE) == 3);
    assert(ftello(io.pFile) == 0);
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == 3);
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == AVERROR_EOF);
    assert(SAM_SeekFunc(&io, 1, SEEK_SET) == 1);
    assert(SAM_ReadFunc(&io, bytes, 1) == 1 && bytes[0] == 'b');
    progressive_playback = 1;
    assert(!fseeko(io.pFile, 0, SEEK_END));
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == 0);
    progressive_playback = 0;
    fclose(io.pFile);
    {
        AVFormatContext *av = avformat_alloc_context();
        AVStream *low, *high;
        AVProgram *a, *b;
        assert(av);
        low = avformat_new_stream(av, NULL);
        high = avformat_new_stream(av, NULL);
        assert(low && high);
        low->codecpar->codec_type = high->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        low->codecpar->codec_id = AV_CODEC_ID_H264;
        high->codecpar->codec_id = AV_CODEC_ID_HEVC;
        high->codecpar->width = 3840;
        high->codecpar->height = 2160;
        a = av_new_program(av, 1);
        b = av_new_program(av, 2);
        assert(a && b);
        av_program_add_stream_index(av, 1, 0);
        av_program_add_stream_index(av, 2, 1);
        av_dict_set(&a->metadata, "variant_bitrate", "100", 0);
        av_dict_set(&b->metadata, "variant_bitrate", "900", 0);
        av_dict_set(&low->metadata, "variant_bitrate", "100", 0);
        av_dict_set(&high->metadata, "variant_bitrate", "900", 0);
        adaptive_quality_set(0, 0);
        assert(select_program(av) == b && select_dash_video(av) == 1);
        adaptive_quality_set(1, 0);
        assert(select_program(av) == a && select_dash_video(av) == 0);
        h264_resolution_limit_set(1920, 1080);
        adaptive_quality_set(2, 0);
        assert(select_dash_video(av) == 1); /* HEVC is not capped. */
        high->codecpar->codec_id = AV_CODEC_ID_H264;
        assert(select_program(av) == a && select_dash_video(av) == 0);
        low->codecpar->width = 3840;
        low->codecpar->height = 2160;
        assert(!select_program(av) && select_dash_video(av) == -1);
        sel_program_id_set(2);
        assert(select_program(av) == b); /* Explicit selection overrides limits. */
        sel_program_id_set(-1);
        h264_resolution_limit_set(0, 0);
        avformat_free_context(av);
    }
    {
        /* DASH audio without a requested track: the best representation of
         * the language of the first one, not the first one. */
        static const struct {
            const char *lang, *bandwidth;
            int channels, software;
        } reps[] = {
            {"en", "128000", 2, 0},  /* 0: first, stereo AAC */
            {"EN", "384000", 6, 0},  /* 1: 5.1 E-AC-3 to the decoder */
            {"de", "640000", 6, 0},  /* 2: other language */
            {"en", "448000", 6, 1},  /* 3: 5.1 decoded here (A_IPCM) */
            {"en", "96000", 2, 0},   /* 4: stereo, lowest bandwidth */
            {"en", "1", 2, 0},       /* 5: pending (an old track) */
        };
        AVFormatContext *av = avformat_alloc_context();
        AVInputFormat dash = {.name = "dash"}, hls = {.name = "hls"}, mp4 = {.name = "mov,mp4,m4a,3gp,3g2,mj2"};
        Track_t tracks[6];
        int i;
        assert(av);
        memset(tracks, 0, sizeof(tracks));
        for (i = 0; i < 6; i++) {
            AVStream *stream = avformat_new_stream(av, NULL);
            assert(stream);
            stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            av_dict_set(&stream->metadata, "variant_bitrate", reps[i].bandwidth, 0);
            tracks[i].Name = (char *)reps[i].lang;
            tracks[i].Id = i + 1;
            tracks[i].stream = stream;
            tracks[i].channels = reps[i].channels;
            tracks[i].inject_as_pcm = reps[i].software;
        }
        tracks[5].pending = 1;
        tracks[5].channels = 8;
        av->iformat = &dash;
        avContextTab[0] = av;
        adaptive_quality_set(0, 0);
        /* of the tracks for the decoder of the box the one with more
         * channels (5.1 AC-3 over stereo AAC), not the 5.1 one decoded here
         * although it has more bandwidth */
        assert(select_dash_audio(tracks, 6, 0) == 1);
        /* a 5.1 track decoded here (PCM 2.0 on HDMI) loses to a stereo one
         * for the decoder of the box */
        tracks[1].inject_as_pcm = 1;
        assert(select_dash_audio(tracks, 6, 0) == 0);
        tracks[1].inject_as_pcm = 0;
        tracks[1].transcode = 1;
        assert(select_dash_audio(tracks, 6, 0) == 0);
        tracks[1].transcode = 0;
        /* both decoded here: channels again */
        tracks[0].inject_as_pcm = tracks[1].inject_as_pcm = tracks[4].inject_as_pcm = 1;
        assert(select_dash_audio(tracks, 6, 0) == 3);
        tracks[0].inject_as_pcm = tracks[1].inject_as_pcm = tracks[4].inject_as_pcm = 0;
        tracks[3].inject_as_pcm = 0;
        assert(select_dash_audio(tracks, 6, 0) == 3);
        tracks[3].inject_as_pcm = 1;
        tracks[3].transcode = 1;
        assert(select_dash_audio(tracks, 6, 0) == 1);
        tracks[3].transcode = 0;
        /* channels unknown for one of two: bandwidth */
        tracks[3].inject_as_pcm = 0;
        tracks[3].channels = 0;
        assert(select_dash_audio(tracks, 6, 0) == 3);
        tracks[3].inject_as_pcm = 1;
        tracks[3].channels = 6;
        /* -Q 1: fewest channels, lowest bandwidth; -Q 2 as Auto */
        adaptive_quality_set(1, 0);
        assert(select_dash_audio(tracks, 6, 0) == 4);
        adaptive_quality_set(2, 0);
        assert(select_dash_audio(tracks, 6, 0) == 1);
        adaptive_quality_set(0, 0);
        /* disposition first: default, not impaired (as av_find_best_stream) */
        av->streams[0]->disposition = AV_DISPOSITION_DEFAULT;
        assert(select_dash_audio(tracks, 6, 0) == 0);
        av->streams[0]->disposition = 0;
        av->streams[1]->disposition = AV_DISPOSITION_HEARING_IMPAIRED;
        assert(select_dash_audio(tracks, 6, 0) == 0); /* 3 is decoded here */
        tracks[3].inject_as_pcm = 0;
        assert(select_dash_audio(tracks, 6, 0) == 3);
        tracks[3].inject_as_pcm = 1;
        av->streams[1]->disposition = 0;
        /* the language of the first track */
        assert(select_dash_audio(tracks, 6, 2) == 2);
        /* an explicit choice wins: -P, audio_rep_index (-1, &1=) */
        sel_program_id_set(1);
        assert(select_dash_audio(tracks, 6, 0) == 0);
        sel_program_id_set(-1);
        ffmpeg_av_dict_set("audio_rep_index", "0", 0);
        assert(select_dash_audio(tracks, 6, 0) == 0);
        ffmpeg_av_dict_set("audio_rep_index", "-1", 0);
        assert(select_dash_audio(tracks, 6, 0) == 1);
        ffmpeg_av_dict_set("audio_rep_index", NULL, 0);
        /* other inputs keep the first track (HLS has its DEFAULT=YES rule) */
        av->iformat = &hls;
        assert(select_dash_audio(tracks, 6, 0) == 0);
        av->iformat = &mp4;
        assert(select_dash_audio(tracks, 6, 0) == 0);
        av->iformat = &dash;
        assert(select_dash_audio(tracks, 6, -1) == -1);
        tracks[0].AVIdx = 1;
        assert(select_dash_audio(tracks, 6, 0) == 0); /* no second input */
        avContextTab[0] = NULL;
        av->iformat = NULL;
        avformat_free_context(av);
    }
    {
        PlaybackHandler_t playback = {.Command = seek_playback, .isPlaying = 1};
        Manager_t track_manager = {.Command = seek_manager};
        ManagerHandler_t managers = {.audio = &track_manager, .video = &track_manager};
        Context_t context = {.playback = &playback, .manager = &managers};
        assert(!container_ffmpeg_seek(&context, 0, 0));
        assert(g_do_seek_target_seconds && g_zero_seek && g_seek_target_seconds == 22375000);
        assert(!container_ffmpeg_seek(&context, -10, 0));
        assert(!g_zero_seek && g_seek_target_seconds == 12375000);
        assert(!container_ffmpeg_seek(&context, 3, 1));
        assert(!g_zero_seek && g_seek_target_seconds == 3000000);
        g_do_seek_target_seconds = false;
        assert(container_ffmpeg_seek(&context, INT64_MAX, 1) < 0);
        assert(!g_do_seek_target_seconds);
    }
    {
        AVFormatContext av = {.duration = 5668000000, .start_time = 0};
        const char *old = getenv("EP3_LIVE_SYNC");
        char *saved = old ? strdup(old) : NULL;
        avContextTab[0] = &av;
        for (int enabled = 0; enabled < 2; enabled++) {
            setenv("EP3_LIVE_SYNC", enabled ? "1" : "0", 1);
            ls_state = 0;
            ls_on_video(NULL, 0);
            assert(ls_state == 2);
        }
        avContextTab[0] = NULL;
        if (saved)
            setenv("EP3_LIVE_SYNC", saved, 1);
        else
            unsetenv("EP3_LIVE_SYNC");
        free(saved);
    }
    {
        AVFormatContext av = {.duration = AV_NOPTS_VALUE};
        AVStream streams[2] = {{0}, {0}};
        PlaybackHandler_t playback = {.Command = seek_playback, .isPlaying = 1};
        Manager_t audio = {.Command = live_audio_manager};
        ManagerHandler_t managers = {.audio = &audio};
        Context_t context = {.playback = &playback, .manager = &managers};
        int32_t selected = 1;
        avContextTab[0] = &av;
        for (int i = 0; i < 2; i++) {
            live_tracks[i].Id = i;
            live_tracks[i].stream = &streams[i];
        }
        g_do_seek_target_seconds = false;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(!g_do_seek_target_seconds);
        assert(streams[0].discard == AVDISCARD_ALL);
        assert(streams[1].discard == AVDISCARD_DEFAULT);
        /* Static/custom-I/O DASH seeks to the playback position; the seek
         * resets the buffers, no reset of its own. */
        playback.isDash = 1;
        playback.Command = switch_playback;
        av.duration = 60 * AV_TIME_BASE;
        use_custom_io[0] = 1;
        selected = 0;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(switch_seeks == 1 && !dashAudioResetPending);
        assert(streams[0].discard == AVDISCARD_DEFAULT);
        assert(streams[1].discard == AVDISCARD_ALL);
        use_custom_io[0] = 0;
        selected = 1;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(switch_seeks == 2 && !dashAudioResetPending);
        /* a refused seek: the switch at the read position with the reset */
        switch_seek_error = -1;
        selected = 0;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(switch_seeks == 3 && dashAudioResetPending);
        assert(streams[0].discard == AVDISCARD_DEFAULT);
        switch_seek_error = 0;
        dashAudioResetPending = 0;
        /* live DASH: no seek, the reset */
        av.duration = AV_NOPTS_VALUE;
        selected = 1;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(!g_do_seek_target_seconds && dashAudioResetPending);
        dashAudioResetPending = 0;
        av.duration = 0;
        selected = 0;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(!g_do_seek_target_seconds && dashAudioResetPending);
        dashAudioResetPending = 0;
        use_custom_io[0] = 0;
        avContextTab[0] = NULL;
    }
    {
        /* Progressive playback: the download appends after the reader caught up. */
        CustomIOCtx_t growing = {0};
        growing.pFile = tmpfile();
        assert(growing.pFile);
        assert(fwrite("abc", 1, 3, growing.pFile) == 3);
        rewind(growing.pFile);
        progressive_playback = 1;
        assert(SAM_ReadFunc(&growing, bytes, sizeof(bytes)) == 3);
        assert(SAM_ReadFunc(&growing, bytes, sizeof(bytes)) == 0);
        assert(pwrite(fileno(growing.pFile), "de", 2, 3) == 2);
        assert(SAM_ReadFunc(&growing, bytes, sizeof(bytes)) == 2 && !memcmp(bytes, "de", 2));
        assert(SAM_ReadFunc(&growing, bytes, sizeof(bytes)) == 0);
        progressive_playback = 0;
        fclose(growing.pFile);
    }
    {
        /* Separate moov atom file: offsets from 6 on are read from it. */
        CustomIOCtx_t split = {.iFileSize = 9, .iMoovAtomOffset = 6};
        split.pFile = tmpfile();
        split.pMoovFile = tmpfile();
        assert(split.pFile && split.pMoovFile);
        assert(fwrite("abcdef", 1, 6, split.pFile) == 6);
        assert(fwrite("xyz", 1, 3, split.pMoovFile) == 3);
        rewind(split.pFile);
        rewind(split.pMoovFile);
        assert(SAM_SeekFunc(&split, 0, AVSEEK_SIZE) == 9);
        assert(SAM_SeekFunc(&split, 2, SEEK_SET) == 2);
        assert(SAM_SeekFunc(&split, 0, SEEK_CUR) == 2);
        assert(SAM_SeekFunc(&split, 3, SEEK_CUR) == 5);
        assert(SAM_ReadFunc(&split, bytes, sizeof(bytes)) == 4 && !memcmp(bytes, "fxyz", 4));
        assert(SAM_SeekFunc(&split, -2, SEEK_CUR) == 7);
        assert(SAM_ReadFunc(&split, bytes, 1) == 1 && bytes[0] == 'y');
        fclose(split.pFile);
        fclose(split.pMoovFile);
    }
    {
        /* MPEG-TS programs have no bandwidth: a probed video beats an unprobed one. */
        AVFormatContext *av = avformat_alloc_context();
        AVStream *unprobed, *probed;
        AVProgram *a, *b;
        assert(av);
        unprobed = avformat_new_stream(av, NULL);
        probed = avformat_new_stream(av, NULL);
        assert(unprobed && probed);
        unprobed->codecpar->codec_type = probed->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        unprobed->codecpar->codec_id = probed->codecpar->codec_id = AV_CODEC_ID_MPEG2VIDEO;
        probed->codecpar->width = 720;
        probed->codecpar->height = 576;
        a = av_new_program(av, 1);
        b = av_new_program(av, 2);
        assert(a && b);
        av_program_add_stream_index(av, 1, 0);
        av_program_add_stream_index(av, 2, 1);
        adaptive_quality_set(0, 0);
        assert(select_program(av) == b);
        adaptive_quality_set(1, 0);
        assert(select_program(av) == b);
        unprobed->codecpar->width = 720;
        unprobed->codecpar->height = 576;
        assert(select_program(av) == a); /* Both probed: the first one, as before. */
        unprobed->codecpar->width = probed->codecpar->width = 0;
        assert(select_program(av) == a); /* Nothing probed: the first one, as before. */
        adaptive_quality_set(0, 0);
        avformat_free_context(av);
    }
    {
        /* The selected track takes its duration from its own stream, not the last one. */
        AVFormatContext *av = avformat_alloc_context();
        Manager_t track_manager = {.Command = seek_manager};
        ManagerHandler_t managers = {.audio = &track_manager};
        Context_t context = {.manager = &managers};
        AVStream *played, *other;
        assert(av);
        played = avformat_new_stream(av, NULL);
        other = avformat_new_stream(av, NULL);
        assert(played && other);
        played->codecpar->codec_type = other->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        played->time_base = other->time_base = (AVRational){1, 1000};
        played->duration = 10000;
        other->duration = 4000;
        seek_track.stream = played;
        avContextTab[0] = av;
        assert(!container_ffmpeg_update_duration(&context));
        assert(seek_track.duration == 10000);
        avContextTab[0] = NULL;
        seek_track.stream = NULL;
        avformat_free_context(av);
    }
    {
        /* WMA, WMA Pro, Vorbis and Opus: the hardware writer only where the
         * driver has a decoder for them; on Vu+ they are decoded in software. */
        static const struct {
            stb_type_t type;
            const char *wma, *wma_pro, *vorbis, *opus;
        } boxes[] = {
            {STB_VUPLUS, "A_IPCM", "A_IPCM", "A_IPCM", "A_IPCM"},
            {STB_DREAMBOX, "A_WMA", "A_WMA/PRO", "A_VORBIS", "A_OPUS"},
            {STB_HISILICON, "A_WMA", "A_WMA/PRO", "A_VORBIS", "A_OPUS"},
            {STB_OTHER, "A_WMA", "A_WMA/PRO", "A_VORBIS", "A_OPUS"},
        };
        int32_t version = 0;
        unsigned i;
        vorbis_software_decode = opus_software_decode = 0;
        for (i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
            stb_type = boxes[i].type;
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_WMAV2, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), boxes[i].wma));
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_WMAPRO, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), boxes[i].wma_pro));
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_VORBIS, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), boxes[i].vorbis));
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_OPUS, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), boxes[i].opus));
            /* The options for software decoding still apply everywhere. */
            wma_software_decode = 1;
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_WMAV2, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), "A_IPCM"));
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_WMAPRO, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), "A_IPCM"));
            wma_software_decode = 0;
        }
        vorbis_software_decode = opus_software_decode = 1;
        assert(!strcmp(Codec2Encoding(AV_CODEC_ID_VORBIS, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), "A_IPCM"));
        assert(!strcmp(Codec2Encoding(AV_CODEC_ID_OPUS, AVMEDIA_TYPE_AUDIO, NULL, 0, 0, &version), "A_IPCM"));
        stb_type = STB_OTHER;
    }
    {
        /* AAC: channel configurations 11-13 (6.1, 7.1) do not fit into the
         * 3 bits of the ADTS header the AAC writer sends: decoded in
         * software. 0 goes to LATM with its PCE, except on HiSilicon. */
        static const stb_type_t types[] = {STB_OTHER, STB_VUPLUS, STB_HISILICON};
        uint8_t asc[2] = {0x12, 0x60}; /* only its size counts, see the stub */
        int32_t version = 0;
        unsigned i;
        aac_config_set = 1;
        aac_config.object_type = 2;
        aac_config.sampling_index = 4;
        for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            stb_type = types[i];
            aac_config.chan_config = 2;
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_AAC, AVMEDIA_TYPE_AUDIO, asc, 2, 0, &version), "A_AAC"));
            aac_config.chan_config = 7;
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_AAC, AVMEDIA_TYPE_AUDIO, asc, 2, 0, &version), "A_AAC"));
            for (aac_config.chan_config = 11; aac_config.chan_config <= 13; aac_config.chan_config++)
                assert(!strcmp(Codec2Encoding(AV_CODEC_ID_AAC, AVMEDIA_TYPE_AUDIO, asc, 2, 0, &version), "A_IPCM"));
            aac_config.chan_config = 0;
            assert(!strcmp(Codec2Encoding(AV_CODEC_ID_AAC, AVMEDIA_TYPE_AUDIO, asc, 2, 0, &version),
                           types[i] == STB_HISILICON ? "A_AAC" : "A_AAC_LATM"));
        }
        aac_config_set = 0;
        stb_type = STB_OTHER;
    }
    {
        /* A stream FFmpeg may still probe (codec_id NONE, e.g. a private
         * MPEG-TS stream_type) stays open; once a packet of it comes with
         * codec_id NONE the probing found nothing and it is discarded. */
        AVFormatContext *av = avformat_alloc_context();
        AVStream *data, *found, *audio, *gone;
        assert(av);
        data = avformat_new_stream(av, NULL);
        found = avformat_new_stream(av, NULL);
        audio = avformat_new_stream(av, NULL);
        gone = avformat_new_stream(av, NULL);
        assert(data && found && audio && gone);
        data->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        data->codecpar->codec_id = AV_CODEC_ID_NONE;
        found->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        found->codecpar->codec_id = AV_CODEC_ID_TIMED_ID3;
        audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio->codecpar->codec_id = AV_CODEC_ID_NONE;
        gone->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
        gone->codecpar->codec_id = AV_CODEC_ID_NONE;
        gone->discard = AVDISCARD_ALL;
        assert(unprobed_stream_ended(data) && data->discard == AVDISCARD_ALL);
        assert(!unprobed_stream_ended(data)); /* once */
        assert(!unprobed_stream_ended(found) && found->discard != AVDISCARD_ALL);
        assert(!unprobed_stream_ended(audio) && audio->discard != AVDISCARD_ALL);
        assert(!unprobed_stream_ended(gone));
        avformat_free_context(av);
    }
    {
        /* Dreambox: AC-3 and E-AC-3 that are not at 48 kHz go to AC3 at
         * 48 kHz, unless EP3_AC3_48K=0; other boxes, 48 kHz, other codecs
         * and AC-3 decoded in software are left as they are. */
        AVFormatContext *av = avformat_alloc_context();
        AVStream *s;
        const int ac3 = audio_transcode_caps() & AUDIO_XCODE_AC3;
        assert(av);
        s = avformat_new_stream(av, NULL);
        assert(s);
        s->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        unsetenv("EP3_AC3_48K");

        stb_type = STB_DREAMBOX;
        s->codecpar->codec_id = AV_CODEC_ID_AC3;
        s->codecpar->sample_rate = 44100;
        assert(AudioTranscoderAc3To48k(s, "A_AC3"));
        assert(AudioTranscoderTarget(s, "A_AC3") == ac3);
        assert(!AudioTranscoderAc3To48k(s, "A_IPCM"));
        assert(!AudioTranscoderTarget(s, "A_IPCM"));
        s->codecpar->codec_id = AV_CODEC_ID_EAC3;
        s->codecpar->sample_rate = 32000;
        assert(AudioTranscoderAc3To48k(s, "A_EAC3"));
        assert(AudioTranscoderTarget(s, "A_EAC3") == ac3);
        s->codecpar->sample_rate = 48000;
        assert(!AudioTranscoderAc3To48k(s, "A_EAC3"));
        assert(!AudioTranscoderTarget(s, "A_EAC3"));
        s->codecpar->sample_rate = 0;
        assert(!AudioTranscoderAc3To48k(s, "A_EAC3"));
        s->codecpar->codec_id = AV_CODEC_ID_AAC;
        s->codecpar->sample_rate = 44100;
        assert(!AudioTranscoderAc3To48k(s, "A_AAC"));

        s->codecpar->codec_id = AV_CODEC_ID_AC3;
        setenv("EP3_AC3_48K", "0", 1);
        assert(!AudioTranscoderAc3To48k(s, "A_AC3"));
        assert(!AudioTranscoderTarget(s, "A_AC3"));
        setenv("EP3_AC3_48K", "1", 1);
        assert(AudioTranscoderAc3To48k(s, "A_AC3"));
        unsetenv("EP3_AC3_48K");

        stb_type = STB_OTHER;
        assert(!AudioTranscoderAc3To48k(s, "A_AC3"));
        assert(!AudioTranscoderTarget(s, "A_AC3"));
        stb_type = STB_VUPLUS;
        assert(!AudioTranscoderAc3To48k(s, "A_AC3"));
        stb_type = STB_OTHER;
        avformat_free_context(av);
    }
    return 0;
}
