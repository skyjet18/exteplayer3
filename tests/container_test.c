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

int main(void)
{
    CustomIOCtx_t io = {0};
    uint8_t bytes[8];
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
        /* Static/custom-I/O DASH requests a reset without seeking. */
        playback.isDash = 1;
        av.duration = 60 * AV_TIME_BASE;
        use_custom_io[0] = 1;
        selected = 0;
        assert(!container_ffmpeg_switch_audio(&context, &selected));
        assert(!g_do_seek_target_seconds);
        assert(streams[0].discard == AVDISCARD_DEFAULT);
        assert(streams[1].discard == AVDISCARD_ALL);
        assert(dashAudioResetPending);
        dashAudioResetPending = 0;
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
    return 0;
}
