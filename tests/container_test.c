#include <assert.h>
#include "../container/container_ffmpeg.c"

int main(void)
{
    CustomIOCtx_t io = {0};
    uint8_t bytes[8];
    assert(network_read_retry(100, 100));
    assert(network_read_retry(100, 100 + 10 * AV_TIME_BASE - 1));
    assert(!network_read_retry(100, 100 + 10 * AV_TIME_BASE));
    assert(!network_read_retry(100, 99));
    io.pFile = tmpfile();
    assert(io.pFile);
    assert(fwrite("abc", 1, 3, io.pFile) == 3);
    rewind(io.pFile);
    assert(SAM_SeekFunc(&io, 0, AVSEEK_SIZE) == 3);
    assert(ftello(io.pFile) == 0);
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == 3);
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == AVERROR_EOF);
    assert(SAM_SeekFunc(&io, 1, SEEK_SET) == 1);
    assert(SAM_ReadFunc(&io, bytes, 1) == 1 && bytes[0] == 'b');
    progressive_playback = 1;
    assert(!fseeko(io.pFile, 0, SEEK_END));
    assert(SAM_ReadFunc(&io, bytes, sizeof(bytes)) == 0);
    progressive_playback = 0;
    fclose(io.pFile);
    return 0;
}
