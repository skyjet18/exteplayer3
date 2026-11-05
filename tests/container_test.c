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

int main(void)
{
    CustomIOCtx_t io = {0};
    uint8_t bytes[8];
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
    return 0;
}
