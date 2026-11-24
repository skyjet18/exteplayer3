#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "drm_key16.h"

/* The forms of a CENC KID or key (drm_key16.h): 32 hex digits, "0x" ahead,
 * UUID, 22 base64url characters; everything else is not taken. */

#define HEX "00112233445566778899aabbccddeeff"

static int taken(const char *src, const char *expected)
{
    char dst[33];
    memset(dst, 'z', sizeof(dst));
    if (!drm_key16(dst, src, strlen(src)))
    {
        /* dst stays as it was */
        size_t i;
        for (i = 0; i < sizeof(dst); i++)
        {
            assert('z' == dst[i]);
        }
        return 0;
    }
    if (strcmp(dst, expected))
    {
        fprintf(stderr, "%s: %s, not %s\n", src, dst, expected);
        return 0;
    }
    return 1;
}

static int not_taken(const char *src)
{
    char dst[33];
    if (drm_key16(dst, src, strlen(src)))
    {
        fprintf(stderr, "taken, but it is not a key: \"%s\" -> %s\n", src, dst);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const char *const rejected[] = {
        "",
        "0011223344556677",                                 /* 16 hex digits */
        "00112233445566778899aabbccddeef",                  /* 31 */
        "00112233445566778899aabbccddeeff0",                /* 33 */
        "00112233445566778899aabbccddeefg",                 /* not hex */
        "00112233445566778899aabbccddee f",
        "0x00112233445566778899aabbccddeef",                /* 0x and 31 */
        "0x00112233445566778899aabbccddeeff0",              /* 0x and 33 */
        "0y00112233445566778899aabbccddeeff",
        "x000112233445566778899aabbccddeeff",
        "0x0x112233445566778899aabbccddeeff",
        "00112233-4455-6677-8899-aabbccddeef",              /* UUID of 35 */
        "00112233-4455-6677-8899-aabbccddeeff0",            /* 37 */
        "001122334-455-6677-8899-aabbccddeeff",             /* dash elsewhere */
        "00112233-4455-6677-8899aabbccddeeff-",
        "00112233-4455-6677-8899+aabbccddeeff",
        "{00112233-4455-6677-8899-aabbccddeef}",
        "0x00112233-4455-6677-8899-aabbccddeeff",           /* 0x before a UUID */
        "00112233-4455-6677-8899-aabbccddeefg",
        "00112233445566778899aabbccdd-eeff",                /* dashes, not a UUID */
        "0011-2233-4455-6677-8899-aabb-ccdd-eeff",
        "ABEiM0RVZneImaq7zN3u_",                            /* 21 base64url */
        "ABEiM0RVZneImaq7zN3u_w_",                          /* 23 */
        "ABEiM0RVZneImaq7zN3u/w=",                          /* a single "=" */
        "ABEiM0RVZneImaq7zN3u/w===",                        /* three */
        "ABEiM0RVZneImaq7zN3u/=w=",                         /* "=" elsewhere */
        "ABEiM0RVZneImaq7zN3u=/w=",
        "=ABEiM0RVZneImaq7zN3u/w=",
        "ABEiM0RVZneImaq7zN3u/==",                          /* 21 and "==" */
        "ABEiM0RVZneImaq7zN3u/wA==",                        /* 23 and "==" */
        "ABEiM0RVZneImaq7zN3u/w=-",
        "ABEiM0RVZneImaq7zN3u/w  ",
        "ABEiM0RVZneImaq7zN3u_x",                           /* bits beyond 16 bytes */
        "ABEiM0RVZneImaq7zN3u_B",
        "ABEiM0RVZneImaq7zN3u/x==",
        "ABEiM0RVZneImaq7zN3u+B==",
        "ABEiM0RVZneImaq7zN3u.w",
        "ABEiM0RVZneImaq7zN3u w",
        "0xABEiM0RVZneImaq7zN3u_w",                         /* 0x before base64url */
        "00112233-4455-6677-ABEiM0RVZneImaq7",              /* mixed */
        "00112233445566778899aabbccddeeff\n",
        NULL,
    };
    char list[512];
    size_t i;
    int ok = 1;

    /* the 32 hex digits of before, also upper case */
    ok &= taken(HEX, HEX);
    ok &= taken("00112233445566778899AABBCCDDEEFF", HEX);
    ok &= taken("00112233445566778899AaBbCcDdEeFf", HEX);
    /* 0x */
    ok &= taken("0x" HEX, HEX);
    ok &= taken("0X00112233445566778899AABBCCDDEEFF", HEX);
    /* UUID */
    ok &= taken("00112233-4455-6677-8899-aabbccddeeff", HEX);
    ok &= taken("00112233-4455-6677-8899-AABBCCDDEEFF", HEX);
    ok &= taken("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", "edef8ba979d64acea3c827dcd51d21ed");
    /* base64url of the 16 bytes (ClearKey JWK "kid" / "k") */
    ok &= taken("ABEiM0RVZneImaq7zN3u_w", HEX);
    ok &= taken("_-7dzLuqmYh3ZlVEMyIRAA", "ffeeddccbbaa99887766554433221100");
    ok &= taken("AAAAAAAAAAAAAAAAAAAAAA", "00000000000000000000000000000000");
    ok &= taken("_____________________w", "ffffffffffffffffffffffffffffffff");
    ok &= taken("Dx4tPEtaaXiHlqW0w9Lh8A", "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    /* base64: "==" padded or not, "+" "/" as well as "-" "_" */
    ok &= taken("ABEiM0RVZneImaq7zN3u/w==", HEX);
    ok &= taken("ABEiM0RVZneImaq7zN3u/w", HEX);
    ok &= taken("ABEiM0RVZneImaq7zN3u_w==", HEX);
    ok &= taken("/+7dzLuqmYh3ZlVEMyIRAA==", "ffeeddccbbaa99887766554433221100");
    ok &= taken("/+7dzLuqmYh3ZlVEMyIRAA", "ffeeddccbbaa99887766554433221100");
    ok &= taken("AAAAAAAAAAAAAAAAAAAAAA==", "00000000000000000000000000000000");
    ok &= taken("/////////////////////w==", "ffffffffffffffffffffffffffffffff");

    for (i = 0; rejected[i]; i++)
    {
        ok &= not_taken(rejected[i]);
    }
    /* a length that is not the one of the string (the parser gets parts
     * of a "dk" line and of "kid:key") */
    {
        char dst[33];
        ok &= !drm_key16(dst, HEX ":", 33);
        ok &= drm_key16(dst, HEX ":" HEX, 32) && !strcmp(dst, HEX);
        ok &= drm_key16(dst, "ABEiM0RVZneImaq7zN3u_w:-", 22) && !strcmp(dst, HEX);
    }

    /* decryption_keys / cenc_decryption_keys of FFmpeg: kid=key:kid=key */
    ok &= drm_key16_list(list, sizeof(list), HEX "=" HEX) && !strcmp(list, HEX "=" HEX);
    ok &= drm_key16_list(list, sizeof(list),
                         "00112233-4455-6677-8899-AABBCCDDEEFF=Dx4tPEtaaXiHlqW0w9Lh8A:"
                         "_-7dzLuqmYh3ZlVEMyIRAA=0xA0B1C2D3E4F5061728394A5B6C7D8E9F") &&
          !strcmp(list, HEX "=0f1e2d3c4b5a69788796a5b4c3d2e1f0:"
                        "ffeeddccbbaa99887766554433221100=a0b1c2d3e4f5061728394a5b6c7d8e9f");
    /* padded base64: the "=" between KID and key is found behind the "==" */
    ok &= drm_key16_list(list, sizeof(list),
                         "ABEiM0RVZneImaq7zN3u/w===/+7dzLuqmYh3ZlVEMyIRAA==:"
                         "/+7dzLuqmYh3ZlVEMyIRAA=ABEiM0RVZneImaq7zN3u/w==") &&
          !strcmp(list, HEX "=ffeeddccbbaa99887766554433221100:"
                        "ffeeddccbbaa99887766554433221100=" HEX);
    ok &= !drm_key16_list(list, sizeof(list), "ABEiM0RVZneImaq7zN3u/w==" HEX);
    ok &= !drm_key16_list(list, sizeof(list), "ABEiM0RVZneImaq7zN3u/w====" HEX);
    ok &= !drm_key16_list(list, sizeof(list), "");
    ok &= !drm_key16_list(list, sizeof(list), HEX);
    ok &= !drm_key16_list(list, sizeof(list), HEX "=" HEX ":");
    ok &= !drm_key16_list(list, sizeof(list), HEX "=" HEX "::" HEX "=" HEX);
    ok &= !drm_key16_list(list, sizeof(list), HEX "=" HEX ":" HEX "=ABEiM0RVZneImaq7zN3u_");
    ok &= !drm_key16_list(list, sizeof(list), HEX ":" HEX);
    /* too small for the result: not taken, not written beyond */
    ok &= !drm_key16_list(list, 65, HEX "=" HEX);
    ok &= drm_key16_list(list, 66, HEX "=" HEX);
    ok &= !drm_key16_list(list, 66 + 65, HEX "=" HEX ":" HEX "=" HEX);
    ok &= drm_key16_list(list, 66 + 66, HEX "=" HEX ":" HEX "=" HEX);

    if (!ok)
    {
        fprintf(stderr, "FAIL drm_key16\n");
        return 1;
    }
    printf("PASS drm_key16: hex, 0x, UUID and base64url KIDs and keys, the rest refused\n");
    return 0;
}
