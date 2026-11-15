#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hevc_hdr.h"

/* The HDR type of an HEVC stream from its SPS VUI and SEI. The NAL units are
 * written bit by bit: the SPS is a complete one (sub-layers, conformance
 * window, scaling lists, short and long term reference pictures) whose
 * profile_tier_level has runs of zero bytes, so it always carries emulation
 * prevention bytes before the VUI. Every buffer handed to the parser is a heap
 * copy of exactly its size, so ASan sees any read beyond it. Each case runs in
 * a child process of its own. */

#define KIB 1024U

typedef struct
{
    uint8_t data[1024];
    size_t bits;
} Bits;

static void put(Bits *bits, uint32_t value, int count)
{
    while (count-- > 0)
    {
        assert(bits->bits < 8 * sizeof(bits->data));
        if ((value >> count) & 1)
            bits->data[bits->bits >> 3] |= 0x80 >> (bits->bits & 7);
        bits->bits++;
    }
}

static void ue(Bits *bits, uint32_t value)
{
    uint32_t code = value + 1;
    int length = 0;
    while (code >> (length + 1))
        length++;
    put(bits, 0, length);
    put(bits, code, length + 1);
}

static void trailing_bits(Bits *bits)
{
    put(bits, 1, 1);
    while (bits->bits & 7)
        put(bits, 0, 1);
}

/* NAL header and payload with emulation prevention, no start code. */
static size_t escape(uint8_t *out, int type, const Bits *bits)
{
    size_t size = 0, i;
    int zeros = 0;
    out[size++] = (uint8_t)(type << 1);
    out[size++] = 1;
    for (i = 0; i < bits->bits / 8; i++)
    {
        uint8_t byte = bits->data[i];
        if (zeros >= 2 && byte <= 3)
        {
            out[size++] = 3;
            zeros = 0;
        }
        out[size++] = byte;
        zeros = byte ? 0 : zeros + 1;
    }
    return size;
}

static int has_emulation_prevention(const uint8_t *nal, size_t size)
{
    size_t i;
    for (i = 0; i + 2 < size; i++)
        if (!nal[i] && !nal[i + 1] && nal[i + 2] == 3)
            return 1;
    return 0;
}

static void profile_tier_level(Bits *bits)
{
    put(bits, 0, 2);          /* general_profile_space */
    put(bits, 0, 1);          /* general_tier_flag */
    put(bits, 2, 5);          /* Main 10 */
    put(bits, 0x20000000, 32);
    put(bits, 0x9, 4);        /* progressive, frame only */
    put(bits, 0, 32);         /* reserved: 43 zero bits + 1 */
    put(bits, 0, 12);
    put(bits, 120, 8);        /* level 4 */
}

