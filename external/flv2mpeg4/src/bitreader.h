/*
 * Bitstream reader
 *
 * Copyright (c) 2006 vixy project
 *
 * This file contains the code that based on FFmpeg (http://ffmpeg.mplayerhq.hu/)
 * See original copyright notice in /FFMPEG_CREDITS and /FFMPEG_IMPORTS
 *
 * This file is part of VIXY FLV Converter.
 *
 * 'VIXY FLV Converter' is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * 'VIXY FLV Converter' is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef BITREADER_H
#define BITREADER_H

#include "type.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 
typedef struct _BR
{
  const uint8* buf;
  uint32 size;
  uint32 read;
  int bitoffset;
} BR;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 
static void init_br(BR* p, const uint8* buf, uint32 size)
{
  p->buf = buf;
  p->size = size;
  p->read = 0;
  p->bitoffset = 0;
}

/* a read behind the end of the packet sees zero bits; br_overrun() tells
 * whether more bits were taken than the packet has */
static uint8 get_u8(BR* p)
{
  uint8 v = p->read < p->size ? p->buf[p->read] : 0;
  p->read++;
  return v;
}

static uint32 get_u24(BR* p)
{
  uint32 a = get_u8(p);
  uint32 b = get_u8(p);
  uint32 c = get_u8(p);

  return (a << 16) | (b << 8) | c;
}

static uint32 get_u32(BR* p)
{
  uint32 a = get_u8(p);
  uint32 b = get_u8(p);
  uint32 c = get_u8(p);
  uint32 d = get_u8(p);

  return (a << 24) | (b << 16) | (c << 8) | d;
}

static int is_eob(BR* p)
{
  return p->read >= p->size;
}

static int br_overrun(BR* p)
{
  return p->read > p->size || (p->read == p->size && p->bitoffset > 0);
}

/* the 32 bits at the read position, zero behind the end of the packet */
static uint32 show_u32(BR* p)
{
  const uint8* pp;
  uint32 tmp = 0;
  uint32 i;

  if (p->read < p->size && p->size - p->read >= 4)
  {
    pp = p->buf + p->read;
    return ((uint32)pp[0] << 24) | (pp[1] << 16) | (pp[2] << 8 ) | (pp[3]);
  }
  for (i = 0; i < 4; i++)
  {
    tmp <<= 8;
    if (p->read < p->size && i < p->size - p->read)
      tmp |= p->buf[p->read + i];
  }
  return tmp;
}

static void skip(BR* p, uint32 skip)
{
  p->read += skip;
}

static uint32 show_bits(BR* p, uint32 bits)
{
  uint32 tmp;
  
  tmp = show_u32(p);
  tmp <<= p->bitoffset;
  tmp >>= 32 - bits;

  return tmp;
}

static int32 show_sbits(BR* p, uint32 bits)
{
  int32 tmp;
  
  /* shifted as unsigned, the sign comes with the conversion */
  tmp = (int32)(show_u32(p) << p->bitoffset);
  tmp >>= 32 - bits;

  return tmp;
}

static void flash_bits(BR* p, uint32 bits)
{
  if (bits > 0)
  {
    bits = bits + p->bitoffset;
	p->read += bits >> 3;
	p->bitoffset = bits & 7;
  }
}

static uint32 get_bits(BR* p, uint32 bits)
{
  uint32 tmp = show_bits(p, bits);
  flash_bits(p, bits);
  return tmp;
}

static int32 get_sbits(BR* p, uint32 bits)
{
  int32 tmp = show_sbits(p, bits);
  flash_bits(p, bits);
  return tmp;
}

static void align_bits(BR* p)
{
  if (p->bitoffset > 0)
  {
    p->bitoffset = 0;
	p->read++;
  }
}

static int __inline get_br_pos(BR* p)
{
  return (p->read << 3) + p->bitoffset;
}

typedef struct _VLCtab
{
	int code;
	int n;
} VLCtab;

static int __inline get_vlc(BR* br, const VLCtab* table, int bits, int max_depth)
{
	int n, index, nb_bits, code;
	index = show_bits(br, bits);
	code  = table[index].code;
	n     = table[index].n;
		
	if (max_depth > 1 && n < 0)
	{
		flash_bits(br, bits);
		nb_bits = -n;
		
		index = show_bits(br, nb_bits) + code;
		code = table[index].code;
		n = table[index].n;
	}	

	flash_bits(br, n);
	return code;
}

static int __inline get_vlcdec(BR* p, const VLCtab* table, int bits, int max_depth, VLCDEC* vlcdec)
{
  int pos = get_br_pos(p);
  uint32 show = show_bits(p, 24);
  uint32 tmp = get_vlc(p, table, bits, max_depth);
  int len = get_br_pos(p) - pos;
  int val = show >> (24 - len);
  vlcdec->bits = len;
  vlcdec->value = val;
  return tmp;
}

#endif // BITREADER_H
