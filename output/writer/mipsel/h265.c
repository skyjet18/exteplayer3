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
static int                     resetPending = 0;
/* the extradata the parameter sets were last taken from */
static uint8_t                 *last_private_data;
static unsigned int            last_private_size;
/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

static int32_t PreparCodecData(unsigned char *data, unsigned int cd_len, unsigned int *NalLength)
{
    h265_printf(10, "H265 check codec data..!\n");
    int32_t ret = -100;

    if (data)
    {
        unsigned char tmp[4096];
        AnnexBSets_t sets = {.buf = tmp, .size = sizeof(tmp)};
        unsigned int tmp_len = 0;

        h265_printf(10, "H265 have codec data..!\n");

        if (cd_len > 3 && (data[0] || data[1] || data[2] > 1))
        {
            if (cd_len > 22)
            {
                int i;
                if (data[0] != 0) 
                {
                    h265_printf(10, "Unsupported extra data version %d, decoding may fail\n", (int)data[0]);
                }
                
                *NalLength = (data[21] & 3) + 1;
                int num_param_sets = data[22];
                int pos = 23;
                for (i = 0; i < num_param_sets; i++)
                {
                    int j;
                    if (pos + 3 > cd_len) 
                    {
                        h265_printf(10, "Buffer underrun in extra header (%d >= %u)\n", pos + 3, cd_len);
                        break;
                    }
                    // ignore flags + NAL type (1 byte)
                    int nal_type = data[pos] & 0x3f;
                    int nal_count = data[pos + 1] << 8 | data[pos + 2];
                    pos += 3;
                    for (j = 0; j < nal_count; j++)
                    {
                        /* a cut array: the arrays after it cannot be found */
                        if (pos + 2 > cd_len)
                        {
                            h265_printf(10, "Buffer underrun in extra nal header (%d >= %u)\n", pos + 2, cd_len);
                            pos = cd_len;
                            break;
                        }
                        int nal_size = data[pos] << 8 | data[pos + 1];
                        pos += 2;
                        if (pos + nal_size > cd_len)
                        {
                            h265_printf(10, "Buffer underrun in extra nal (%d >= %u)\n", pos + 2 + nal_size, cd_len);
                            pos = cd_len;
                            break;
                        }
                        /* use only VPS, SPS, PPS nals, each once and without
                         * trailing zeros, see AnnexBSetsAppend() */
                        if ((nal_type == 0x20 || nal_type == 0x21 || nal_type == 0x22) &&
                            AnnexBSetsAppend(&sets, data + pos, nal_size) < 0)
                        {
                            h264_err("Ignoring nal as tmp buffer is too small tmp_len + nal = %u\n", sets.len + 4 + nal_size);
                        }
                        pos += nal_size;
                    }
                }
                tmp_len = sets.len;

                /* no VPS, SPS or PPS (they come in-band): empty codec data,
                 * not a malloc(0) that may give NULL and drop the frame */
                CodecData = malloc(tmp_len ? tmp_len : 1);
                if (!CodecData)
                {
                    h264_err("allocating codec data failed\n");
                    return ret;
                }
                memcpy(CodecData, tmp, tmp_len);
                CodecDataLen = tmp_len;
            }
        }
    }
    else
    {
        *NalLength = 0;
    }
    
    return ret;
}

/* An Annex B start code at the start of the data: 00 00 01 or 00 00 00 01. */
static int HasStartCode(const uint8_t *data, unsigned int size)
{
    return (size > 2 && !data[0] && !data[1] && data[2] == 1) ||
           (size > 3 && !data[0] && !data[1] && !data[2] && data[3] == 1);
}

/* A VPS, SPS or PPS behind one of the start codes of an Annex B packet. */
static int HasParameterSets(const uint8_t *data, unsigned int size)
{
    unsigned int i;
    for (i = 0; i + 3 < size; i++)
    {
        if (!data[i] && !data[i + 1] && data[i + 2] == 1)
        {
            int type = (data[i + 3] >> 1) & 0x3f;
            if (type == 32 || type == 33 || type == 34)
            {
                return 1;
            }
        }
    }
    return 0;
}

