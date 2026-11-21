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

//#include "stm_ioctls.h"
#include "bcm_ioctls.h"

#include "debug.h"
#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */

/* ***************************** */
/* Types                         */
/* ***************************** */

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

static int reset()
{
    return 0;
}

static int writeData(void *_call)
{
    WriterAVCallData_t* call = (WriterAVCallData_t*) _call;

    uint8_t PesHeader[PES_MAX_HEADER_SIZE + 4 + 9];

    amr_printf(10, "\n");

    if (call == NULL) {
        amr_err("call data is NULL...\n");
        return 0;
    }

    if (call->data == NULL || call->len == 0 || call->fd < 0) {
        amr_err("call error wrong data call: %p, data: %p, len: %u, fd: %d\n", call, call->data, call->len, call->fd);
        return 0;
    }
    
    amr_printf(10, "AudioPts %llu\n", call->Pts);

    /* The AMR decoder specific box (size, 'damr', 9 bytes of data) goes in
     * front of each frame behind a 4 byte length that counts the box and the
     * frame, as gstdvbaudiosink does; without it the frames go alone. */
    bool hasCodecData = call->private_data != NULL && call->private_size >= 17;
    if (call->private_data != NULL && !hasCodecData)
    {
        amr_printf(10, "codec data of %u bytes ignored\n", call->private_size);
    }

    uint32_t headerSize = InsertPesHeader(PesHeader, call->len + (hasCodecData ? 4 + 9 : 0),
                                          MPEG_AUDIO_PES_START_CODE, call->Pts, 0);

    if (hasCodecData) {
        size_t payload_len = call->len + 17;
        PesHeader[headerSize++] = (payload_len >> 24) & 0xff;
        PesHeader[headerSize++] = (payload_len >> 16) & 0xff;
        PesHeader[headerSize++] = (payload_len >> 8)  & 0xff;
        PesHeader[headerSize++] = payload_len & 0xff;
        memcpy(&PesHeader[headerSize], call->private_data + 8, 9);
        headerSize += 9;
    }

    struct iovec iov[2];
    iov[0].iov_base = PesHeader;
    iov[0].iov_len = headerSize;
    iov[1].iov_base = call->data;
    iov[1].iov_len = call->len;
    
    int len = call->WriteV(call->fd, iov, 2);

    amr_printf(10, "amr_Write-< len=%d\n", len);
    return len;
}

/* ***************************** */
/* Writer  Definition            */
/* ***************************** */

static WriterCaps_t caps_amr = {
    "amr",
    eAudio,
    "A_AMR",
    -1,
    AUDIOTYPE_AMR,
    -1
};

struct Writer_s WriterAudioAMR = {
    &reset,
    &writeData,
    NULL,
    &caps_amr
};


