/*
 * Lightweight HEVC HDR classifier for exteplayer3.
 *
 * Parser logic is a C port of BlackHole/enigma2 lib/service/hevc_hdr.cpp.
 * It reads transfer_characteristics from SPS VUI and HDR-related SEI payloads.
 *
 * Copyright (C) 2026 OpenBH contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "hevc_hdr.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define HEVC_HDR_MAX_ES_BYTES  (2U * 1024U * 1024U)
#define HEVC_HDR_CLASSIFY_STEP (64U * 1024U)
#define HEVC_HDR_SDR_COMMIT    (768U * 1024U)
#define HEVC_HDR_INITIAL_CAP   (512U * 1024U)

typedef struct BitReader_s {
    uint8_t *data;
    size_t bit_pos;
    size_t bit_size;
} BitReader;

static int bit_reader_init(BitReader *reader, const uint8_t *data, size_t length)
{
    size_t i;
    size_t out = 0;
    int zeros = 0;

    memset(reader, 0, sizeof(*reader));
    if (!data || !length)
        return 0;

    reader->data = malloc(length);
    if (!reader->data)
        return 0;

    for (i = 0; i < length; ++i) {
        uint8_t byte = data[i];
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;
        }
        reader->data[out++] = byte;
        zeros = byte == 0x00 ? zeros + 1 : 0;
    }
    reader->bit_size = out * 8U;
    return 1;
}

static void bit_reader_destroy(BitReader *reader)
{
    free(reader->data);
    memset(reader, 0, sizeof(*reader));
}

static int bit_reader_exhausted(const BitReader *reader)
{
    return reader->bit_pos >= reader->bit_size;
}

static uint32_t bit_reader_bit(BitReader *reader)
{
    size_t byte_index;
    int offset;

    if (reader->bit_pos >= reader->bit_size)
        return 0;
    byte_index = reader->bit_pos >> 3;
    offset = 7 - (int)(reader->bit_pos & 7U);
    ++reader->bit_pos;
    return (reader->data[byte_index] >> offset) & 1U;
}

static uint32_t bit_reader_u(BitReader *reader, int bits)
{
    uint32_t value = 0;
    while (bits-- > 0)
        value = (value << 1) | bit_reader_bit(reader);
    return value;
}

static uint32_t bit_reader_ue(BitReader *reader)
{
    int leading_zeroes = 0;
    while (!bit_reader_exhausted(reader) && bit_reader_bit(reader) == 0 && leading_zeroes < 31)
        ++leading_zeroes;
    if (leading_zeroes >= 31)
        return UINT_MAX;
    return ((1U << leading_zeroes) - 1U) + bit_reader_u(reader, leading_zeroes);
}

static int32_t bit_reader_se(BitReader *reader)
{
    uint32_t value = bit_reader_ue(reader);
    return (value & 1U) ? (int32_t)((value + 1U) >> 1) : -(int32_t)(value >> 1);
}

static void skip_ptl(BitReader *reader, int max_sub_layers)
{
    int i;
    int sub_profile[8] = {0};
    int sub_level[8] = {0};

    bit_reader_u(reader, 2);
    bit_reader_u(reader, 1);
    bit_reader_u(reader, 5);
    bit_reader_u(reader, 32);
    bit_reader_u(reader, 32);
    bit_reader_u(reader, 16);
    bit_reader_u(reader, 8);

    for (i = 0; i < max_sub_layers; ++i) {
        sub_profile[i] = bit_reader_u(reader, 1);
        sub_level[i] = bit_reader_u(reader, 1);
    }
    if (max_sub_layers > 0) {
        for (i = max_sub_layers; i < 8; ++i)
            bit_reader_u(reader, 2);
    }
    for (i = 0; i < max_sub_layers; ++i) {
        if (sub_profile[i]) {
            bit_reader_u(reader, 2);
            bit_reader_u(reader, 1);
            bit_reader_u(reader, 5);
            bit_reader_u(reader, 32);
            bit_reader_u(reader, 32);
            bit_reader_u(reader, 16);
        }
        if (sub_level[i])
            bit_reader_u(reader, 8);
    }
}

static void skip_strps(BitReader *reader, int count)
{
    int index;
    int num_delta_pocs[65] = {0};

    if (count > 64)
        count = 64;
    for (index = 0; index < count; ++index) {
        int inter_prediction;
        if (bit_reader_exhausted(reader))
            break;
        inter_prediction = index != 0 ? bit_reader_u(reader, 1) : 0;
        if (inter_prediction) {
            int j;
            int previous_count;
            int current_count = 0;
            bit_reader_u(reader, 1);
            bit_reader_ue(reader);
            previous_count = num_delta_pocs[index - 1];
            for (j = 0; j <= previous_count; ++j) {
                int used = bit_reader_u(reader, 1);
                int use_delta = 1;
                if (!used)
                    use_delta = bit_reader_u(reader, 1);
                if (used || use_delta)
                    ++current_count;
            }
            num_delta_pocs[index] = current_count;
        } else {
            uint32_t negative = bit_reader_ue(reader);
            uint32_t positive = bit_reader_ue(reader);
            uint32_t j;
            if (negative > 16)
                negative = 16;
            if (positive > 16)
                positive = 16;
            num_delta_pocs[index] = (int)(negative + positive);
            for (j = 0; j < negative && !bit_reader_exhausted(reader); ++j) {
                bit_reader_ue(reader);
                bit_reader_u(reader, 1);
            }
            for (j = 0; j < positive && !bit_reader_exhausted(reader); ++j) {
                bit_reader_ue(reader);
                bit_reader_u(reader, 1);
            }
        }
    }
}

static void skip_scaling(BitReader *reader)
{
    int size_id;
    for (size_id = 0; size_id < 4; ++size_id) {
        int matrix_count = size_id != 3 ? 6 : 2;
        int matrix_id;
        for (matrix_id = 0; matrix_id < matrix_count; ++matrix_id) {
            if (!bit_reader_u(reader, 1)) {
                bit_reader_ue(reader);
            } else {
                int coefficient_count = 1 << (4 + (size_id << 1));
                int coefficient;
                if (coefficient_count > 64)
                    coefficient_count = 64;
                if (size_id > 1)
                    bit_reader_se(reader);
                for (coefficient = 0; coefficient < coefficient_count; ++coefficient)
                    bit_reader_se(reader);
            }
        }
    }
}

/* Returns transfer_characteristics, or -1. */
static int sps_transfer(const uint8_t *rbsp, size_t length)
{
    BitReader reader;
    int result = -1;
    int max_sub_layers;
    uint32_t chroma;
    uint32_t log2_poc;
    uint32_t num_short_term;
    int sub_layer_ordering_present;
    int i;

    if (!bit_reader_init(&reader, rbsp, length))
        return -1;

    bit_reader_u(&reader, 4);
    max_sub_layers = bit_reader_u(&reader, 3);
    bit_reader_u(&reader, 1);
    skip_ptl(&reader, max_sub_layers);
    bit_reader_ue(&reader);
    chroma = bit_reader_ue(&reader);
    if (chroma == 3)
        bit_reader_u(&reader, 1);
    bit_reader_ue(&reader);
    bit_reader_ue(&reader);
    if (bit_reader_u(&reader, 1)) {
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
    }
    bit_reader_ue(&reader);
    bit_reader_ue(&reader);
    log2_poc = bit_reader_ue(&reader);
    sub_layer_ordering_present = bit_reader_u(&reader, 1);
    for (i = sub_layer_ordering_present ? 0 : max_sub_layers; i <= max_sub_layers; ++i) {
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
    }
    for (i = 0; i < 6; ++i)
        bit_reader_ue(&reader);
    if (bit_reader_u(&reader, 1) && bit_reader_u(&reader, 1))
        skip_scaling(&reader);
    bit_reader_u(&reader, 1);
    bit_reader_u(&reader, 1);
    if (bit_reader_u(&reader, 1)) {
        bit_reader_u(&reader, 4);
        bit_reader_u(&reader, 4);
        bit_reader_ue(&reader);
        bit_reader_ue(&reader);
        bit_reader_u(&reader, 1);
    }
    num_short_term = bit_reader_ue(&reader);
    if (num_short_term > 0)
        skip_strps(&reader, (int)num_short_term);
    if (bit_reader_u(&reader, 1)) {
        uint32_t count = bit_reader_ue(&reader);
        uint32_t index;
        if (count > 32)
            count = 32;
        if (log2_poc > 12)
            goto out;
        for (index = 0; index < count && !bit_reader_exhausted(&reader); ++index) {
            bit_reader_u(&reader, (int)log2_poc + 4);
            bit_reader_u(&reader, 1);
        }
    }
    bit_reader_u(&reader, 1);
    bit_reader_u(&reader, 1);
    if (!bit_reader_u(&reader, 1))
        goto out; /* vui_parameters_present_flag */
    if (bit_reader_u(&reader, 1)) {
        uint32_t aspect_ratio_idc = bit_reader_u(&reader, 8);
        if (aspect_ratio_idc == 255) {
            bit_reader_u(&reader, 16);
            bit_reader_u(&reader, 16);
        }
    }
    if (bit_reader_u(&reader, 1))
        bit_reader_u(&reader, 1);
    if (bit_reader_u(&reader, 1)) {
        bit_reader_u(&reader, 3);
        bit_reader_u(&reader, 1);
        if (bit_reader_u(&reader, 1)) {
            bit_reader_u(&reader, 8);
            result = (int)bit_reader_u(&reader, 8);
            bit_reader_u(&reader, 8);
        }
    }

out:
    bit_reader_destroy(&reader);
    return result;
}