/* transfer < 0: no VUI. */
static size_t sps(uint8_t *out, int transfer)
{
    Bits bits = {{0}, 0};
    int i;
    put(&bits, 0, 4);         /* sps_video_parameter_set_id */
    put(&bits, 1, 3);         /* sps_max_sub_layers_minus1 */
    put(&bits, 1, 1);
    profile_tier_level(&bits);
    put(&bits, 1, 1);         /* sub_layer_profile_present_flag[0] */
    put(&bits, 1, 1);         /* sub_layer_level_present_flag[0] */
    for (i = 1; i < 8; i++)
        put(&bits, 0, 2);
    put(&bits, 0, 8);         /* sub-layer profile: 88 bits */
    put(&bits, 0, 32);
    put(&bits, 0, 32);
    put(&bits, 0, 16);
    put(&bits, 90, 8);        /* sub-layer level */
    ue(&bits, 0);             /* sps_seq_parameter_set_id */
    ue(&bits, 1);             /* chroma_format_idc */
    ue(&bits, 3840);
    ue(&bits, 2160);
    put(&bits, 1, 1);         /* conformance_window_flag */
    ue(&bits, 0);
    ue(&bits, 0);
    ue(&bits, 0);
    ue(&bits, 4);
    ue(&bits, 2);             /* bit_depth_luma_minus8 */
    ue(&bits, 2);
    ue(&bits, 4);             /* log2_max_pic_order_cnt_lsb_minus4 */
    put(&bits, 1, 1);         /* sps_sub_layer_ordering_info_present_flag */
    for (i = 0; i < 2; i++)
    {
        ue(&bits, 4);
        ue(&bits, 2);
        ue(&bits, 0);
    }
    ue(&bits, 0);             /* log2_min_luma_coding_block_size_minus3 */
    ue(&bits, 3);
    ue(&bits, 0);
    ue(&bits, 3);
    ue(&bits, 1);
    ue(&bits, 1);
    put(&bits, 1, 1);         /* scaling_list_enabled_flag */
    put(&bits, 1, 1);         /* sps_scaling_list_data_present_flag */
    for (i = 0; i < 20; i++)
    {
        /* 4x4 matrix 0 and 32x32 matrix 0 coded, the others predicted */
        if (i == 0 || i == 18)
        {
            int count = i ? 64 : 16, k;
            put(&bits, 1, 1);
            if (i)
                ue(&bits, 15);  /* scaling_list_dc_coef_minus8 = 8 */
            for (k = 0; k < count; k++)
                ue(&bits, k & 1 ? 2 : 1);
        }
        else
        {
            put(&bits, 0, 1);
            ue(&bits, 0);
        }
    }
    put(&bits, 1, 1);         /* amp_enabled_flag */
    put(&bits, 1, 1);         /* sample_adaptive_offset_enabled_flag */
    put(&bits, 0, 1);         /* pcm_enabled_flag */
    ue(&bits, 2);             /* num_short_term_ref_pic_sets */
    ue(&bits, 1);             /* set 0: one negative picture */
    ue(&bits, 0);
    ue(&bits, 0);
    put(&bits, 1, 1);
    put(&bits, 1, 1);         /* set 1: predicted from set 0 */
    put(&bits, 0, 1);
    ue(&bits, 0);
    put(&bits, 1, 1);
    put(&bits, 0, 1);
    put(&bits, 1, 1);
    put(&bits, 1, 1);         /* long_term_ref_pics_present_flag */
    ue(&bits, 2);
    put(&bits, 0xa5, 8);      /* lt_ref_pic_poc_lsb_sps: log2_max_pic_order_cnt_lsb bits */
    put(&bits, 0, 1);         /* used_by_curr_pic_lt_sps_flag */
    put(&bits, 0xc3, 8);
    put(&bits, 1, 1);
    put(&bits, 1, 1);         /* sps_temporal_mvp_enabled_flag */
    put(&bits, 1, 1);         /* strong_intra_smoothing_enabled_flag */
    put(&bits, transfer >= 0, 1);
    if (transfer >= 0)
    {
        put(&bits, 1, 1);     /* aspect_ratio_info_present_flag */
        put(&bits, 255, 8);
        put(&bits, 4, 16);
        put(&bits, 3, 16);
        put(&bits, 1, 1);     /* overscan_info_present_flag */
        put(&bits, 0, 1);
        put(&bits, 1, 1);     /* video_signal_type_present_flag */
        put(&bits, 5, 3);
        put(&bits, 0, 1);
        put(&bits, 1, 1);     /* colour_description_present_flag */
        put(&bits, 9, 8);
        put(&bits, (uint32_t)transfer, 8);
        put(&bits, 9, 8);
        put(&bits, 0, 7);     /* chroma_loc .. bitstream_restriction */
    }
    put(&bits, 0, 1);         /* sps_extension_present_flag */
    trailing_bits(&bits);
    return escape(out, 33, &bits);
}

static size_t sei(uint8_t *out, int payload_type, const uint8_t *payload, size_t size)
{
    Bits bits = {{0}, 0};
    size_t i;
    put(&bits, (uint32_t)payload_type, 8);
    put(&bits, (uint32_t)size, 8);
    for (i = 0; i < size; i++)
        put(&bits, payload[i], 8);
    trailing_bits(&bits);
    return escape(out, 39, &bits);
}

