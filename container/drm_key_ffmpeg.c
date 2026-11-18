/*
 * CENC content keys per KID for the mov, DASH and HLS demuxers.
 *
 * --key <kid>:<key> preloads keys: they are passed to FFmpeg as
 * decryption_keys (mov), cenc_decryption_keys (DASH) and
 * seg_format_options=decryption_keys=... (HLS), which every patched FFmpeg
 * has had for a long time.
 *
 * If FFmpeg can request the key of a KID it meets while playing (option
 * decryption_key_url of the mov demuxer), the player sets that option to an
 * URL of its own (DRM_KEY_URL_PREFIX) and catches it in io_open: a known KID
 * is answered at once (HLS gets preloaded keys this way as well). With
 * --key-request the front end is asked for the others, it gets
 *   {"DRM_KEY_REQ":{"kid":"<32 hex>","type":"video","pssh":[{"system":"<uuid>","data":"<base64 box>"}]}}
 * and answers on stdin with
 *   dk <kid>:<key> [<kid>:<key> ...]    (32 hex digits each)
 *   dk <kid>:-                          (no key for that KID)
 * The demuxer waits for the answer (--key-timeout, 10 s); no answer is no
 * key. Without --key-request a KID that is not known has no key at once,
 * as a front end that does not know the request would never answer. The
 * key goes back to
 * FFmpeg as a data: URL through the original io_open.
 *
 * This file is included by container_ffmpeg.c.
 */

#include <libavutil/base64.h>
#if LIBAVUTIL_VERSION_MAJOR >= 56
#include <libavutil/encryption_info.h>
#endif

#define DRM_KEY_URL_PREFIX "ep3drm:"
#define DRM_KEY_URL DRM_KEY_URL_PREFIX "?kid={kid}&type={type}&pssh={pssh}"
#define DRM_KEY_MAX 64

typedef struct
{
    char kid[33];
    char key[33];
    int8_t state; /* 1: key, -1: no key */
} DrmKey_t;

static DrmKey_t drmKeys[DRM_KEY_MAX];
static int32_t drmKeysCount = 0;
static int32_t drmKeyTimeoutMs = 10000;
static int32_t drmKeyRequest = 0; /* --key-request: ask the front end */
/* a request was not answered: the front end does not answer, the next
 * KIDs are not waited for (no key at once) */
static int32_t drmKeyNoAnswer = 0;
static pthread_mutex_t drmKeyMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drmKeyCond = PTHREAD_COND_INITIALIZER;

#if LIBAVFORMAT_VERSION_MAJOR >= 58
static int (*drmOrigIoOpen)(struct AVFormatContext *s, AVIOContext **pb, const char *url,
                            int flags, AVDictionary **options) = NULL;
#endif

