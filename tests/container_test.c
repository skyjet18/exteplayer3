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
    {
        AVFormatContext *av = avformat_alloc_context();
        AVStream *low, *high;
        AVProgram *a, *b;
        assert(av);
        low = avformat_new_stream(av, NULL);
        high = avformat_new_stream(av, NULL);
        assert(low && high);
        low->codecpar->codec_type = high->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        low->codecpar->codec_id = AV_CODEC_ID_H264;
        high->codecpar->codec_id = AV_CODEC_ID_HEVC;
        high->codecpar->width = 3840;
        high->codecpar->height = 2160;
        a = av_new_program(av, 1);
        b = av_new_program(av, 2);
        assert(a && b);
        av_program_add_stream_index(av, 1, 0);
        av_program_add_stream_index(av, 2, 1);
        av_dict_set(&a->metadata, "variant_bitrate", "100", 0);
        av_dict_set(&b->metadata, "variant_bitrate", "900", 0);
        av_dict_set(&low->metadata, "variant_bitrate", "100", 0);
        av_dict_set(&high->metadata, "variant_bitrate", "900", 0);
        adaptive_quality_set(0, 0);
        assert(select_program(av) == b && select_dash_video(av) == 1);
        adaptive_quality_set(1, 0);
        assert(select_program(av) == a && select_dash_video(av) == 0);
        h264_resolution_limit_set(1920, 1080);
        adaptive_quality_set(2, 0);
        assert(select_dash_video(av) == 1); /* HEVC is not capped. */
        high->codecpar->codec_id = AV_CODEC_ID_H264;
        assert(select_program(av) == a && select_dash_video(av) == 0);
        low->codecpar->width = 3840;
        low->codecpar->height = 2160;
        assert(!select_program(av) && select_dash_video(av) == -1);
        sel_program_id_set(2);
        assert(select_program(av) == b); /* Explicit selection overrides limits. */
        sel_program_id_set(-1);
        h264_resolution_limit_set(0, 0);
        avformat_free_context(av);
    }
    return 0;
}