static const uint8_t mastering_display[24] = {
    0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80,
    0x3d, 0x13, 0x40, 0x42, 0x00, 0x98, 0x96, 0x80, 0x00, 0x00, 0x00, 0x32};
static const uint8_t content_light[4] = {0x03, 0xe8, 0x01, 0x90};
static const uint8_t transfer_hlg = 18;

static size_t mastering_sei(uint8_t *out) { return sei(out, 137, mastering_display, 24); }
static size_t content_light_sei(uint8_t *out) { return sei(out, 144, content_light, 4); }
static size_t hlg_sei(uint8_t *out) { return sei(out, 147, &transfer_hlg, 1); }

/* A suffix SEI with two messages: content light level, then mastering display. */
static size_t suffix_light_and_mastering_sei(uint8_t *out)
{
    Bits bits = {{0}, 0};
    size_t i;
    put(&bits, 144, 8);
    put(&bits, 4, 8);
    for (i = 0; i < 4; i++)
        put(&bits, content_light[i], 8);
    put(&bits, 137, 8);
    put(&bits, 24, 8);
    for (i = 0; i < 24; i++)
        put(&bits, mastering_display[i], 8);
    trailing_bits(&bits);
    return escape(out, 40, &bits);
}

/* A stream under construction: Annex-B or with 4-byte NAL lengths. */
typedef struct
{
    uint8_t data[4096];
    size_t size;
    int prefixed;
} Stream;

static void add(Stream *stream, const uint8_t *nal, size_t size)
{
    assert(stream->size + 4 + size <= sizeof(stream->data));
    if (stream->prefixed)
    {
        stream->data[stream->size++] = (uint8_t)(size >> 24);
        stream->data[stream->size++] = (uint8_t)(size >> 16);
        stream->data[stream->size++] = (uint8_t)(size >> 8);
        stream->data[stream->size++] = (uint8_t)size;
    }
    else
    {
        static const uint8_t start_code[4] = {0, 0, 0, 1};
        memcpy(stream->data + stream->size, start_code, 4);
        stream->size += 4;
    }
    memcpy(stream->data + stream->size, nal, size);
    stream->size += size;
}

static void add_sps(Stream *stream, int transfer)
{
    uint8_t nal[1024];
    add(stream, nal, sps(nal, transfer));
}

static void add_sei(Stream *stream, size_t (*make)(uint8_t *))
{
    uint8_t nal[64];
    add(stream, nal, make(nal));
}

/* A heap copy of exactly the given size. */
static uint8_t *copy(const uint8_t *data, size_t size)
{
    uint8_t *buffer = malloc(size ? size : 1);
    assert(buffer);
    if (size)
        memcpy(buffer, data, size);
    return buffer;
}

static int classify(const Stream *stream, int *saw_sps)
{
    uint8_t *buffer = copy(stream->data, stream->size);
    int result = hevc_hdr_classify(buffer, stream->size, saw_sps);
    free(buffer);
    return result;
}

static int configure(HevcHdrDetector *detector, const uint8_t *data, size_t size)
{
    uint8_t *buffer = copy(data, size);
    int done = hevc_hdr_detector_configure(detector, buffer, size);
    free(buffer);
    return done;
}

static int feed(HevcHdrDetector *detector, const uint8_t *data, size_t size)
{
    uint8_t *buffer = copy(data, size);
    int done = hevc_hdr_detector_feed(detector, buffer, size);
    free(buffer);
    return done;
}

static int feed_stream(HevcHdrDetector *detector, const Stream *stream)
{
    return feed(detector, stream->data, stream->size);
}

