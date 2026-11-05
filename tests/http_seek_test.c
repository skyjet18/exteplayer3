#include <assert.h>
#include "../container/container_ffmpeg.c"

int main(int argc, char **argv)
{
    AVFormatContext *av = NULL;
    AVPacket packet = {0};
    int64_t size, packet_size, expected, actual = AV_NOPTS_VALUE;
    int i;
    assert(argc == 2);
    assert(avformat_open_input(&av, argv[1], NULL, NULL) >= 0);
    assert(avformat_find_stream_info(av, NULL) >= 0);
    assert(av->duration > 0);
    assert(av_opt_get_int(av->priv_data, "ts_packetsize", 0, &packet_size) >= 0);
    size = avio_size(av->pb);
    expected = ts_seek_position(av->duration / 2, av->duration, size, packet_size);
    assert(expected > 0);
    assert(fast_http_ts_seek(av, av->duration / 2) >= 0);
    for (i = 0; i < 1000 && av_read_frame(av, &packet) >= 0; i++)
    {
        if (packet.pts != AV_NOPTS_VALUE)
            actual = av_rescale_q(packet.pts, av->streams[packet.stream_index]->time_base,
                                  AV_TIME_BASE_Q) - av->start_time;
        av_packet_unref(&packet);
        if (actual != AV_NOPTS_VALUE)
            break;
    }
    assert(actual != AV_NOPTS_VALUE);
    printf("{\"position\":%"PRId64",\"packet_size\":%"PRId64",\"target_us\":%"PRId64
           ",\"actual_us\":%"PRId64"}\n", expected, packet_size, av->duration / 2, actual);
    avformat_close_input(&av);
    return 0;
}
