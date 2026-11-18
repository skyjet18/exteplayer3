/*
 * linuxdvb output/writer handling.
 *
 * konfetti 2010 based on linuxdvb.c code from libeplayer2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <poll.h>

#include "stm_ioctls.h"
#include "bcm_ioctls.h"

#include "debug.h"
#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"
#include "avcc.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */
#define IOVEC_SIZE 1024

/* ***************************** */
/* Types                         */
/* ***************************** */

/* ***************************** */
/* Varaibles                     */
/* ***************************** */
static unsigned char           Head[] = {0, 0, 0, 1};
static int                     initialHeader = 1;
static unsigned int            NalLengthBytes = 1;
static unsigned char           *CodecData     = NULL; 
static unsigned int            CodecDataLen   = 0;
static int                     avc3 = 0;
static int                     sps_pps_in_stream = 0;
static uint8_t                 *last_private_data;
static unsigned int            last_private_size;
static int                     resetPending = 0;
/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

/* Room for all SPS and PPS of a stream as Annex B NAL units (start code +
 * data each); PreparCodecData() converts the avcC into a buffer of this size. */
#define AVCC_TMP_SIZE 2048

// Please see: https://bugzilla.mozilla.org/show_bug.cgi?id=1105771
static int32_t UpdateExtraData(uint8_t **ppExtraData, uint32_t *pExtraDataSize, uint8_t *pData, uint32_t dataSize)
{
    uint8_t *aExtraData = *ppExtraData;
    
    if (aExtraData[0] != 1 || !pData) {
        // Not AVCC or nothing to update with.
        return -1;
    }
    
    int32_t nalsize = (aExtraData[4] & 3) + 1;
    
    uint8_t sps[AVCC_TMP_SIZE];
    uint32_t spsIdx = 0;
    
    uint32_t numSps = 0;
    
    uint8_t pps[AVCC_TMP_SIZE];
    uint32_t ppsIdx = 0;
    
    uint8_t numPps = 0;

    /* what the collected sets need in PreparCodecData(), where each one
     * gets a 4 byte start code instead of the 2 byte length it has here */
    uint32_t annexbLen = 0;

    // Find SPS and PPS NALUs in AVCC data
    uint8_t *d = pData;
    while (d + nalsize < pData + dataSize)
    {
        /* the NAL length has 1 to 4 bytes, as told by the avcC */
        uint32_t nalLen = 0;
        int32_t k;
        for (k = 0; k < nalsize; k++)
        {
            nalLen = (nalLen << 8) | d[k];
        }
        if (nalLen > (uint32_t)(pData + dataSize - d - nalsize))
        {
            h264_err("NAL length %u beyond the packet\n", nalLen);
            return -1;
        }
        
        /* an empty NAL has no type, the byte belongs to the next length */
        uint8_t nalType = nalLen ? d[nalsize] & 0x1f : 0;
        if (nalType == 7) 
        { /* SPS */
        
            if (annexbLen + 4 + nalLen > AVCC_TMP_SIZE)
            {
                h264_err("SPS no free space to copy...\n");
                return -1;
            }
            annexbLen += 4 + nalLen;

            // 16 bits size
            sps[spsIdx++] = (uint8_t)(0xFF & (nalLen >> 8));
            sps[spsIdx++] = (uint8_t)(0xFF & nalLen);

            memcpy(&(sps[spsIdx]), d + nalsize, nalLen);
            spsIdx += nalLen;
            
            numSps += 1;
            
            h264_printf(10, "SPS len[%u]...\n", nalLen);
        } 
        else if (nalType == 8)
        { /* PPS */

            /* the avcC count of the PPS has 8 bits */
            if (numPps == 255)
            {
                h264_err("too many PPS...\n");
                return -1;
            }

            if (annexbLen + 4 + nalLen > AVCC_TMP_SIZE)
            {
                h264_err("PPS not free space to copy...\n");
                return -1;
            }
            annexbLen += 4 + nalLen;

            // 16 bits size
            pps[ppsIdx++] = (uint8_t)(0xFF & (nalLen >> 8));
            pps[ppsIdx++] = (uint8_t)(0xFF & nalLen);

            memcpy(&(pps[ppsIdx]), d + nalsize, nalLen);
            ppsIdx += nalLen;
            
            numPps += 1;
            
            h264_printf(10, "PPS len[%u]...\n", nalLen);
        }
        d += nalsize + nalLen;
    }
    /* profile and level are taken from the first SPS, the SPS count has 5 bits */
    if (numSps == 0 || numSps > 31 || spsIdx < 6)
    {
        return -1;
    }

    uint32_t idx = 0;
    aExtraData = malloc(7 + spsIdx + ppsIdx);
    if (!aExtraData)
    {
        h264_err("allocating codec data failed\n");
        return -1;
    }
    *ppExtraData = aExtraData;
    aExtraData[idx++] = 0x1;            // version
    aExtraData[idx++] = sps[3];         // profile
    aExtraData[idx++] = sps[4];         // profile compat
    aExtraData[idx++] = sps[5];         // level
    aExtraData[idx++] = 0xfc | (nalsize - 1); // nal size - 1
    
    aExtraData[idx++] = 0xe0 | numSps;
    if (numSps)
    {
        memcpy(&(aExtraData[idx]), sps, spsIdx);
        idx += spsIdx;
    }
    
    aExtraData[idx++] = numPps;
    if (numPps)
    {
        memcpy(&(aExtraData[idx]), pps, ppsIdx);
        idx += ppsIdx;
    }
    
    h264_printf(10, "aExtraData len[%u]...\n", idx);
    *pExtraDataSize = idx;
    return 0;
}