/* 32 hex digits to lower case; 0 if it is not */
static int32_t drm_hex32(char *dst, const char *src, size_t len)
{
    size_t i;
    if (len != 32)
    {
        return 0;
    }
    for (i = 0; i < 32; i++)
    {
        if (!isxdigit((unsigned char)src[i]))
        {
            return 0;
        }
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[32] = '\0';
    return 1;
}

/* with drmKeyMutex held */
static DrmKey_t *drm_key_find(const char *kid)
{
    int32_t i;
    for (i = 0; i < drmKeysCount; i++)
    {
        if (0 == strcmp(drmKeys[i].kid, kid))
        {
            return &drmKeys[i];
        }
    }
    return NULL;
}

/* "<kid>:<key>" or "<kid>:-"; 0 or -1 if it is not one */
static int32_t drm_key_add_pair(const char *pair, size_t len)
{
    const char *colon = memchr(pair, ':', len);
    char kid[33], key[33];
    int8_t state = 1;
    DrmKey_t *entry;

    if (!colon || !drm_hex32(kid, pair, colon - pair))
    {
        return -1;
    }
    if (1 == pair + len - colon - 1 && '-' == colon[1])
    {
        state = -1;
        key[0] = '\0';
    }
    else if (!drm_hex32(key, colon + 1, pair + len - colon - 1))
    {
        return -1;
    }

    pthread_mutex_lock(&drmKeyMutex);
    entry = drm_key_find(kid);
    if (!entry && drmKeysCount < DRM_KEY_MAX)
    {
        entry = &drmKeys[drmKeysCount++];
        strcpy(entry->kid, kid);
    }
    if (entry)
    {
        strcpy(entry->key, key);
        entry->state = state;
    }
    pthread_cond_broadcast(&drmKeyCond);
    pthread_mutex_unlock(&drmKeyMutex);
    return entry ? 0 : -1;
}

/* --key <kid>:<key> */
int32_t ffmpeg_drm_key_add(const char *pair)
{
    return drm_key_add_pair(pair, strlen(pair));
}

/* --key-request */
void ffmpeg_drm_key_request_set(int32_t enable)
{
    drmKeyRequest = enable;
}

/* --key-timeout <s> */
void ffmpeg_drm_key_timeout_set(int32_t seconds)
{
    drmKeyTimeoutMs = seconds > 0 ? seconds * 1000 : 0;
}

/* stdin "dk <kid>:<key> [<kid>:<key> ...]": the number of pairs taken, -1 for
 * a line with one that is not a pair */
int32_t ffmpeg_drm_key_command(const char *line)
{
    const char *p = line + 2;
    int32_t count = 0;

    while (*p)
    {
        size_t len;
        p += strspn(p, " \t\r\n");
        len = strcspn(p, " \t\r\n");
        if (!len)
        {
            break;
        }
        if (drm_key_add_pair(p, len) < 0)
        {
            ffmpeg_err("dk: \"%.*s\" is not <kid>:<key> or <kid>:- (32 hex digits each)\n", (int)len, p);
            return -1;
        }
        count++;
        p += len;
    }
    return count;
}

/* Whether this FFmpeg can request keys: the mov demuxer has decryption_key_url. */
static int32_t drm_key_request_supported(void)
{
#if LIBAVFORMAT_VERSION_MAJOR >= 58
    const AVInputFormat *mov = av_find_input_format("mov");
    return mov && mov->priv_class &&
           NULL != av_opt_find((void *)&mov->priv_class, "decryption_key_url", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
#else
    return 0;
#endif
}

/* "drmkey" of EPLAYER3_EXTENDED: 1 --key, 2 DRM_KEY_REQ / dk */
int32_t ffmpeg_drm_key_caps(void)
{
    return 1 | (drm_key_request_supported() ? 2 : 0);
}

/* The value of name in the query of an ep3drm: URL, percent escapes decoded. */
static char *drm_query_value(const char *query, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p)
    {
        if ('?' == *p || '&' == *p)
        {
            p++;
        }
        if (0 == strncmp(p, name, nlen) && '=' == p[nlen])
        {
            const char *v = p + nlen + 1;
            size_t len = strcspn(v, "&");
            char *out = malloc(len + 1);
            size_t i, o = 0;
            if (!out)
            {
                return NULL;
            }
            for (i = 0; i < len; i++)
            {
                unsigned int c;
                if ('%' == v[i] && i + 2 < len && isxdigit((unsigned char)v[i + 1]) &&
                    isxdigit((unsigned char)v[i + 2]) && 1 == sscanf(v + i + 1, "%2x", &c))
                {
                    out[o++] = (char)c;
                    i += 2;
                }
                else
                {
                    out[o++] = v[i];
                }
            }
            out[o] = '\0';
            return out;
        }
        p = strchr(p, '&');
    }
    return NULL;
}

/* The JSON list of the pssh boxes in base64 (whole boxes, one after the
 * other): [{"system":"<uuid>","data":"<base64 of the box>"},...] */
static char *drm_pssh_json(const char *b64)
{
    size_t b64len = b64 ? strlen(b64) : 0;
    int size;
    uint8_t *data;
    char *json;
    size_t jsonSize, pos = 0;
    int off = 0;

    jsonSize = 3 + b64len * 3 + 128;
    json = malloc(jsonSize);
    if (!json)
    {
        return NULL;
    }
    json[pos++] = '[';
    data = b64len ? malloc(b64len) : NULL;
    size = data ? av_base64_decode(data, b64, (int)b64len) : 0;
    while (size > 0 && off + 32 <= size)
    {
        uint32_t len = ((uint32_t)data[off] << 24) | (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3];
        const uint8_t *id = data + off + 12;
        char *box64;
        int w;
        if (len < 32 || len > (uint32_t)(size - off) || memcmp(data + off + 4, "pssh", 4))
        {
            break;
        }
        box64 = malloc(AV_BASE64_SIZE(len));
        if (!box64 || !av_base64_encode(box64, AV_BASE64_SIZE(len), data + off, len))
        {
            free(box64);
            break;
        }
        w = snprintf(json + pos, jsonSize - pos,
                     "%s{\"system\":\"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\",\"data\":\"%s\"}",
                     pos > 1 ? "," : "",
                     id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
                     id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15], box64);
        free(box64);
        if (w < 0 || (size_t)w >= jsonSize - pos - 1)
        {
            break; /* no room left for the closing bracket; the list ends here */
        }
        pos += w;
        off += len;
    }
    free(data);
    json[pos++] = ']';
    json[pos] = '\0';
    return json;
}