/* bit0=mastering display (137), bit1=content light (144). */
static int sei_flags(const uint8_t *rbsp, size_t length, int *alternative_transfer)
{
    uint8_t *data;
    size_t input;
    size_t output = 0;
    size_t index = 0;
    int zeros = 0;
    int flags = 0;

    *alternative_transfer = -1;
    data = malloc(length ? length : 1);
    if (!data)
        return 0;

    for (input = 0; input < length; ++input) {
        uint8_t byte = rbsp[input];
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;
        }
        data[output++] = byte;
        zeros = byte == 0x00 ? zeros + 1 : 0;
    }

    while (index < output) {
        int type = 0;
        int payload_size = 0;
        while (index < output && data[index] == 0xff) {
            type += 255;
            ++index;
        }
        if (index < output)
            type += data[index++];
        while (index < output && data[index] == 0xff) {
            payload_size += 255;
            ++index;
        }
        if (index < output)
            payload_size += data[index++];

        if (type == 137)
            flags |= 1;
        else if (type == 144)
            flags |= 2;
        else if (type == 147 && payload_size >= 1 && index < output)
            *alternative_transfer = data[index];

        if ((size_t)payload_size > output - index)
            break;
        index += (size_t)payload_size;
        if (type == 0 && payload_size == 0)
            break;
    }

    free(data);
    return flags;
}