/* hvcC with 4-byte NAL lengths and one array per NAL unit. */
static size_t hvcc(uint8_t *out, const Stream *nals)
{
    size_t size = 23, offset = 0;
    memset(out, 0, 23);
    out[0] = 1;
    out[1] = 2;
    out[21] = 0xfc | 3;
    while (offset < nals->size)
    {
        size_t length = ((size_t)nals->data[offset] << 24 | (size_t)nals->data[offset + 1] << 16 |
                         (size_t)nals->data[offset + 2] << 8 | nals->data[offset + 3]);
        const uint8_t *nal = nals->data + offset + 4;
        out[size++] = 0x80 | (nal[0] >> 1);
        out[size++] = 0;
        out[size++] = 1;
        out[size++] = (uint8_t)(length >> 8);
        out[size++] = (uint8_t)length;
        memcpy(out + size, nal, length);
        size += length;
        offset += 4 + length;
        out[22]++;
    }
    return size;
}

/* Video packets that are neither SPS nor SEI: 4 KiB each, slice NAL units
 * whose payload has no zero byte. Returns the result of the last feed. */
static int feed_slices(HevcHdrDetector *detector, int prefixed, size_t bytes)
{
    static uint8_t packet[4 * KIB];
    int done = 0;
    assert(bytes % sizeof(packet) == 0);
    memset(packet, 0xa5, sizeof(packet));
    if (prefixed)
    {
        packet[0] = 0;
        packet[1] = 0;
        packet[2] = (uint8_t)((sizeof(packet) - 4) >> 8);
        packet[3] = (uint8_t)(sizeof(packet) - 4);
    }
    else
    {
        packet[0] = 0;
        packet[1] = 0;
        packet[2] = 0;
        packet[3] = 1;
    }
    packet[4] = 1 << 1;
    packet[5] = 1;
    for (; bytes; bytes -= sizeof(packet))
        done = hevc_hdr_detector_feed(detector, packet, sizeof(packet));
    return done;
}

static void vui_transfer(void)
{
    static const struct
    {
        int transfer;
        int type;
    } cases[] = {
        {-1, HEVC_HDR_SDR}, {1, HEVC_HDR_SDR}, {2, HEVC_HDR_SDR}, {6, HEVC_HDR_SDR},
        {16, HEVC_HDR_HDR10}, {18, HEVC_HDR_HLG}, {14, HEVC_HDR_GENERIC}, {15, HEVC_HDR_GENERIC}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Stream stream = {{0}, 0, 0};
        uint8_t nal[1024];
        size_t size = sps(nal, cases[i].transfer);
        int saw_sps = 0;
        /* the escaped SPS differs from its RBSP before the VUI */
        assert(has_emulation_prevention(nal, size));
        add(&stream, nal, size);
        assert(classify(&stream, &saw_sps) == cases[i].type && saw_sps == 1);
    }
}

static void sei_messages(void)
{
    Stream stream = {{0}, 0, 0};
    int saw_sps = 1;

    /* mastering display SEI alone: HDR10 */
    add_sei(&stream, mastering_sei);
    assert(classify(&stream, &saw_sps) == HEVC_HDR_HDR10 && saw_sps == 0);
    /* with an SDR VUI as well */
    stream.size = 0;
    add_sps(&stream, 1);
    add_sei(&stream, mastering_sei);
    assert(classify(&stream, &saw_sps) == HEVC_HDR_HDR10 && saw_sps == 1);
    /* content light level alone is not HDR10 */
    stream.size = 0;
    add_sps(&stream, 1);
    add_sei(&stream, content_light_sei);
    assert(classify(&stream, NULL) == HEVC_HDR_SDR);
    /* mastering display as the second message of a suffix SEI */
    stream.size = 0;
    add_sps(&stream, 1);
    add_sei(&stream, suffix_light_and_mastering_sei);
    assert(classify(&stream, NULL) == HEVC_HDR_HDR10);
    /* the alternative transfer SEI overrides the VUI, before or after the SPS */
    stream.size = 0;
    add_sps(&stream, 14);
    add_sei(&stream, hlg_sei);
    assert(classify(&stream, NULL) == HEVC_HDR_HLG);
    stream.size = 0;
    add_sei(&stream, hlg_sei);
    add_sps(&stream, 1);
    assert(classify(&stream, NULL) == HEVC_HDR_HLG);
}

