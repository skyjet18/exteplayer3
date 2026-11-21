/* SPDX-License-Identifier: MIT */
#ifndef DTS_FRAME_H_
#define DTS_FRAME_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The drivers of several chipsets (Broadcom: Vu+ and GigaBlue 4K; HiSilicon:
 * SF8008) only handle DTS core frames whose size is a multiple of four bytes.
 * DVD style full rate DTS uses 2013 byte frames, and the box then sends
 * nothing a receiver can decode, or noise.  Blu-ray cores (2012 bytes) are
 * unaffected.
 *
 * A core frame may carry padding after its audio data: the decoder finds the
 * next frame through the FSIZE header field and ignores what is left over.
 * So such a frame can be made acceptable by appending zero bytes and raising
 * FSIZE to match; the decoded audio is bit-identical.  The X96 (DTS 96/24)
 * and XCH (DTS-ES) extensions inside a core frame record their distance to
 * the end of the frame, so that field is raised as well, otherwise a decoder
 * no longer accepts the extension and plays the 5.1 core only.
 *
 * dts_core_aligned_size() and dts_core_align(), and this comment, are
 * dvb_dts_aligned_size() and dvb_dts_align() of dtsframe.h in the
 * dvbaudiosink of gstreamer1.0-plugin-dvbmediasink as patched by
 * oe-alliance-core (0003-dvbaudiosink-vuplus-pad-dts-core-frames.patch and
 * 0004-dvbaudiosink-dts-driver-fixes.patch), renamed, reindented and with
 * DTS_CORE_MAX_FRAME_SIZE for the literal 16384.  That file is under the MIT
 * licence (SPDX identifier, no copyright line), and so is this one.
 */

/* Largest frame dts_core_aligned_size() returns: FSIZE has 14 bits. */
#define DTS_CORE_MAX_FRAME_SIZE 16384

/*
 * Returns the padded size for a buffer that holds exactly one 16 bit big
 * endian core frame needing it, and 0 when the buffer must be left alone:
 * already aligned, not a lone core frame (e.g. core + DTS-HD extension),
 * header CRC present, or no room left in the frame's IEC 61937 burst.
 */
static inline size_t dts_core_aligned_size(const uint8_t *input, size_t size)
{
    size_t frame_size, samples, padded;

    if (!input || size < 16 || !(size & 3))
        return 0;
    if (input[0] != 0x7f || input[1] != 0xfe || input[2] != 0x80 || input[3] != 0x01)
        return 0;
    if (input[4] & 0x02) /* CPF: a header CRC would no longer match */
        return 0;
    samples = ((((size_t)input[4] & 0x01) << 6 | input[5] >> 2) + 1) * 32;
    frame_size = (((size_t)input[5] & 0x03) << 12 | (size_t)input[6] << 4 | input[7] >> 4) + 1;
    if (frame_size != size)
        return 0;
    padded = (size + 3) & ~(size_t)3;
    if (padded > DTS_CORE_MAX_FRAME_SIZE || padded + 8 > samples * 4)
        return 0;
    return padded;
}

/* Copy one core frame to output, padded to the size dts_core_aligned_size() gave. */
static inline void dts_core_align(uint8_t *output, size_t padded, const uint8_t *input, size_t size)
{
    size_t field = padded - 1;

    memcpy(output, input, size);
    memset(output + size, 0, padded - size);
    output[5] = (uint8_t)((output[5] & 0xfc) | ((field >> 12) & 0x03));
    output[6] = (uint8_t)(field >> 4);
    output[7] = (uint8_t)((output[7] & 0x0f) | ((field & 0x0f) << 4));

    if (input[10] & 0x10) /* EXT_AUDIO: an extension follows the core data */
    {
        const unsigned int id = input[10] >> 5; /* EXT_AUDIO_ID: 0 XCH, 2 X96 */
        size_t pos;

        /* Extensions start on a 4 byte boundary.  Only a sync word whose
         * size field matches the distance to the end of the frame is one. */
        for (pos = 16; pos + 8 <= size; pos += 4)
        {
            const uint32_t sync = (uint32_t)input[pos] << 24 | (uint32_t)input[pos + 1] << 16 |
                                  (uint32_t)input[pos + 2] << 8 | input[pos + 3];
            if (id == 2 && sync == UINT32_C(0x1d95f262))
            {
                /* FSIZE96: 12 bits */
                if ((((size_t)input[pos + 4] << 4) | (input[pos + 5] >> 4)) + 1 != size - pos)
                    continue;
                field = padded - pos - 1;
                if (field > 0xfff)
                    break;
                output[pos + 4] = (uint8_t)(field >> 4);
                output[pos + 5] = (uint8_t)((output[pos + 5] & 0x0f) | ((field & 0x0f) << 4));
                break;
            }
            if (id == 0 && sync == UINT32_C(0x5a5a5a5a))
            {
                /* XChFSIZE: 10 bits. Some encoders write the distance
                 * plus one, which decoders take as well (FFmpeg
                 * dca_core.c); the field is raised by the padding then. */
                size_t xch = (((size_t)input[pos + 4] << 2) | (input[pos + 5] >> 6)) + 1;
                if (xch != size - pos && xch - 1 != size - pos)
                    continue;
                field = xch - 1 + (padded - size);
                if (field > 0x3ff)
                    break;
                output[pos + 4] = (uint8_t)(field >> 2);
                output[pos + 5] = (uint8_t)((output[pos + 5] & 0x3f) | ((field & 0x03) << 6));
                break;
            }
        }
    }
}

#endif