/* now in ms, for the waits */
static uint64_t drm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The key of the KID of an ep3drm: URL: known, or asked from the front end.
 * 0 with the key (hex) in key, AVERROR(ENOENT) without, AVERROR_EXIT. */
static int drm_key_get(AVFormatContext *s, const char *url, char *key)
{
    char kid[33];
    char *kidArg = drm_query_value(url, "kid");
    char *type = NULL, *pssh = NULL, *list = NULL;
    PlaybackHandler_t *playback = (PlaybackHandler_t *)s->interrupt_callback.opaque;
    DrmKey_t *entry;
    int ret = AVERROR(ENOENT);
    int asked = 0;
    uint64_t end;

    if (!kidArg || !drm_hex32(kid, kidArg, strlen(kidArg)))
    {
        ffmpeg_err("key request without a KID: %s\n", url);
        free(kidArg);
        return AVERROR(EINVAL);
    }
    free(kidArg);

    pthread_mutex_lock(&drmKeyMutex);
    entry = drm_key_find(kid);
    if (!entry && drmKeyRequest && !drmKeyNoAnswer)
    {
        /* ask the front end */
        pthread_mutex_unlock(&drmKeyMutex);
        type = drm_query_value(url, "type");
        pssh = drm_query_value(url, "pssh");
        list = drm_pssh_json(pssh);
        E2iSendMsg("{\"DRM_KEY_REQ\":{\"kid\":\"%s\",\"type\":\"%s\",\"pssh\":%s}}\n",
                   kid, type && strspn(type, "abcdefghijklmnopqrstuvwxyz") == strlen(type) ? type : "",
                   list ? list : "[]");
        free(type);
        free(pssh);
        free(list);
        asked = 1;
        end = drm_now_ms() + drmKeyTimeoutMs;
        pthread_mutex_lock(&drmKeyMutex);
        while (!(entry = drm_key_find(kid)))
        {
            struct timespec ts;
            uint64_t now = drm_now_ms();
            if ((playback && playback->abortRequested) || PlaybackDieNow(0))
            {
                ret = AVERROR_EXIT;
                break;
            }
            if (now >= end)
            {
                break;
            }
            /* waits of at most 100 ms: the exit is not signalled on the condition */
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000000;
            if (ts.tv_nsec >= 1000000000)
            {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&drmKeyCond, &drmKeyMutex, &ts);
        }
        if (!entry && AVERROR_EXIT != ret && drmKeysCount < DRM_KEY_MAX)
        {
            /* no answer: no key, also for a later request of this KID */
            entry = &drmKeys[drmKeysCount++];
            strcpy(entry->kid, kid);
            entry->key[0] = '\0';
            entry->state = -2;
        }
        if (entry && -2 == entry->state)
        {
            drmKeyNoAnswer = 1;
        }
    }
    if (entry && 1 == entry->state)
    {
        strcpy(key, entry->key);
        ret = 0;
    }
    if (asked && AVERROR_EXIT != ret)
    {
        E2iSendMsg("{\"DRM_KEY\":{\"kid\":\"%s\",\"sts\":%d}}\n", kid, entry ? (entry->state > 0 ? 0 : entry->state) : -2);
    }
    pthread_mutex_unlock(&drmKeyMutex);

    /* the wait for the front end is not a stalled read */
    readStallStartMs = read_stall_now_ms();
    return ret;
}

#if LIBAVFORMAT_VERSION_MAJOR >= 58
static int drm_io_open(struct AVFormatContext *s, AVIOContext **pb, const char *url,
                       int flags, AVDictionary **options)
{
    if (0 == strncmp(url, DRM_KEY_URL_PREFIX, sizeof(DRM_KEY_URL_PREFIX) - 1))
    {
        char key[33];
        char data[6 + 33];
        int ret = drm_key_get(s, url + sizeof(DRM_KEY_URL_PREFIX) - 1, key);
        if (ret < 0)
        {
            return ret;
        }
        snprintf(data, sizeof(data), "data:,%s", key);
        return drmOrigIoOpen(s, pb, data, flags, NULL);
    }
    return drmOrigIoOpen(s, pb, url, flags, options);
}
#endif