static void annex_b_extradata(void)
{
    static const struct
    {
        int transfer;
        int type;
    } cases[] = {{16, HEVC_HDR_HDR10}, {18, HEVC_HDR_HLG}};
    size_t i;
    for (i = 0; i < 2; i++)
    {
        HevcHdrDetector detector;
        Stream extradata = {{0}, 0, 0};
        add_sps(&extradata, cases[i].transfer);
        hevc_hdr_detector_init(&detector);
        assert(hevc_hdr_detector_result(&detector) == -1);
        assert(configure(&detector, extradata.data, extradata.size) == 1);
        assert(detector.done && hevc_hdr_detector_result(&detector) == cases[i].type);
        /* final: packets change nothing */
        assert(feed_slices(&detector, 0, 64 * KIB) == 0);
        assert(hevc_hdr_detector_result(&detector) == cases[i].type);
        hevc_hdr_detector_destroy(&detector);
    }
}

static void hvcc_extradata(void)
{
    HevcHdrDetector detector;
    Stream nals = {{0}, 0, 1}, packet = {{0}, 0, 1};
    uint8_t record[4096];
    size_t size;

    add_sps(&nals, 16);
    size = hvcc(record, &nals);
    hevc_hdr_detector_init(&detector);
    assert(configure(&detector, record, size) == 1);
    assert(hevc_hdr_detector_result(&detector) == HEVC_HDR_HDR10);
    hevc_hdr_detector_destroy(&detector);

    /* SDR record, then a mastering display SEI in a length-prefixed packet */
    nals.size = 0;
    add_sps(&nals, 1);
    size = hvcc(record, &nals);
    hevc_hdr_detector_init(&detector);
    assert(configure(&detector, record, size) == 0);
    assert(!detector.done && hevc_hdr_detector_result(&detector) == -1);
    add_sei(&packet, mastering_sei);
    feed_stream(&detector, &packet);
    assert(feed_slices(&detector, 1, 64 * KIB) == 1);
    assert(detector.done && hevc_hdr_detector_result(&detector) == HEVC_HDR_HDR10);
    hevc_hdr_detector_destroy(&detector);
}

/* Backwards compatible HLG: 14 in the VUI of the extradata, 18 in an SEI of
 * the first packet. */
static void alternative_transfer_after_the_extradata(void)
{
    HevcHdrDetector detector;
    Stream extradata = {{0}, 0, 0}, packet = {{0}, 0, 0};

    add_sps(&extradata, 14);
    hevc_hdr_detector_init(&detector);
    assert(configure(&detector, extradata.data, extradata.size) == 0);
    assert(!detector.done && hevc_hdr_detector_result(&detector) == HEVC_HDR_GENERIC);
    add_sei(&packet, hlg_sei);
    feed_stream(&detector, &packet);
    assert(feed_slices(&detector, 0, 64 * KIB) == 1);
    assert(detector.done && hevc_hdr_detector_result(&detector) == HEVC_HDR_HLG);
    hevc_hdr_detector_destroy(&detector);
}

static void mastering_display_after_the_extradata(void)
{
    HevcHdrDetector detector;
    Stream extradata = {{0}, 0, 0}, packet = {{0}, 0, 0};

    add_sps(&extradata, 1);
    hevc_hdr_detector_init(&detector);
    assert(configure(&detector, extradata.data, extradata.size) == 0);
    add_sei(&packet, mastering_sei);
    feed_stream(&detector, &packet);
    assert(feed_slices(&detector, 0, 64 * KIB) == 1);
    assert(hevc_hdr_detector_result(&detector) == HEVC_HDR_HDR10);
    hevc_hdr_detector_destroy(&detector);
}

