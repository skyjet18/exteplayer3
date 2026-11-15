#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include "../external/plugins/src/png.c"

enum { WIDTH = 5, HEIGHT = 3 };

static char directory[] = "/tmp/exteplayer3-png-XXXXXX";
static char path[sizeof(directory) + 32];

static size_t files(void)
{
    size_t count = 0;
    struct dirent *entry;
    DIR *dir = opendir(directory);
    assert(dir);
    while ((entry = readdir(dir)))
        count += entry->d_name[0] != '.';
    closedir(dir);
    return count;
}

/* The file as the front end gets it, through the PNG decoder of libavcodec. */
static AVFrame *decode(const char *name)
{
    FILE *file = fopen(name, "rb");
    long size;
    uint8_t *data;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    assert(file && codec && context && packet && frame);
    assert(!fseek(file, 0, SEEK_END) && (size = ftell(file)) > 8 && !fseek(file, 0, SEEK_SET));
    data = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    assert(data && fread(data, 1, size, file) == (size_t)size);
    fclose(file);
    assert(!memcmp(data, "\x89PNG\r\n\x1a\n", 8));
    assert(!av_packet_from_data(packet, data, size));
    assert(avcodec_open2(context, codec, NULL) >= 0);
    assert(avcodec_send_packet(context, packet) >= 0);
    assert(avcodec_receive_frame(context, frame) >= 0);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return frame;
}

/* exteplayer3png.so is not installed: the encoder of libavcodec takes its
 * place, the picture comes back pixel for pixel, alpha included. */
static void picture_without_plugin(void)
{
    unsigned char rgba[WIDTH * HEIGHT * 4];
    void *plugin = dlopen("exteplayer3png.so", RTLD_LAZY);
    AVFrame *frame;
    size_t i;
    int y;
    if (plugin) {
        fprintf(stderr, "exteplayer3png.so is installed, this test needs a system without it\n");
        abort();
    }
    for (i = 0; i < sizeof(rgba); i++)
        rgba[i] = (unsigned char)(i * 37 + i / 4 * 61);
    rgba[3] = 0;
    rgba[7] = 255;
    assert(PNGPlugin_init() == 0);
    assert(PNGPlugin_init() == 0);
    assert(PNGPlugin_saveRGBAImage(path, rgba, WIDTH, HEIGHT) == 0);
    /* only the picture: the temporary name is gone */
    assert(files() == 1);
    frame = decode(path);
    assert(frame->format == AV_PIX_FMT_RGBA && frame->width == WIDTH && frame->height == HEIGHT);
    for (y = 0; y < HEIGHT; y++)
        assert(!memcmp(frame->data[0] + y * frame->linesize[0], rgba + y * WIDTH * 4, WIDTH * 4));
    av_frame_free(&frame);
    assert(!unlink(path));
}

/* A picture the encoder refuses leaves no file behind. */
static void refused_picture(void)
{
    unsigned char rgba[4] = {1, 2, 3, 4};
    assert(PNGPlugin_init() == 0);
    assert(PNGPlugin_saveRGBAImage(path, rgba, 0, 1) == -1);
    assert(PNGPlugin_saveRGBAImage(path, rgba, 1, 0) == -1);
    assert(PNGPlugin_saveRGBAImage(path, NULL, 1, 1) == -1);
    assert(files() == 0);
}

int main(void)
{
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/1_1000_0.png", directory);
    picture_without_plugin();
    refused_picture();
    assert(!rmdir(directory));
    return 0;
}