/* The level limit of 4.1 was written for MIPS Broadcom decoders and gave
 * artefacts at 1080p50/60 on ARM boxes: only MIPS builds apply it.
 * EP3_H264_LEVEL41=1 forces it on, =0 off, on any CPU. A build can set its
 * own default with -DH264_LEVEL41_DEFAULT=0|1. */
#ifndef H264_LEVEL41_DEFAULT
#if defined(__mips__) || defined(__mips)
#define H264_LEVEL41_DEFAULT 1
#else
#define H264_LEVEL41_DEFAULT 0
#endif
#endif

static int LimitSpsLevelWanted(void)
{
    static int wanted = -1;
    if (wanted < 0)
    {
        const char *env = getenv("EP3_H264_LEVEL41");
        if (env && env[0])
        {
            wanted = env[0] != '0';
        }
        else
        {
            wanted = H264_LEVEL41_DEFAULT;
        }
    }
    return wanted;
}

/* Log the level of a SPS from avcC and limit it to 4.1 where wanted, see
 * LimitSpsLevelWanted(). */
static void LimitSpsLevel(unsigned char *sps, unsigned int len)
{
    static const uint8_t profile_num[] = { 66, 77, 88, 100 };
    static const char *const profile_str[] = { "baseline", "main", "extended", "high" };
    unsigned int i;

    if (len <= 3 || sps[0] != 0x67)
    {
        return;
    }
    for (i = 0; i < 4; ++i)
    {
        if (sps[1] == profile_num[i])
        {
            uint8_t level_org = sps[3];
            if (level_org > 0x29 && LimitSpsLevelWanted())
            {
                h264_printf(10, "H264 %s profile@%d.%d patched down to 4.1!", profile_str[i], level_org / 10 , level_org % 10);
                sps[3] = 0x29; // level 4.1
            }
            else
            {
                h264_printf(10, "H264 %s profile@%d.%d", profile_str[i], level_org / 10 , level_org % 10);
            }
            return;
        }
    }
}

static int32_t PreparCodecData(unsigned char *data, unsigned int cd_len, unsigned int *NalLength)
{
    unsigned char tmp[AVCC_TMP_SIZE];
    AnnexBSets_t sets = {.buf = tmp, .size = sizeof(tmp)};
    unsigned int num_sps = 0;
    unsigned int num_pps = 0;
    unsigned int i;
    int length_size;

    h264_printf(10, "H264 check codec data..!\n");
    if (!data)
    {
        *NalLength = 0;
        return -100;
    }
    h264_printf(10, "H264 have codec data..!\n");
    if (cd_len <= 7)
    {
        h264_printf(10, "codec_data too short(1)");
        return -1;
    }
    if (data[0] != 1)
    {
        h264_printf(10, "wrong avcC version %d!", data[0]);
        return -100;
    }

    /* Pass on all SPS and all PPS from avcC, not only the first ones.
     * Some live streams use a second PPS (e.g. weighted bi-prediction
     * for B slices) and never repeat it in-band. Each set goes once and
     * without trailing zeros: replay clips have SPS and PPS twice in
     * the avcC, one PPS with a zero byte at its end, and the decoders
     * of the boxes showed no picture with them. */
    length_size = AvccToAnnexB(data, cd_len, &sets, &num_sps, &num_pps);
    if (!length_size)
    {
        h264_printf(10, "codec_data without usable SPS/PPS (%u SPS, %u PPS)", num_sps, num_pps);
        return -2;
    }
    for (i = 0; i < sets.count; i++)
    {
        if ((tmp[sets.at[i]] & 0x1f) == 7)
        {
            LimitSpsLevel(tmp + sets.at[i], sets.length[i]);
        }
    }

    CodecData = malloc(sets.len);
    if (!CodecData)
    {
        h264_err("allocating codec data failed\n");
        return -1;
    }
    memcpy(CodecData, tmp, sets.len);
    CodecDataLen = sets.len;
    *NalLength = length_size;
    h264_printf(10, "H264 codec data: %u SPS, %u PPS, %u bytes", num_sps, num_pps, sets.len);
    return 0;
}