/* SDR and "other HDR" are provisional until 768 KiB after the first SPS. */
static void provisional_until_768_kib_after_the_sps(int transfer, int type)
{
    HevcHdrDetector detector;
    Stream extradata = {{0}, 0, 0}, packet = {{0}, 0, 0};

    add_sps(&extradata, transfer);
    hevc_hdr_detector_init(&detector);
    assert(configure(&detector, extradata.data, extradata.size) == 0);
    assert(feed_slices(&detector, 0, 704 * KIB) == 0);
    assert(!detector.done);
    assert(hevc_hdr_detector_result(&detector) == (type == HEVC_HDR_SDR ? -1 : type));
    assert(feed_slices(&detector, 0, 64 * KIB) == 1);
    assert(detector.done && hevc_hdr_detector_result(&detector) == type);
    assert(!detector.buffer && !detector.size);
    /* a mastering display SEI after that is not looked at */
    add_sei(&packet, mastering_sei);
    assert(feed_stream(&detector, &packet) == 0);
    assert(feed_slices(&detector, 0, 64 * KIB) == 0);
    assert(hevc_hdr_detector_result(&detector) == type);
    hevc_hdr_detector_destroy(&detector);
}

static void sdr_final_after_768_kib(void)
{
    provisional_until_768_kib_after_the_sps(1, HEVC_HDR_SDR);
}

static void bt2020_final_after_768_kib(void)
{
    provisional_until_768_kib_after_the_sps(14, HEVC_HDR_GENERIC);
}

/* Without any SPS the search ends after 2 MiB. */
static void sdr_final_after_2_mib(void)
{
    HevcHdrDetector detector;
    hevc_hdr_detector_init(&detector);
    assert(feed_slices(&detector, 0, 2048 * KIB - 64 * KIB) == 0);
    assert(!detector.done && hevc_hdr_detector_result(&detector) == -1);
    assert(feed_slices(&detector, 0, 64 * KIB) == 1);
    assert(detector.done && hevc_hdr_detector_result(&detector) == HEVC_HDR_SDR);
    hevc_hdr_detector_destroy(&detector);
}

/* An SPS first seen after 1.5 MiB: the 2 MiB end comes before 768 KiB more. */
static void sdr_final_after_2_mib_with_a_late_sps(void)
{
    HevcHdrDetector detector;
    Stream packet = {{0}, 0, 0};
    hevc_hdr_detector_init(&detector);
    assert(feed_slices(&detector, 0, 1536 * KIB) == 0);
    add_sps(&packet, 1);
    assert(feed_stream(&detector, &packet) == 0);
    assert(feed_slices(&detector, 0, 448 * KIB) == 0);
    assert(!detector.done && detector.first_sps_at);
    assert(feed_slices(&detector, 0, 64 * KIB) == 1);
    assert(detector.done && hevc_hdr_detector_result(&detector) == HEVC_HDR_SDR);
    hevc_hdr_detector_destroy(&detector);
}

/* Every prefix of valid input, as classifier input, extradata and packet. */
static void truncated_input(void)
{
    Stream annex_b = {{0}, 0, 0}, nals = {{0}, 0, 1};
    uint8_t record[4096];
    size_t record_size, i;

    add_sps(&annex_b, 16);
    add_sei(&annex_b, mastering_sei);
    add_sei(&annex_b, hlg_sei);
    add_sps(&nals, 16);
    add_sei(&nals, hlg_sei);
    record_size = hvcc(record, &nals);
    for (i = 0; i <= annex_b.size; i++)
    {
        HevcHdrDetector detector;
        uint8_t *buffer = copy(annex_b.data, i);
        int type = hevc_hdr_classify(buffer, i, NULL);
        free(buffer);
        assert(type >= HEVC_HDR_SDR && type <= HEVC_HDR_GENERIC);
        hevc_hdr_detector_init(&detector);
        configure(&detector, annex_b.data, i);
        hevc_hdr_detector_destroy(&detector);
        hevc_hdr_detector_init(&detector);
        feed(&detector, annex_b.data, i);
        hevc_hdr_detector_destroy(&detector);
    }
    for (i = 0; i <= record_size; i++)
    {
        HevcHdrDetector detector;
        hevc_hdr_detector_init(&detector);
        configure(&detector, record, i);
        assert(hevc_hdr_detector_result(&detector) >= -1 &&
               hevc_hdr_detector_result(&detector) <= HEVC_HDR_GENERIC);
        hevc_hdr_detector_destroy(&detector);
    }
    for (i = 0; i <= nals.size; i++)
    {
        HevcHdrDetector detector;
        hevc_hdr_detector_init(&detector);
        configure(&detector, record, record_size);
        hevc_hdr_detector_reset(&detector);
        configure(&detector, record, 23);
        feed(&detector, nals.data, i);
        hevc_hdr_detector_destroy(&detector);
    }
}