/* reset() is called from the command thread while the demuxer thread can be
 * inside writeData(): it only leaves a request, the next writeData() takes it. */
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
    int ts;
    struct iovec iov[IOVEC_SIZE];
    h265_printf(20, "\n");

    if (call == NULL)
    {
        h264_err("call data is NULL...\n");
        return 0;
    }

    TimeDelta = call->FrameRate;
    TimeScale = call->FrameScale;
    VideoPts  = call->Pts;
    
    h265_printf(20, "VideoPts %llu - %u %u\n", call->Pts, TimeDelta, TimeScale);

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
        initialHeader = 1;
    }

    /* a size without a buffer is no codec data either */
    if (call->private_data == NULL)
    {
        call->private_size = 0;
    }

    /* New parameter sets without a codec change (another representation of
     * an adaptive stream): send them before the next picture, as for H.264. */
    if (call->private_size &&
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
    }
    
    h265_printf(10, "H265 private_size: %u\n", call->private_size);
    h265_printf(10, "H265 private_data: %X %X %X %X %X %X\n",
        call->private_size > 0 ? call->private_data[0] : 0,
        call->private_size > 1 ? call->private_data[1] : 0,
        call->private_size > 2 ? call->private_data[2] : 0,
        call->private_size > 3 ? call->private_data[3] : 0,
        call->private_size > 4 ? call->private_data[4] : 0,
        call->private_size > 5 ? call->private_data[5] : 0 );

    h265_printf(10, "H265 len: %u\n", call->len);
    h265_printf(10, "H265 data: %X %X %X %X %X %X\n",
        call->len > 0 ? call->data[0] : 0,
        call->len > 1 ? call->data[1] : 0,
        call->len > 2 ? call->data[2] : 0,
        call->len > 3 ? call->data[3] : 0,
        call->len > 4 ? call->data[4] : 0,
        call->len > 5 ? call->data[5] : 0 );

    /* Annex B: from a TS container (InfoFlags), with Annex B extradata, or
     * without any extradata and a start code at the start of the packet
     * (raw .hevc, RTSP, AVI and other containers without the TS flag). */
    ts = (call->InfoFlags & 0x1) &&
         (call->private_size == 0 || HasStartCode(call->private_data, call->private_size));
    if (ts || HasStartCode(call->private_data, call->private_size) ||
        (call->private_size == 0 && CodecData == NULL && HasStartCode(call->data, call->len)))
    {
        h265_printf(10, "H265 simple inject method!\n");
        uint32_t PacketLength = 0;
        uint32_t FakeStartCode = (call->Version << 8) | PES_VERSION_FAKE_START_CODE;
        
        iov[ic++].iov_base = PesHeader;

        iov[ic].iov_base = "";
        iov[ic++].iov_len = 1;

        /* Outside TS the parameter sets can be in the extradata only (the
         * sprop sets of RTSP): send them once, unless the packet has its own. */
        if (!ts && initialHeader && call->private_size &&
            !HasParameterSets(call->data, call->len))
        {
            iov[ic].iov_base  = call->private_data;
            iov[ic++].iov_len = call->private_size;
            PacketLength     += call->private_size;
        }
        initialHeader = 0;

        iov[ic].iov_base  = call->data;
        iov[ic++].iov_len = call->len;
        PacketLength     += call->len;
        
        iov[0].iov_len = InsertPesHeader(PesHeader, -1, MPEG_VIDEO_PES_START_CODE, VideoPts, FakeStartCode);
        
        return call->WriteV(call->fd, iov, ic);
    }

    uint32_t PacketLength = 0;
    
    ic = 0;
    iov[ic++].iov_base = PesHeader;
    
    if (initialHeader)
    {
        if (CodecData)
        {
            free(CodecData);
            CodecData = NULL;
        }
        
        uint8_t  *private_data = call->private_data;
        uint32_t  private_size = call->private_size;
    
        PreparCodecData(private_data, private_size, &NalLengthBytes);
        
        if (CodecData != NULL)
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
                /* the whole access unit or nothing, as in the H.264 writer */
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
            
            iov[ic].iov_base  = Head;
            iov[ic++].iov_len = sizeof(Head);
            PacketLength += sizeof(Head);
            
            iov[ic].iov_base  = call->data + pos;
            iov[ic++].iov_len = pack_len;
            PacketLength     += pack_len;
    
            pos += pack_len;
            
        } while ((pos + NalLengthBytes) < call->len);
        
        h265_printf (10, "<<<< PacketLength [%u]\n", PacketLength);
        iov[0].iov_len = InsertPesHeader(PesHeader, -1, MPEG_VIDEO_PES_START_CODE, VideoPts, 0);
        
        len = call->WriteV(call->fd, iov, ic);
        PacketLength += iov[0].iov_len;
        if (PacketLength != len)
        {
            h264_err("<<<< not all data have been written [%d/%u]\n", len, PacketLength);
        }
    }

    h265_printf (10, "< len %d\n", len);
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
    "h265",
    eVideo,
    "V_HEVC",
    -1,
    STREAMTYPE_MPEG4_H265,
    CT_H265
};

struct Writer_s WriterVideoH265 = {
    &reset,
    &writeData,
    &writeReverseData,
    &caps
};