/* reset() is called from the command thread while the demuxer thread can be
 * inside writeData(), blocked in the write with CodecData in its iovec. The
 * state and the buffers therefore belong to writeData() alone: reset() only
 * leaves a request and the next writeData() starts over. */
static int reset()
{
    __atomic_store_n(&resetPending, 1, __ATOMIC_RELEASE);
    return 0;
}

static int writeData(void* _call)
{
    WriterAVCallData_t* call = (WriterAVCallData_t*) _call;

    unsigned char           PesHeader[PES_MAX_HEADER_SIZE];
    unsigned long long int  VideoPts;
    unsigned int            TimeDelta;
    unsigned int            TimeScale;
    int                     len = 0;
    int ic = 0;
    struct iovec iov[IOVEC_SIZE];
    h264_printf(20, "\n");

    if (call == NULL)
    {
        h264_err("call data is NULL...\n");
        return 0;
    }

    TimeDelta = call->FrameRate;
    TimeScale = call->FrameScale;
    VideoPts  = call->Pts;
    
    h264_printf(20, "VideoPts %lld - %d %d\n", call->Pts, TimeDelta, TimeScale);

    if ((call->data == NULL) || (call->len <= 0))
    {
        h264_err("NULL Data. ignoring...\n");
        return 0;
    }

    if (call->fd < 0)
    {
        h264_err("file pointer < 0. ignoring ...\n");
        return 0;
    }

    if (__atomic_exchange_n(&resetPending, 0, __ATOMIC_ACQ_REL))
    {
        /* CodecData is rebuilt below, last_private_data stays valid */
        initialHeader = 1;
        avc3 = 0;
        sps_pps_in_stream = 0;
    }

    if (call->private_data && call->private_size &&
        (last_private_size != call->private_size ||
         memcmp(last_private_data, call->private_data, call->private_size)))
    {
        uint8_t *copy = malloc(call->private_size);
        if (!copy)
        {
            h264_err("allocating codec data failed\n");
            return -1;
        }
        memcpy(copy, call->private_data, call->private_size);
        free(last_private_data);
        last_private_data = copy;
        last_private_size = call->private_size;
        initialHeader = 1;
        avc3 = 0;
        sps_pps_in_stream = 0;
    }

    /* AnnexA */
    if( !avc3 && ((1 < call->private_size && 0 == call->private_data[0]) || 
        (call->len > 3) && ((call->data[0] == 0x00 && call->data[1] == 0x00 && call->data[2] == 0x00 && call->data[3] == 0x01) ||
        (call->data[0] == 0xff && call->data[1] == 0xff && call->data[2] == 0xff && call->data[3] == 0xff))))
    {
        uint32_t i = 0;
        uint8_t InsertPrivData = !sps_pps_in_stream;
        uint32_t PacketLength = 0;
        uint32_t FakeStartCode = (call->Version << 8) | PES_VERSION_FAKE_START_CODE;
        iov[ic++].iov_base = PesHeader;
        
        while (InsertPrivData && i < 36 && (call->len - i) > 5)
        {
            if ( (call->data[i] == 0x00 && call->data[i+1] == 0x00 && call->data[i+2] == 0x00 && call->data[i+3] == 0x01 && (call->data[i+4] == 0x67 || call->data[i+4] == 0x68)) )
            {
                InsertPrivData = 0;
                sps_pps_in_stream = 1;
            }
            i += 1;
        }
        
        if (InsertPrivData && call->private_data && call->private_size > 0 && initialHeader)
        {
            initialHeader = 0;
            iov[ic].iov_base  = call->private_data;
            iov[ic++].iov_len = call->private_size;
            PacketLength     += call->private_size;
        }
        
        iov[ic].iov_base  = call->data;
        iov[ic++].iov_len = call->len;
        PacketLength     += call->len;
        
#ifdef HAVE_DREAMNEXTGEN
        /* The AML DVB-API video device takes raw Annex-B ES, without PES. */
        iov[0].iov_len = 0;
#else
        iov[0].iov_len = InsertPesHeaderWithDts(PesHeader, -1, MPEG_VIDEO_PES_START_CODE, VideoPts, call->Dts, FakeStartCode);
#endif
        
        return call->WriteV(call->fd, iov, ic);
    }
    else if (!call->private_data || call->private_size < 7 || 1 != call->private_data[0])
    {
        h264_err("No valid private data available! [%d]\n", (int)call->private_size);
        return 0;
    }

    uint32_t PacketLength = 0;
    
    ic = 0;
    iov[ic++].iov_base = PesHeader;
    
    if (!avc3 && initialHeader)
    {
        if (CodecData)
        {
            free(CodecData);
            CodecData = NULL;
        }
        
        uint8_t  *private_data = call->private_data;
        uint32_t  private_size = call->private_size;
    
        if ( PreparCodecData(private_data, private_size, &NalLengthBytes))
        {
            UpdateExtraData(&private_data, &private_size, call->data, call->len);
            PreparCodecData(private_data, private_size, &NalLengthBytes);
        }
        
        if (private_data != call->private_data)
        {
            /* the packet had no parameter sets: look again in the next one */
            if (CodecData != NULL)
            {
                avc3 = 1;
            }
            free(private_data);
            private_data = NULL;
        }
        
        /* stream has its own SPS/PPS: do not prepend avcC ones, they can differ
         * (re-cut TV recordings) and break the picture */
        if (!sps_pps_in_stream && NalLengthBytes >= 1 && NalLengthBytes <= 4)
        {
            uint32_t p = 0;
            while (p + NalLengthBytes < (uint32_t)call->len)
            {
                uint32_t nl = 0, k;
                for (k = 0; k < NalLengthBytes; k++) nl = (nl << 8) | call->data[p + k];
                /* an empty NAL has no type, the byte belongs to the next length */
                if (nl == 0) { p += NalLengthBytes; continue; }
                uint8_t t = call->data[p + NalLengthBytes] & 0x1f;
                if (t == 7 || t == 8) { sps_pps_in_stream = 1; break; }
                /* corrupt NAL length */
                if (nl > (uint32_t)call->len - p - NalLengthBytes) break;
                p += NalLengthBytes + nl;
            }
        }

        if (CodecData != NULL && !sps_pps_in_stream)
        {
            iov[ic].iov_base  = CodecData;
            iov[ic++].iov_len = CodecDataLen;
            PacketLength     += CodecDataLen;
            initialHeader = 0;
        }
    }

    if (CodecData != NULL)
    {
        uint32_t pos = 0;
        do
        {
            if (ic + 2 > IOVEC_SIZE)
            {
                h264_err(">> Drop data due to ic overflow\n");
                return -1;
            }
            
            uint32_t pack_len = 0;
            uint32_t i = 0;
            if (call->len - pos < NalLengthBytes)
            {
                h264_err("packet too short for a NAL length [%u]\n", call->len);
                break;
            }
            for (i = 0; i < NalLengthBytes; i++, pos++)
            {
                pack_len <<= 8;
                pack_len += call->data[pos];
            }
            
            if ( pack_len > call->len - pos )
            {
                pack_len = call->len - pos;
            }

            /* parameter sets of the stream without trailing zeros, see
             * PreparCodecData() */
            uint32_t nal_len = pack_len;
            if (pack_len && ((call->data[pos] & 0x1f) == 7 || (call->data[pos] & 0x1f) == 8))
            {
                nal_len = NalTrim(call->data + pos, pack_len);
            }
            /* an empty NAL would be a start code without a NAL unit */
            if (!nal_len)
            {
                pos += pack_len;
                continue;
            }
            
            iov[ic].iov_base  = Head;
            iov[ic++].iov_len = sizeof(Head);
            PacketLength += sizeof(Head);
            
            iov[ic].iov_base  = call->data + pos;
            iov[ic++].iov_len = nal_len;
            PacketLength     += nal_len;
    
            pos += pack_len;
            
        } while ((pos + NalLengthBytes) < call->len);
        
        h264_printf (10, "<<<< PacketLength [%d]\n", PacketLength);
#ifdef HAVE_DREAMNEXTGEN
        /* The AML DVB-API video device takes raw Annex-B ES, without PES. */
        iov[0].iov_len = 0;
#else
        iov[0].iov_len = InsertPesHeaderWithDts(PesHeader, -1, MPEG_VIDEO_PES_START_CODE, VideoPts, call->Dts, 0);
#endif
        
        len = call->WriteV(call->fd, iov, ic);
        PacketLength += iov[0].iov_len;
        if (PacketLength != len)
        {
            h264_err("<<<< not all data have been written [%d/%d]\n", len, PacketLength);
        }
    }

    h264_printf (10, "< len %d\n", len);
    return len;
}

static int writeReverseData(void* _call)
{
    WriterAVCallData_t* call = (WriterAVCallData_t*) _call;
    
    return 0;
}
/* ***************************** */
/* Writer  Definition            */
/* ***************************** */

static WriterCaps_t caps = {
    "h264",
    eVideo,
    "V_MPEG4/ISO/AVC",
    VIDEO_ENCODING_H264,
    STREAMTYPE_MPEG4_H264,
    CT_H264
};

struct Writer_s WriterVideoH264 = {
    &reset,
    &writeData,
    &writeReverseData,
    &caps
};
