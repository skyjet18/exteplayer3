/*
 * Closed captions as a text subtitle track: CEA-608 from the cc_data() of
 * the video stream (cc_data.c) and from EIA-608 subtitle streams (MOV c608,
 * SCC), decoded by the ccaption decoder of libavcodec (cc_dec).
 *
 * The decoder runs in its real time mode: it reports every change of the
 * caption screen. The end of a screen is known only when the next change is
 * decoded, the client wants it with the cue. So a screen is written in pieces
 * of CC_PIECE that end at the time read so far; the receiver shows a piece
 * when its decoder buffer holds more than that, which it does while playing.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "cc_data.c"

/* the subtitle track of the captions in the video stream; stream ids are
 * smaller (MPEG-TS PIDs end at 0x1FFF) */
#define CC_EMBEDDED_TRACK_ID 0x10000
#define CC_ENCODING          "S_TEXT/EIA608"
/* 90 kHz */
#define CC_PIECE             (500 * 90)
#define CC_MAX_GAP           (10 * 90000)
/* 15 rows of 32 characters, a special character takes up to 3 bytes in UTF-8 */
#define CC_TEXT_MAX          2048

typedef struct
{
    int32_t         trackId;
    int             failed;     /* libavcodec has no ccaption decoder */
    AVCodecContext *decoder;
    AVPacket       *packet;
    char            text[CC_TEXT_MAX];  /* the screen shown since start, "" none */
    int64_t         start;      /* of the part not written yet, INVALID_PTS_VALUE none */
} CcScreen;

typedef struct
{
    CcScreen        screen;
    const AVStream *stream;     /* the video stream the data comes from */
    uint32_t        avIdx;
    int             codec;
    int             lengthSize;
    int             seen;       /* a caption has been in it */
    int64_t         lastFed;
    CcReorder       reorder;
} CcEmbedded;

static CcEmbedded ccEmbedded = { .screen = { .trackId = CC_EMBEDDED_TRACK_ID, .start = INVALID_PTS_VALUE } };
static CcScreen   ccStream = { .trackId = -1, .start = INVALID_PTS_VALUE };

static int cc_decoder_available(void)
{
    return avcodec_find_decoder(AV_CODEC_ID_EIA_608) != NULL;
}

static int cc_screen_open(CcScreen *screen)
{
    AVDictionary *options = NULL;
    const AVCodec *codec;

    if (screen->decoder)
        return 0;
    if (screen->failed)
        return -1;
    codec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    screen->decoder = codec ? avcodec_alloc_context3(codec) : NULL;
    screen->packet = av_packet_alloc();
    if (!screen->decoder || !screen->packet)
    {
        ffmpeg_err("no closed caption decoder\n");
        goto fail;
    }
    screen->decoder->pkt_timebase = (AVRational){1, 90000};
    av_dict_set(&options, "real_time", "1", 0);
    /* field 1: CC1 */
    av_dict_set(&options, "data_field", "0", 0);
    if (avcodec_open2(screen->decoder, codec, &options) < 0)
    {
        ffmpeg_err("opening the closed caption decoder failed\n");
        av_dict_free(&options);
        goto fail;
    }
    av_dict_free(&options);
    screen->text[0] = '\0';
    screen->start = INVALID_PTS_VALUE;
    return 0;
fail:
    avcodec_free_context(&screen->decoder);
    av_packet_free(&screen->packet);
    screen->failed = 1;
    return -1;
}

static void cc_screen_close(CcScreen *screen)
{
    avcodec_free_context(&screen->decoder);
    av_packet_free(&screen->packet);
    screen->failed = 0;
    screen->text[0] = '\0';
    screen->start = INVALID_PTS_VALUE;
}

/* A new decoder, not avcodec_flush_buffers(): the flush of cc_dec keeps the
 * time of the last change, and after a seek back its real time mode holds
 * every roll-up and paint-on change until the time read before is reached. */
static void cc_screen_reset(CcScreen *screen)
{
    if (screen->decoder)
    {
        cc_screen_close(screen);
        cc_screen_open(screen);
    }
    screen->text[0] = '\0';
    screen->start = INVALID_PTS_VALUE;
}