/* Options and io_open of a format context before avformat_open_input(). */
static void drm_key_prepare(AVFormatContext *ctx, AVDictionary **opts)
{
    int32_t i, n = 0;
    char *keys;
    char *segKeys;

    pthread_mutex_lock(&drmKeyMutex);
    keys = malloc(drmKeysCount * 66 + 1);
    segKeys = malloc(drmKeysCount * 68 + 17);
    if (keys && segKeys)
    {
        /* kid=key:kid=key, and the same with = and : escaped as an option
         * value inside seg_format_options */
        size_t k = 0, s = 0;
        s += sprintf(segKeys, "decryption_keys=");
        for (i = 0; i < drmKeysCount; i++)
        {
            if (1 != drmKeys[i].state)
            {
                continue;
            }
            k += sprintf(keys + k, "%s%s=%s", n ? ":" : "", drmKeys[i].kid, drmKeys[i].key);
            s += sprintf(segKeys + s, "%s%s\\=%s", n ? "\\:" : "", drmKeys[i].kid, drmKeys[i].key);
            n++;
        }
    }
    pthread_mutex_unlock(&drmKeyMutex);

    if (n)
    {
        av_dict_set(opts, "decryption_keys", keys, AV_DICT_DONT_OVERWRITE);
        av_dict_set(opts, "cenc_decryption_keys", keys, AV_DICT_DONT_OVERWRITE);
        av_dict_set(opts, "seg_format_options", segKeys, AV_DICT_DONT_OVERWRITE);
        ffmpeg_printf(10, "%d preloaded CENC keys\n", n);
    }
    free(keys);
    free(segKeys);

#if LIBAVFORMAT_VERSION_MAJOR >= 58
    if (drm_key_request_supported())
    {
        av_dict_set(opts, "decryption_key_url", DRM_KEY_URL, AV_DICT_DONT_OVERWRITE);
        av_dict_set(opts, "cenc_decryption_key_url", DRM_KEY_URL, AV_DICT_DONT_OVERWRITE);
        if (ctx->io_open != drm_io_open)
        {
            drmOrigIoOpen = ctx->io_open;
            ctx->io_open = drm_io_open;
        }
    }
#else
    (void)ctx;
#endif
}

/* ---- streams without a key ----
 *
 * A KID without a key (dk <kid>:-, no answer, not known without
 * --key-request) does not end the read: FFmpeg returns its packets
 * encrypted, with AV_PKT_DATA_ENCRYPTION_INFO (as without any key). The
 * read thread hands them to drm_key_check_packet():
 * - audio: the stream is discarded (DASH stops fetching it) and
 *   {"DRM_KEY_DROP":{"kid","type":"audio","id","switch"}} is sent; if
 *   another audio track is not dropped, the command loop switches to it
 *   ("switch" is its id, otherwise -1 and the playback goes on without
 *   audio);
 * - subtitles and other streams: discarded, DRM_KEY_DROP;
 * - video: {"FF_ERROR":{"msg":"No key for KID <kid> (video)","code":<EACCES>}},
 *   the read stops and the command loop stops the playback as "q" does
 *   (PLAYBACK_STOP).
 */

#define DRM_DROPPED_MAX 32

static const AVStream *drmDropped[DRM_DROPPED_MAX];
static int32_t drmDroppedCount = 0;
static int32_t drmAudioSwitch = -1;              /* with drmKeyMutex */
static const AVStream *drmAudioSwitchStream = NULL;
static volatile int32_t drmFatal = 0; /* 1: stop the playback, 2: stopped */

/* with drmKeyMutex */
static int32_t drm_stream_dropped(const AVStream *st)
{
    int32_t i;
    for (i = 0; i < drmDroppedCount; i++)
    {
        if (drmDropped[i] == st)
        {
            return 1;
        }
    }
    return 0;
}

/* the command loop: the id of the audio track to switch to, or -1; never
 * one that has been dropped meanwhile */
int32_t ffmpeg_drm_key_audio_switch(void)
{
    int32_t id;
    pthread_mutex_lock(&drmKeyMutex);
    id = drmAudioSwitch;
    if (id >= 0 && drm_stream_dropped(drmAudioSwitchStream))
    {
        id = -1;
    }
    drmAudioSwitch = -1;
    drmAudioSwitchStream = NULL;
    pthread_mutex_unlock(&drmKeyMutex);
    return id;
}

/* CONTAINER_SWITCH_AUDIO: whether the stream of an audio track was dropped
 * (no key); such a track is not switched to */