int hevc_hdr_classify(const uint8_t *buffer, size_t length, int *saw_sps)
{
    int best_transfer = -1;
    int alternative_transfer = -1;
    int all_sei_flags = 0;
    int got_sps = 0;
    size_t i = 0;

    if (saw_sps)
        *saw_sps = 0;
    if (!buffer || length < 5)
        return HEVC_HDR_SDR;

    while (i + 4 < length) {
        if (buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 1) {
            size_t nal_start = i + 3;
            int nal_type = (buffer[nal_start] >> 1) & 0x3f;
            size_t payload = nal_start + 2;
            size_t next = payload;
            while (next + 2 < length &&
                   !(buffer[next] == 0 && buffer[next + 1] == 0 && buffer[next + 2] == 1))
                ++next;
            if (next + 2 >= length)
                next = length; /* the last NAL unit runs to the end */
            if (next > payload) {
                size_t payload_length = next - payload;
                if (nal_type == 33) {
                    int transfer;
                    got_sps = 1;
                    transfer = sps_transfer(buffer + payload, payload_length);
                    if (transfer >= 0)
                        best_transfer = transfer;
                } else if (nal_type == 39 || nal_type == 40) {
                    int transfer = -1;
                    all_sei_flags |= sei_flags(buffer + payload, payload_length, &transfer);
                    if (transfer >= 0)
                        alternative_transfer = transfer;
                }
            }
            i = nal_start;
        } else {
            ++i;
        }
    }

    if (saw_sps)
        *saw_sps = got_sps;
    if (alternative_transfer >= 0)
        best_transfer = alternative_transfer;
    if (best_transfer == 16 || (all_sei_flags & 1))
        return HEVC_HDR_HDR10;
    if (best_transfer == 18)
        return HEVC_HDR_HLG;
    if (best_transfer == 14 || best_transfer == 15)
        return HEVC_HDR_GENERIC;
    return HEVC_HDR_SDR;
}

