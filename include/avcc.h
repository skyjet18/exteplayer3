/*
 * Parameter sets of the codec data of H.264 (avcC) and HEVC (hvcC) as
 * Annex B NAL units, for the video writers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef AVCC_H_
#define AVCC_H_

#include <stdint.h>
#include <string.h>

/* 31 SPS and 255 PPS fit an avcC */
#define ANNEXB_SETS_MAX 288

/* Parameter sets with a start code each, every one only once. */
typedef struct
{
    uint8_t      *buf;
    unsigned int  size;                       /* room in buf */
    unsigned int  len;                        /* bytes in buf */
    unsigned int  count;                      /* NAL units in buf */
    unsigned int  at[ANNEXB_SETS_MAX];        /* offset of each NAL unit, behind its start code */
    unsigned int  length[ANNEXB_SETS_MAX];
} AnnexBSets_t;

/* The last byte of a NAL unit is never 0 (H.264 7.4.1, H.265 7.4.2): zeros
 * at the end of a parameter set are trailing_zero_8bits of a byte stream
 * that the muxer counted into its length. A decoder that takes them for
 * more RBSP data reads fields the set does not have. */
static inline unsigned int NalTrim(const uint8_t *nal, unsigned int len)
{
    while (len && !nal[len - 1])
    {
        len--;
    }
    return len;
}

/* Appends a NAL unit with a start code, without its trailing zeros and not
 * when the same unit is there already. Returns 1 when appended, 0 for an
 * empty unit or a repeat, -1 when there is no room. */
static inline int AnnexBSetsAppend(AnnexBSets_t *sets, const uint8_t *nal, unsigned int len)
{
    unsigned int i;

    len = NalTrim(nal, len);
    if (!len)
    {
        return 0;
    }
    for (i = 0; i < sets->count; i++)
    {
        if (sets->length[i] == len && !memcmp(sets->buf + sets->at[i], nal, len))
        {
            return 0;
        }
    }
    if (sets->count == ANNEXB_SETS_MAX || sets->size - sets->len < 4 || sets->size - sets->len - 4 < len)
    {
        return -1;
    }
    memcpy(sets->buf + sets->len, "\0\0\0\1", 4);
    sets->len += 4;
    memcpy(sets->buf + sets->len, nal, len);
    sets->at[sets->count] = sets->len;
    sets->length[sets->count] = len;
    sets->count++;
    sets->len += len;
    return 1;
}

/* A parameter set of NAL type "type" with a 16 bit length at "pos" that
 * ends within the avcC. */
static inline int AvccSetAt(const uint8_t *data, unsigned int size, unsigned int pos, int type)
{
    unsigned int len;

    if (pos > size || size - pos < 3)
    {
        return 0;
    }
    len = (data[pos] << 8) | data[pos + 1];
    return len && len <= size - pos - 2 && (data[pos + 2] & 0x1f) == type;
}

/* Takes "count" parameter sets of NAL type "type" (7 SPS, 8 PPS) from the
 * avcC at *pos. A length of 0 is an empty set when a set follows it, else
 * a stray zero byte in front of the next length: both are skipped. A set
 * that runs past the avcC or has another type ends the list, the sets
 * before it stay. Returns 0 when all sets were read, else -1 with *pos at
 * the end of the avcC. */
static inline int AvccSets(const uint8_t *data, unsigned int size, unsigned int *pos,
                           unsigned int count, int type, AnnexBSets_t *sets)
{
    unsigned int n = 0;

    while (n < count && *pos <= size && size - *pos >= 2)
    {
        unsigned int at = *pos;
        unsigned int len = (data[at] << 8) | data[at + 1];

        if (!len)
        {
            *pos = at + (AvccSetAt(data, size, at + 2, type) ? 2 : 1);
            continue;
        }
        if (!AvccSetAt(data, size, at, type))
        {
            break;
        }
        *pos = at + 2 + len;
        n++;
        if (AnnexBSetsAppend(sets, data + at + 2, len) < 0)
        {
            break;
        }
    }
    if (n < count)
    {
        *pos = size;
        return -1;
    }
    return 0;
}

/* The SPS and PPS of an avcC (ISO/IEC 14496-15 5.3.3.1) as Annex B, each
 * set once and without trailing zeros, the lengths checked against the
 * size of the avcC. The counts of the sets taken go to *sps and *pps.
 * Returns the size of the NAL length fields of the samples (1 to 4), or 0
 * when the avcC has no SPS or no PPS that can be used. */
static inline int AvccToAnnexB(const uint8_t *data, unsigned int size, AnnexBSets_t *sets,
                               unsigned int *sps, unsigned int *pps)
{
    unsigned int pos = 6;
    unsigned int before = sets->count;

    *sps = *pps = 0;
    if (!data || size < 7 || data[0] != 1)
    {
        return 0;
    }
    AvccSets(data, size, &pos, data[5] & 0x1f, 7, sets);
    *sps = sets->count - before;
    /* zero bytes where the PPS count belongs: stray bytes in front of it */
    while (pos < size && size - pos > 1 && !data[pos])
    {
        pos++;
    }
    if (pos < size)
    {
        unsigned int count = data[pos++];
        AvccSets(data, size, &pos, count, 8, sets);
    }
    *pps = sets->count - before - *sps;
    if (!*sps || !*pps)
    {
        return 0;
    }
    return (data[4] & 0x03) + 1;
}

#endif
