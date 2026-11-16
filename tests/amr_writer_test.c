#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../output/writer/mipsel/amr.c"

/* The AMR writer of the hardware path (A_AMR): with codec data of at least
 * 17 bytes (the 'damr' box) the PES carries a 4 byte length of frame + 17 and
 * the 9 data bytes of the box in front of the frame; without codec data, or
 * with a shorter one, the frame goes alone. The codec data is given in a
 * buffer of its own size, so ASan sees a read beyond it. */

static uint8_t written[512];
static size_t written_size;
static int calls;

static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    int i;
    assert(fd == 1 && count >= 1 && !calls++);
    written_size = 0;
    for (i = 0; i < count; i++)
    {
        assert(written_size + iov[i].iov_len <= sizeof(written));
        memcpy(written + written_size, iov[i].iov_base, iov[i].iov_len);
        written_size += iov[i].iov_len;
    }
    return (ssize_t)written_size;
}

/* the PES packet length and where the payload of the packet starts */
static size_t pes_payload(size_t *packet_length)
{
    assert(written_size > 9);
    assert(written[0] == 0 && written[1] == 0 && written[2] == 1 && written[3] == 0xc0);
    *packet_length = (size_t)written[4] << 8 | written[5];
    assert(*packet_length + 6 == written_size);
    return 9 + written[8];
}

static void write_frame(const uint8_t *codec_data, unsigned int codec_size)
{
    uint8_t frame[32];
    uint8_t *config = NULL;
    WriterAVCallData_t call = {.fd = 1, .data = frame, .len = sizeof(frame), .Pts = 90000,
                               .WriteV = capture};
    size_t i;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)(0x3c + i);
    if (codec_data)
    {
        config = malloc(codec_size);
        assert(config);
        memcpy(config, codec_data, codec_size);
        call.private_data = config;
        call.private_size = codec_size;
    }
    calls = 0;
    assert(WriterAudioAMR.writeData(&call) == (int)written_size && calls == 1);
    free(config);
}

static void check_plain(void)
{
    size_t length, start = pes_payload(&length);
    assert(written_size - start == 32);
    assert(written[start] == 0x3c && written[written_size - 1] == 0x3c + 31);
}

int main(void)
{
    /* size 17, 'damr', vendor 'FFMP', decoder version 0, mode set 0x81ff,
     * mode change period 0, frames per sample 1 */
    static const uint8_t damr[17] = {0, 0, 0, 17, 'd', 'a', 'm', 'r', 'F', 'F', 'M', 'P',
                                     0, 0x81, 0xff, 0, 1};
    size_t length, start;

    /* codec data: length, the 9 bytes after size and type, the frame */
    write_frame(damr, sizeof(damr));
    start = pes_payload(&length);
    assert(written_size - start == 4 + 9 + 32);
    assert(written[start] == 0 && written[start + 1] == 0 && written[start + 2] == 0 &&
           written[start + 3] == 32 + 17);
    assert(!memcmp(written + start + 4, damr + 8, 9));
    assert(written[start + 13] == 0x3c && written[written_size - 1] == 0x3c + 31);

    /* no codec data (raw .amr): the frame alone */
    write_frame(NULL, 0);
    check_plain();

    /* codec data too short for the box: ignored */
    write_frame(damr, 8);
    check_plain();
    write_frame(damr, 16);
    check_plain();

    printf("AMR writer tests passed\n");
    return 0;
}
