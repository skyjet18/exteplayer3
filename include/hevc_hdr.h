/*
 * HEVC HDR classifier interface for exteplayer3.
 * GPL-2.0-or-later.
 */
#ifndef HEVC_HDR_H_
#define HEVC_HDR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HEVC_HDR_SDR = 0,
    HEVC_HDR_HDR10 = 1,
    HEVC_HDR_HLG = 2,
    HEVC_HDR_GENERIC = 3
};

/* Classify an Annex-B HEVC elementary stream buffer. */
int hevc_hdr_classify(const uint8_t *buffer, size_t length, int *saw_sps);

typedef struct HevcHdrDetector_s {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
    size_t last_classify;
    size_t first_sps_at;
    int nal_length_size;
    int length_prefixed;
    int result; /* -1 while unknown, otherwise HEVC_HDR_* */
    int done;
} HevcHdrDetector;

void hevc_hdr_detector_init(HevcHdrDetector *detector);
void hevc_hdr_detector_reset(HevcHdrDetector *detector);
void hevc_hdr_detector_destroy(HevcHdrDetector *detector);

/*
 * Configure from AVCodecParameters extradata. Supports Annex-B extradata and
 * HEVCDecoderConfigurationRecord (hvcC). Returns 1 if classification finished.
 */
int hevc_hdr_detector_configure(HevcHdrDetector *detector,
                                const uint8_t *extradata,
                                size_t extradata_size);

/*
 * Feed one selected HEVC AVPacket. Supports Annex-B and hvcC length-prefixed
 * packet data. Returns 1 only when a final result becomes available.
 */
int hevc_hdr_detector_feed(HevcHdrDetector *detector,
                           const uint8_t *packet,
                           size_t packet_size);

int hevc_hdr_detector_result(const HevcHdrDetector *detector);

#ifdef __cplusplus
}
#endif

#endif
