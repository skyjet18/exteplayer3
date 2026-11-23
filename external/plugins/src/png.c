/*
 * Subtitle output to one registered client.
 *
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
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include "plugins/png.h"
#include "debug.h"

/* Without the plugin the PNG encoder of libavcodec writes the files.
 * It needs avcodec_send_frame() / avcodec_receive_packet(). */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
#define HAVE_PNG_ENCODER_FALLBACK
#endif

static void *handle;
int (*SaveRGBAImage_handle)(const char *filename, const unsigned char *data, int width, int height);
static int use_encoder;

#ifdef HAVE_PNG_ENCODER_FALLBACK
/* the same contract as SaveRGBAImage() of the plugin: width x height RGBA
 * pixels, rows without padding; 0 on success, -1 on error (no file left) */
static int EncodeRGBAImage(const char *filename, const unsigned char *data, int width, int height)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *context = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    char *tmpname = NULL;
    FILE *file = NULL;
    int ret = -1;
    int y;

    if (!codec || !filename || !data || width <= 0 || height <= 0)
        return -1;

    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!context || !frame || !packet)
        goto done;

    context->width = width;
    context->height = height;
    context->pix_fmt = AV_PIX_FMT_RGBA;
    context->time_base = (AVRational){1, 25};
    /* as the plugin: the fastest zlib level for flat colours on transparency */
    context->compression_level = 1;
    /* this also checks that width x height is a valid picture size */
    if (avcodec_open2(context, codec, NULL) < 0)
        goto done;

    frame->format = AV_PIX_FMT_RGBA;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0)
        goto done;
    for (y = 0; y < height; y++)
        memcpy(frame->data[0] + (size_t)y * frame->linesize[0], data + (size_t)y * width * 4, (size_t)width * 4);

    /* one picture: send it, end the stream, take its packet */
    if (avcodec_send_frame(context, frame) < 0 || avcodec_send_frame(context, NULL) < 0 ||
        avcodec_receive_packet(context, packet) < 0)
        goto done;

    /* written under another name and renamed: the directory never holds
     * a partly written file under the name of a subtitle */
    tmpname = malloc(strlen(filename) + sizeof(".tmp"));
    if (!tmpname)
        goto done;
    sprintf(tmpname, "%s.tmp", filename);
    file = fopen(tmpname, "wb");
    if (!file)
        goto done;
    if (fwrite(packet->data, 1, packet->size, file) == (size_t)packet->size)
        ret = 0;
    /* a full disk may only show up when the buffered data is written */
    if (fclose(file) != 0)
        ret = -1;
    if (0 == ret && rename(tmpname, filename) != 0)
        ret = -1;
    if (0 != ret)
        remove(tmpname);

done:
    if (0 != ret)
        plugin_err("cannot write %s with the PNG encoder of libavcodec\n", filename);
    free(tmpname);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return ret;
}
#endif

/* exteplayer3png.so cannot be used: take the PNG encoder of libavcodec if
 * there is one, else return the error of the plugin as before */
static int UseEncoder(int error)
{
#ifdef HAVE_PNG_ENCODER_FALLBACK
    if (avcodec_find_encoder(AV_CODEC_ID_PNG))
    {
        plugin_err("graphic subtitles are written with the PNG encoder of libavcodec\n");
        use_encoder = 1;
        return 0;
    }
#endif
    plugin_err("no PNG encoder in libavcodec either: graphic subtitles cannot be saved\n");
    return error;
}

int PNGPlugin_saveRGBAImage(const char *filename, const unsigned char *data, int width, int height)
{
    if (SaveRGBAImage_handle != NULL)
        return SaveRGBAImage_handle(filename, data, width, height);
#ifdef HAVE_PNG_ENCODER_FALLBACK
    if (use_encoder)
        return EncodeRGBAImage(filename, data, width, height);
#endif
    return -1;
}

int PNGPlugin_init(void)
{
    if (NULL != handle || use_encoder)
        return 0; /* Already initialized */
    else
    {
        handle = dlopen("exteplayer3png.so", RTLD_LAZY);
        if (handle)
        {
            char *error = NULL;

            dlerror();    /* Clear any existing error */

            *(void **) (&SaveRGBAImage_handle) = dlsym(handle, "SaveRGBAImage");

            if ((error = dlerror()) != NULL)
            {
                /* the text of dlerror() may not outlive dlclose() */
                plugin_err("%s\n", error);
                dlclose(handle);
                handle = NULL;
                SaveRGBAImage_handle = NULL;
                return UseEncoder(-2);
            }
            return 0;
        }
        plugin_err( "%s\n", dlerror());
    }

    return UseEncoder(-1);
}

int PNGPlugin_term(void)
{
    use_encoder = 0;
    if (handle)
    {
        dlclose(handle);
        handle = NULL;
        SaveRGBAImage_handle = NULL;
        return 0;
    }
    return -1;
}