static int detector_reserve(HevcHdrDetector *detector, size_t extra)
{
    size_t required;
    size_t capacity;
    uint8_t *buffer;

    if (!detector || extra > HEVC_HDR_MAX_ES_BYTES - detector->size)
        return 0;
    required = detector->size + extra;
    if (required <= detector->capacity)
        return 1;

    capacity = detector->capacity ? detector->capacity : HEVC_HDR_INITIAL_CAP;
    while (capacity < required) {
        if (capacity >= HEVC_HDR_MAX_ES_BYTES / 2U) {
            capacity = HEVC_HDR_MAX_ES_BYTES;
            break;
        }
        capacity *= 2U;
    }
    buffer = realloc(detector->buffer, capacity);
    if (!buffer)
        return 0;
    detector->buffer = buffer;
    detector->capacity = capacity;
    return 1;
}

static int detector_append(HevcHdrDetector *detector, const uint8_t *data, size_t length)
{
    size_t remaining;

    if (!length)
        return 1;
    if (!detector || !data || detector->size >= HEVC_HDR_MAX_ES_BYTES)
        return 0;

    remaining = HEVC_HDR_MAX_ES_BYTES - detector->size;
    if (length > remaining)
        length = remaining;
    if (!detector_reserve(detector, length))
        return 0;
    memcpy(detector->buffer + detector->size, data, length);
    detector->size += length;
    return 1;
}

static int detector_append_nal(HevcHdrDetector *detector, const uint8_t *nal, size_t length)
{
    static const uint8_t start_code[4] = {0, 0, 0, 1};
    return detector_append(detector, start_code, sizeof(start_code)) &&
           detector_append(detector, nal, length);
}

/* The result is final: the collected stream is not needed any more. */
static int detector_finish(HevcHdrDetector *detector, int result)
{
    free(detector->buffer);
    detector->buffer = NULL;
    detector->size = 0;
    detector->capacity = 0;
    detector->last_classify = 0;
    detector->result = result;
    detector->done = 1;
    return 1;
}

static int detector_evaluate(HevcHdrDetector *detector, int force)
{
    int saw_sps = 0;
    int result;

    if (!detector || detector->done || !detector->size)
        return 0;
    if (!force && detector->size < HEVC_HDR_MAX_ES_BYTES &&
        detector->size - detector->last_classify < HEVC_HDR_CLASSIFY_STEP)
        return 0;

    /* only the bytes added since the last pass: the earlier ones were SDR so
     * far, and every append is whole NAL units, so this is a NAL boundary */
    result = hevc_hdr_classify(detector->buffer + detector->last_classify,
                               detector->size - detector->last_classify, &saw_sps);
    detector->last_classify = detector->size;
    if (result != HEVC_HDR_SDR)
        return detector_finish(detector, result);
    if (saw_sps && !detector->first_sps_at)
        detector->first_sps_at = detector->size;
    if (detector->first_sps_at &&
        detector->size - detector->first_sps_at >= HEVC_HDR_SDR_COMMIT)
        return detector_finish(detector, HEVC_HDR_SDR);
    if (detector->size >= HEVC_HDR_MAX_ES_BYTES)
        return detector_finish(detector, HEVC_HDR_SDR);
    return 0;
}