/* The text of an ASS event of the decoder: no tags, \N a line feed, \h a space. */
static void cc_ass_text(const char *ass, char *out, size_t cap)
{
    size_t n = 0;
    int commas = 0;

    /* ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text */
    while (*ass && commas < 8)
        if (*ass++ == ',')
            commas++;
    while (*ass && n + 1 < cap)
    {
        if (*ass == '{')
        {
            const char *close = strchr(ass, '}');

            if (!close)
                break;
            ass = close + 1;
        }
        else if (ass[0] == '\\' && (ass[1] == 'N' || ass[1] == 'n'))
        {
            out[n++] = '\n';
            ass += 2;
        }
        else if (ass[0] == '\\' && ass[1] == 'h')
        {
            out[n++] = ' ';
            ass += 2;
        }
        else
        {
            out[n++] = *ass++;
        }
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
        n--;
    out[n] = '\0';
}

/* Writes what the screen has shown from its start to end, when its track is selected. */
static void cc_screen_write(Context_t *context, CcScreen *screen, int64_t end)
{
    static char text[CC_TEXT_MAX];
    int32_t current = -1;
    SubtitleOut_t out;

    if (!screen->text[0] || screen->start == INVALID_PTS_VALUE ||
        end <= screen->start || end - screen->start > CC_MAX_GAP)
        return;
    context->manager->subtitle->Command(context, MANAGER_GET, &current);
    if (current != screen->trackId)
        return;
    strcpy(text, screen->text);
    memset(&out, 0, sizeof(out));
    out.trackId = screen->trackId;
    out.data = (uint8_t *)text;
    out.len = strlen(text);
    out.pts = screen->start;
    /* pieces of one screen meet without a gap */
    out.durationMS = end / 90 - screen->start / 90;
    if (Write(context->output->subtitle->Write, context, &out, screen->start) < 0)
        ffmpeg_err("writing closed captions failed\n");
}

/* Decodes the triplets of one picture shown at pts. */
static void cc_screen_decode(Context_t *context, CcScreen *screen, int64_t pts,
                             uint8_t *data, int size)
{
    static char text[CC_TEXT_MAX];
    AVSubtitle sub;
    int got = 0;

    if (!screen->decoder || size <= 0)
        return;
    screen->packet->data = data;
    screen->packet->size = size;
    screen->packet->pts = pts;
    screen->packet->dts = pts;
    memset(&sub, 0, sizeof(sub));
    if (avcodec_decode_subtitle2(screen->decoder, &sub, &got, screen->packet) < 0 || !got)
        return;
    text[0] = '\0';
    if (sub.num_rects > 0 && sub.rects[sub.num_rects - 1]->ass)
        cc_ass_text(sub.rects[sub.num_rects - 1]->ass, text, sizeof(text));
    avsubtitle_free(&sub);
    if (!strcmp(text, screen->text))
        return;
    cc_screen_write(context, screen, pts);
    /* the old screen may have been written a little beyond pts already: the
     * new one starts after it, cues do not overlap */
    if (!screen->text[0] || screen->start == INVALID_PTS_VALUE ||
        pts >= screen->start || screen->start - pts > CC_MAX_GAP)
        screen->start = pts;
    strcpy(screen->text, text);
}

/* Everything up to now has been decoded: write the screen up to there in
 * pieces of CC_PIECE. */
static void cc_screen_advance(Context_t *context, CcScreen *screen, int64_t now)
{
    if (screen->start == INVALID_PTS_VALUE || now == INVALID_PTS_VALUE)
        return;
    if (now - screen->start > CC_MAX_GAP || screen->start - now > CC_MAX_GAP)
    {
        /* a jump of the time */
        screen->start = now;
        return;
    }
    if (now <= screen->start)
        return;
    if (now - screen->start >= CC_PIECE && screen->text[0])
    {
        cc_screen_write(context, screen, now);
        screen->start = now;
    }
}

static void cc_embedded_feed(Context_t *context, CcEmbedded *cc, const CcEntry *entry)
{
    uint8_t data[CC_DATA_MAX];

    memcpy(data, entry->cc.data, entry->cc.size);
    cc_screen_decode(context, &cc->screen, entry->pts, data, (int)entry->cc.size);
    cc->lastFed = entry->pts;
}

static void cc_reset(void)
{
    cc_reorder_clear(&ccEmbedded.reorder);
    ccEmbedded.lastFed = INVALID_PTS_VALUE;
    cc_screen_reset(&ccEmbedded.screen);
    cc_screen_reset(&ccStream);
}

static void cc_close(void)
{
    cc_reorder_clear(&ccEmbedded.reorder);
    cc_screen_close(&ccEmbedded.screen);
    cc_screen_close(&ccStream);
    ccEmbedded.stream = NULL;
    ccEmbedded.seen = 0;
}

/* A packet of the video stream that is played, pts and dts on the 90 kHz
 * clock of the output. */
static void cc_video_packet(Context_t *context, uint32_t avIdx, AVStream *stream,
                            const AVPacket *packet, int64_t pts, int64_t dts)
{
    CcEmbedded *cc = &ccEmbedded;
    CcData data;
    CcEntry entry;
    int64_t decoded;
    int depth;

    /* an EIA-608 stream can have packets only where something changes: the
     * video tells how far the time has gone */
    if (ccStream.decoder)
        cc_screen_advance(context, &ccStream, dts != INVALID_PTS_VALUE ? dts : pts);

    if (cc->stream != stream)
    {
        cc_reorder_clear(&cc->reorder);
        cc_screen_reset(&cc->screen);
        cc->stream = stream;
        cc->avIdx = avIdx;
        cc->lastFed = INVALID_PTS_VALUE;
        switch (get_codecpar(stream)->codec_id)
        {
        case AV_CODEC_ID_MPEG2VIDEO:
            cc->codec = CC_CODEC_MPEG2;
            break;
        case AV_CODEC_ID_H264:
            cc->codec = CC_CODEC_H264;
            break;
        case AV_CODEC_ID_HEVC:
            cc->codec = CC_CODEC_HEVC;
            break;
        default:
            cc->codec = CC_CODEC_NONE;
            break;
        }
        cc->lengthSize = get_codecpar(stream)->extradata_size > 0 ?
            cc_length_size(cc->codec, get_codecpar(stream)->extradata,
                           (size_t)get_codecpar(stream)->extradata_size) : 0;
    }
    if (cc->codec == CC_CODEC_NONE || cc->screen.failed || pts == INVALID_PTS_VALUE)
        return;

    cc_data_extract(cc->codec, cc->lengthSize, packet->data,
                    packet->size > 0 ? (size_t)packet->size : 0, &data);
    if (!cc->seen)
    {
        /* streams can carry cc_data() with nothing but padding */
        if (!data.text || cc_screen_open(&cc->screen) < 0)
            return;
        cc->seen = 1;
        __atomic_add_fetch(&ccTrackGen, 1, __ATOMIC_RELAXED);
        ffmpeg_printf(10, "closed captions in the video stream\n");
    }

    /* a jump of the time: what waits belongs to the time before it */
    if (cc->reorder.count > 0 &&
        llabs(pts - cc->reorder.entry[cc->reorder.count - 1].pts) > CC_MAX_GAP)
    {
        while (cc_reorder_pop(&cc->reorder, &entry))
            cc_embedded_feed(context, cc, &entry);
    }
    if (data.size > 0)
    {
        if (cc->reorder.count == CC_REORDER_MAX && cc_reorder_pop(&cc->reorder, &entry))
            cc_embedded_feed(context, cc, &entry);
        cc_reorder_insert(&cc->reorder, pts, &data);
    }
    /* without a decoding time only the depth of the queue tells */
    decoded = dts != INVALID_PTS_VALUE ? dts : INT64_MIN;
    depth = dts != INVALID_PTS_VALUE ? CC_REORDER_MAX - 1 : 4;
    while (cc_reorder_due(&cc->reorder, decoded, depth) && cc_reorder_pop(&cc->reorder, &entry))
        cc_embedded_feed(context, cc, &entry);
    cc_screen_advance(context, &cc->screen, dts != INVALID_PTS_VALUE ? dts : cc->lastFed);
}

/* A packet of a selected EIA-608 subtitle stream: triplets in presentation order. */
static void cc_stream_packet(Context_t *context, int32_t trackId, const AVPacket *packet, int64_t pts)
{
    if (ccStream.trackId != trackId)
    {
        cc_screen_reset(&ccStream);
        ccStream.trackId = trackId;
    }
    if (pts == INVALID_PTS_VALUE || cc_screen_open(&ccStream) < 0)
        return;
    cc_screen_decode(context, &ccStream, pts, packet->data, packet->size);
    cc_screen_advance(context, &ccStream, pts);
}

/* The track of the captions in the video stream, once there has been one. */
static void cc_add_track(Context_t *context)
{
    AVDictionaryEntry *lang = NULL;
    Track_t track;

    if (!ccEmbedded.seen || !ccEmbedded.stream || !context->manager->subtitle)
        return;
    memset(&track, 0, sizeof(track));
    lang = av_dict_get(ccEmbedded.stream->metadata, "language", NULL, 0);
    track.Name = lang ? lang->value : "und";
    track.Title = "CC1";
    track.Encoding = CC_ENCODING;
    track.Id = CC_EMBEDDED_TRACK_ID;
    track.AVIdx = ccEmbedded.avIdx;
    if (context->manager->subtitle->Command(context, MANAGER_ADD, &track) < 0)
        ffmpeg_err("failed to add the closed caption track\n");
}
