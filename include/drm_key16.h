#ifndef _drm_key16_123
#define _drm_key16_123

/*
 * A 16-byte CENC KID or key as FFmpeg takes it: 32 lower case hex digits.
 *
 * Front ends and key services give them as
 *   32 hex digits, upper or lower case    00112233445566778899aabbccddeeff
 *   the same with "0x" (or "0X") ahead    0x00112233445566778899AABBCCDDEEFF
 *   a UUID, 8-4-4-4-12 hex digits         00112233-4455-6677-8899-aabbccddeeff
 *   22 base64url characters, no padding   ABEiM0RVZneImaq7zN3u_w
 *     (RFC 4648 section 5; "kid" and "k" of a W3C EME ClearKey JWK)
 *   base64, 22 characters or 24 with "=="  ABEiM0RVZneImaq7zN3u/w==
 *     (RFC 4648 section 4; "+" "/" and "-" "_" are both taken, they stand
 *     for the same bits)
 * Anything else is not taken: other lengths, other characters, dashes
 * elsewhere, "0x" before a UUID, a single "=" or "=" elsewhere, and base64
 * whose last character has bits beyond the 16 bytes.
 */

/* the forms, for messages */
#define DRM_KEY16_FORMS "16-byte KID/key expected: 32 hex, 0x+32 hex, UUID or base64/base64url"

#include <stddef.h>
#include <string.h>

static inline int drm_key16_hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* base64 and base64url */
static inline int drm_key16_base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if ('-' == c || '+' == c)
    {
        return 62;
    }
    if ('_' == c || '/' == c)
    {
        return 63;
    }
    return -1;
}

/* src (len characters, not terminated) as 32 lower case hex digits in
 * dst[33]; 1 if it is one of the forms above, otherwise 0 and dst is left
 * as it was */
static inline int drm_key16(char *dst, const char *src, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    size_t i, n = 0;

    if (34 == len && '0' == src[0] && ('x' == src[1] || 'X' == src[1]))
    {
        src += 2;
        len = 32;
    }
    if (32 == len || 36 == len)
    {
        for (i = 0; i < len; i++)
        {
            int v;
            if (36 == len && (8 == i || 13 == i || 18 == i || 23 == i))
            {
                if ('-' != src[i])
                {
                    return 0;
                }
                continue;
            }
            v = drm_key16_hex_value(src[i]);
            if (v < 0)
            {
                return 0;
            }
            hex[n++] = digits[v];
        }
    }
    else if (22 == len || (24 == len && '=' == src[22] && '=' == src[23]))
    {
        /* 22 x 6 bits: the 16 bytes and 4 bits that have to be 0; "=" only
         * as the padding "==" */
        unsigned int bits = 0, count = 0;
        for (i = 0; i < 22; i++)
        {
            int v = drm_key16_base64_value(src[i]);
            if (v < 0)
            {
                return 0;
            }
            bits = ((bits << 6) | (unsigned int)v) & 0x3fff;
            count += 6;
            if (count >= 8)
            {
                unsigned int byte;
                count -= 8;
                byte = (bits >> count) & 0xff;
                hex[n++] = digits[byte >> 4];
                hex[n++] = digits[byte & 0x0f];
            }
        }
        if (bits & ((1u << count) - 1))
        {
            return 0;
        }
    }
    else
    {
        return 0;
    }
    hex[32] = '\0';
    memcpy(dst, hex, 33);
    return 1;
}

/* "kid=key:kid=key..." (decryption_keys of the mov demuxer, cenc_decryption_keys
 * of the DASH demuxer) with every KID and key as 32 hex digits, in dst of
 * size bytes; 1 if every pair is one, otherwise 0 (dst then holds a part).
 * A KID in padded base64 has "=" of its own: the "=" between KID and key is
 * the one with a KID before it and a key after it. */
static inline int drm_key16_list(char *dst, size_t size, const char *src)
{
    size_t o = 0;

    for (;;)
    {
        size_t len = strcspn(src, ":");
        const char *eq = memchr(src, '=', len);
        if (size < o + 66)
        {
            return 0;
        }
        while (eq && !(drm_key16(dst + o, src, eq - src) &&
                       drm_key16(dst + o + 33, eq + 1, src + len - eq - 1)))
        {
            eq = memchr(eq + 1, '=', src + len - eq - 1);
        }
        if (!eq)
        {
            return 0;
        }
        dst[o + 32] = '=';
        o += 65;
        if (!src[len])
        {
            return 1;
        }
        dst[o++] = ':';
        src += len + 1;
    }
}

#endif