static int is_annex_b(const uint8_t *data, size_t length)
{
    return data && ((length >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
                    (length >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1));
}

void hevc_hdr_detector_init(HevcHdrDetector *detector)
{
    if (!detector)
        return;
    memset(detector, 0, sizeof(*detector));
    detector->nal_length_size = 4;
    detector->result = -1;
}

void hevc_hdr_detector_reset(HevcHdrDetector *detector)
{
    if (!detector)
        return;
    free(detector->buffer);
    hevc_hdr_detector_init(detector);
}

void hevc_hdr_detector_destroy(HevcHdrDetector *detector)
{
    hevc_hdr_detector_reset(detector);
}

int hevc_hdr_detector_configure(HevcHdrDetector *detector,
                                const uint8_t *extradata,
                                size_t extradata_size)
{
    if (!detector || !extradata || !extradata_size || detector->done)
        return 0;

    if (is_annex_b(extradata, extradata_size)) {
        if (!detector_append(detector, extradata, extradata_size))
            return 0;
        return detector_evaluate(detector, 1);
    }

    /* ISO/IEC 14496-15 HEVCDecoderConfigurationRecord (hvcC). */
    if (extradata_size >= 23 && extradata[0] == 1) {
        size_t offset = 23;
        unsigned int array_count = extradata[22];
        unsigned int array_index;
        detector->nal_length_size = (extradata[21] & 3) + 1;
        detector->length_prefixed = 1;

        for (array_index = 0; array_index < array_count && offset + 3 <= extradata_size; ++array_index) {
            unsigned int nal_count;
            unsigned int nal_index;
            ++offset; /* array_completeness/reserved/NAL_unit_type */
            nal_count = ((unsigned int)extradata[offset] << 8) | extradata[offset + 1];
            offset += 2;
            for (nal_index = 0; nal_index < nal_count && offset + 2 <= extradata_size; ++nal_index) {
                size_t nal_length = ((size_t)extradata[offset] << 8) | extradata[offset + 1];
                offset += 2;
                if (nal_length > extradata_size - offset)
                    return detector_evaluate(detector, 1);
                if (!detector_append_nal(detector, extradata + offset, nal_length))
                    return 0;
                offset += nal_length;
            }
        }
        return detector_evaluate(detector, 1);
    }
    return 0;
}

static int packet_is_length_prefixed(const uint8_t *packet, size_t packet_size, int length_size)
{
    size_t offset = 0;
    int parsed_any = 0;

    if (length_size < 1 || length_size > 4)
        return 0;
    while (offset + (size_t)length_size <= packet_size) {
        size_t nal_length = 0;
        int byte;
        for (byte = 0; byte < length_size; ++byte)
            nal_length = (nal_length << 8) | packet[offset + (size_t)byte];
        offset += (size_t)length_size;
        if (!nal_length || nal_length > packet_size - offset)
            return 0;
        parsed_any = 1;
        offset += nal_length;
    }
    return parsed_any && offset == packet_size;
}

static int detector_append_length_prefixed(HevcHdrDetector *detector,
                                           const uint8_t *packet,
                                           size_t packet_size,
                                           int length_size)
{
    size_t offset = 0;

    if (!packet_is_length_prefixed(packet, packet_size, length_size))
        return 0;
    while (offset + (size_t)length_size <= packet_size) {
        size_t nal_length = 0;
        int byte;
        for (byte = 0; byte < length_size; ++byte)
            nal_length = (nal_length << 8) | packet[offset + (size_t)byte];
        offset += (size_t)length_size;
        if (!detector_append_nal(detector, packet + offset, nal_length))
            return 0;
        offset += nal_length;
    }
    return 1;
}

int hevc_hdr_detector_feed(HevcHdrDetector *detector,
                           const uint8_t *packet,
                           size_t packet_size)
{
    int length_size;

    if (!detector || detector->done || !packet || !packet_size)
        return 0;

    length_size = detector->nal_length_size;
    if (length_size < 1 || length_size > 4)
        length_size = 4;

    if (detector->length_prefixed &&
        packet_is_length_prefixed(packet, packet_size, length_size)) {
        if (!detector_append_length_prefixed(detector, packet, packet_size, length_size))
            return 0;
    } else if (is_annex_b(packet, packet_size)) {
        if (!detector_append(detector, packet, packet_size))
            return 0;
    } else if (!detector_append_length_prefixed(detector, packet, packet_size, length_size)) {
        return 0;
    }
    return detector_evaluate(detector, 0);
}

int hevc_hdr_detector_result(const HevcHdrDetector *detector)
{
    return detector ? detector->result : -1;
}
