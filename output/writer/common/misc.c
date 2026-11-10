/*
 * LinuxDVB Output handling.
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
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>

#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>

#include "stm_ioctls.h"
#include "bcm_ioctls.h"

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"

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

void PutBits(BitPacker_t * ld, unsigned int code, unsigned int length)
{
    unsigned int bit_buf;
    unsigned int bit_left;

    bit_buf = ld->BitBuffer;
    bit_left = ld->Remaining;

#ifdef DEBUG_PUTBITS
    if (ld->debug)
        dprintf("code = %d, length = %d, bit_buf = 0x%x, bit_left = %d\n", code, length, bit_buf, bit_left);
#endif /* DEBUG_PUTBITS */

    if (length < bit_left)
    {
        /* fits into current buffer */
        bit_buf = (bit_buf << length) | code;
        bit_left -= length;
    }
    else
    {
        /* doesn't fit */
        bit_buf <<= bit_left;
        bit_buf |= code >> (length - bit_left);
        ld->Ptr[0] = (char)(bit_buf >> 24);
        ld->Ptr[1] = (char)(bit_buf >> 16);
        ld->Ptr[2] = (char)(bit_buf >> 8);
        ld->Ptr[3] = (char)bit_buf;
        ld->Ptr   += 4;
        length    -= bit_left;
        bit_buf    = code & ((1 << length) - 1);
        bit_left   = 32 - length;
        bit_buf = code;
    }

#ifdef DEBUG_PUTBITS
    if (ld->debug)
        dprintf("bit_left = %d, bit_buf = 0x%x\n", bit_left, bit_buf);
#endif /* DEBUG_PUTBITS */

    /* writeback */
    ld->BitBuffer = bit_buf;
    ld->Remaining = bit_left;
}

void FlushBits(BitPacker_t * ld)
{
    ld->BitBuffer <<= ld->Remaining;
    while (ld->Remaining < 32)
    {
#ifdef DEBUG_PUTBITS
        if (ld->debug)
            dprintf("flushing 0x%2.2x\n", ld->BitBuffer >> 24);
#endif /* DEBUG_PUTBITS */
        *ld->Ptr++ = ld->BitBuffer >> 24;
        ld->BitBuffer <<= 8;
        ld->Remaining += 8;
    }
    ld->Remaining = 32;
    ld->BitBuffer = 0;
}

/* true when the first line of the file names a HiSilicon SoC,
 * e.g. "hi3798mv200" or "Hi3716MV430" in /proc/stb/info/chipset */
static bool IsHiSiliconChipsetFile(const char *path)
{
    char buf[64];
    size_t i;
    FILE *f = fopen(path, "r");

    if (NULL == f)
    {
        return false;
    }
    if (NULL == fgets(buf, (int)sizeof(buf), f))
    {
        fclose(f);
        return false;
    }
    fclose(f);

    for (i = 0; buf[i] != '\0'; ++i)
    {
        if (buf[i] >= 'A' && buf[i] <= 'Z')
        {
            buf[i] = (char)(buf[i] - 'A' + 'a');
        }
    }
    return 0 == strncmp(buf, "hi3", 3) || NULL != strstr(buf, "hisi");
}

/* The device tree node is missing on a number of HiSilicon receivers, so
 * also accept what the vendor SDK creates and the chipset name of the box. */
static bool IsHiSiliconSTB(void)
{
    static const char *const markers[] = {
        "/sys/firmware/devicetree/base/soc/hisilicon_clock/name",
        "/proc/hisi",
        "/dev/hi_mem",
        "/dev/hi_dmx",
        "/sys/module/hi_media",
        NULL
    };
    int i;

    for (i = 0; markers[i] != NULL; ++i)
    {
        if (access(markers[i], F_OK) != -1)
        {
            return true;
        }
    }
    return IsHiSiliconChipsetFile("/proc/stb/info/chipset");
}

stb_type_t GetSTBType()
{
    static stb_type_t type = STB_UNKNOWN;
    if (type == STB_UNKNOWN) {
        struct stat buffer;
        if (access("/proc/stb/tpm/0/serial", F_OK) != -1) {
            type = STB_DREAMBOX;
        }
        else if (access("/proc/stb/info/vumodel", F_OK) != -1 && \
                 access("/proc/stb/info/boxtype", F_OK) == -1 ) {
            // some STB like Octagon SF4008 has also /proc/stb/info/vumodel
            // but VU PLUS does not have /proc/stb/info/boxtype
            // please see: https://gitlab.com/e2i/e2iplayer/issues/282
            type = STB_VUPLUS;
        }
        else if (IsHiSiliconSTB()) {
            type = STB_HISILICON;
        }
        else {
            type = STB_OTHER;
        }
    }

    return type;
}