static int32_t drm_audio_stream_dropped(const AVStream *st)
{
    int32_t dropped;
    pthread_mutex_lock(&drmKeyMutex);
    dropped = st && drm_stream_dropped(st);
    pthread_mutex_unlock(&drmKeyMutex);
    return dropped;
}

/* the command loop: 1 once when the playback has to be stopped */
int32_t ffmpeg_drm_key_stop(void)
{
    return __sync_bool_compare_and_swap(&drmFatal, 1, 2);
}

/* the read thread: whether it has to stop reading */
static int32_t drm_key_fatal(void)
{
    return drmFatal != 0;
}

/* A packet of the read thread, with the mutex held: 0 to use it, 1 to drop
 * it (its stream has no key). */
static int32_t drm_key_check_packet(Context_t *context, int32_t cAVIdx, AVPacket *pkt)
{
#if LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVUTIL_VERSION_MAJOR >= 56
#if LIBAVCODEC_VERSION_MAJOR >= 59
    size_t size = 0;
#else
    int size = 0;
#endif
    const uint8_t *sideData = av_packet_get_side_data(pkt, AV_PKT_DATA_ENCRYPTION_INFO, &size);
    AVStream *st;
    AVEncryptionInfo *info;
    enum AVMediaType type;
    const char *typeName;
    char kid[33] = "";
    int32_t id, switchTo = -1;

    if (!sideData)
    {
        return 0;
    }
    st = avContextTab[cAVIdx]->streams[pkt->stream_index];
    pthread_mutex_lock(&drmKeyMutex);
    if (drm_stream_dropped(st))
    {
        pthread_mutex_unlock(&drmKeyMutex);
        /* read before the discard took effect: it stays without a key */
        st->discard = AVDISCARD_ALL;
        return 1;
    }
    pthread_mutex_unlock(&drmKeyMutex);

    info = av_encryption_info_get_side_data(sideData, size);
    if (info)
    {
        if (16 == info->key_id_size)
        {
            int32_t i;
            for (i = 0; i < 16; i++)
            {
                snprintf(kid + 2 * i, 3, "%02x", info->key_id[i]);
            }
        }
        av_encryption_info_free(info);
    }
    type = get_codecpar(st)->codec_type;
    typeName = av_get_media_type_string(type);
    if (!typeName)
    {
        typeName = "data";
    }
    id = stream_track_id(avContextTab[cAVIdx], st);

    if (AVMEDIA_TYPE_VIDEO == type)
    {
        ffmpeg_err("no key for KID %s of the video, playback stops\n", kid);
        E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"No key for KID %s (video)\",\"code\":%d}}\n", kid, AVERROR(EACCES));
        drmFatal = 1;
        return 1;
    }

    st->discard = AVDISCARD_ALL;
    pthread_mutex_lock(&drmKeyMutex);
    if (drmDroppedCount < DRM_DROPPED_MAX)
    {
        drmDropped[drmDroppedCount++] = st;
    }
    if (AVMEDIA_TYPE_AUDIO == type && context->manager->audio)
    {
        /* a new audio track only if this one plays or was about to be
         * switched to; a pending switch to it is replaced (or cancelled) */
        Track_t *current = NULL;
        context->manager->audio->Command(context, MANAGER_GET_TRACK, &current);
        if ((current && current->stream == st) || drmAudioSwitchStream == st)
        {
            Track_t *tracks = NULL;
            int32_t count = 0, i;
            context->manager->audio->Command(context, MANAGER_REF_LIST, &tracks);
            context->manager->audio->Command(context, MANAGER_REF_LIST_SIZE, &count);
            for (i = 0; tracks && i < count; i++)
            {
                const AVStream *other = (const AVStream *)tracks[i].stream;
                if (other && other != st && !drm_stream_dropped(other))
                {
                    switchTo = tracks[i].Id;
                    drmAudioSwitchStream = other;
                    break;
                }
            }
            drmAudioSwitch = switchTo;
            if (switchTo < 0)
            {
                drmAudioSwitchStream = NULL;
            }
        }
    }
    pthread_mutex_unlock(&drmKeyMutex);
    ffmpeg_err("no key for KID %s of %s track %d, dropped\n", kid, typeName, id);
    E2iSendMsg("{\"DRM_KEY_DROP\":{\"kid\":\"%s\",\"type\":\"%s\",\"id\":%d,\"switch\":%d}}\n",
               kid, typeName, id, switchTo);
    return 1;
#else
    (void)context;
    (void)cAVIdx;
    (void)pkt;
    return 0;
#endif
}