static uint32_t seed = 12345;

static uint32_t next_random(void)
{
    seed = seed * 1103515245U + 12345U;
    return seed >> 8;
}

/* Random bytes behind SPS and SEI headers, random hvcC records and packets,
 * and lengths that claim more than there is. */
static void garbage_input(void)
{
    uint8_t data[512];
    int round;
    for (round = 0; round < 4000; round++)
    {
        HevcHdrDetector detector;
        size_t size = 6 + next_random() % (sizeof(data) - 6), i;
        int type;
        for (i = 0; i < size; i++)
            data[i] = (uint8_t)next_random();
        /* mostly zero bits, as in a real SPS, every fourth round */
        if (round % 4 == 1)
            for (i = 0; i < size; i++)
                data[i] &= (uint8_t)next_random();
        switch (round % 4)
        {
        case 0:
        case 1:
            memcpy(data, "\0\0\1\x42\1", 5);
            break;
        case 2:
            memcpy(data, "\0\0\1\x4e\1", 5);
            break;
        default:
            break;
        }
        {
            uint8_t *buffer = copy(data, size);
            type = hevc_hdr_classify(buffer, size, NULL);
            free(buffer);
        }
        assert(type >= HEVC_HDR_SDR && type <= HEVC_HDR_GENERIC);

        hevc_hdr_detector_init(&detector);
        if (round % 2)
        {
            data[0] = 1;  /* an hvcC record */
            data[21] = (uint8_t)next_random();
            configure(&detector, data, size);
        }
        for (i = 0; i < 3; i++)
        {
            size_t packet_size = 1 + next_random() % size;
            size_t k;
            for (k = 0; k < packet_size; k++)
                data[k] = (uint8_t)next_random();
            if (i == 1 && packet_size > 4)
                memcpy(data, "\0\0\0\1", 4);
            feed(&detector, data, packet_size);
        }
        assert(hevc_hdr_detector_result(&detector) >= -1 &&
               hevc_hdr_detector_result(&detector) <= HEVC_HDR_GENERIC);
        hevc_hdr_detector_destroy(&detector);
    }
}

static int run(const char *name, void (*test)(void))
{
    int status;
    pid_t child;
    fflush(stderr);
    child = fork();
    assert(child >= 0);
    if (!child)
    {
        test();
        exit(0); /* not _exit(): LeakSanitizer checks at exit */
    }
    assert(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && !WEXITSTATUS(status))
        return 0;
    fprintf(stderr, "hevc_hdr: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("SPS VUI transfer characteristics", vui_transfer);
    failed += run("mastering display, content light and alternative transfer SEI", sei_messages);
    failed += run("Annex-B extradata", annex_b_extradata);
    failed += run("hvcC extradata and length-prefixed packets", hvcc_extradata);
    failed += run("alternative transfer SEI after a BT.2020 VUI", alternative_transfer_after_the_extradata);
    failed += run("mastering display SEI after an SDR VUI", mastering_display_after_the_extradata);
    failed += run("SDR final 768 KiB after the SPS", sdr_final_after_768_kib);
    failed += run("BT.2020 final 768 KiB after the SPS", bt2020_final_after_768_kib);
    failed += run("SDR final after 2 MiB without an SPS", sdr_final_after_2_mib);
    failed += run("SDR final after 2 MiB with a late SPS", sdr_final_after_2_mib_with_a_late_sps);
    failed += run("truncated input", truncated_input);
    failed += run("garbage input", garbage_input);
    return failed ? 1 : 0;
}
