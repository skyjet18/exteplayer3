#!/bin/sh
set -eu
: "${FFMPEG_SOURCE:?Set FFMPEG_SOURCE to the FFmpeg source tree}"
: "${FFMPEG_BUILD:?Set FFMPEG_BUILD to the configured native FFmpeg build}"
: "${1:?Specify the output executable path}"
output=$1
cd "$(dirname "$0")/.."
alias_flags=""
if [ "${HOST_HLS_ALIAS:-0}" = 1 ]; then
    alias_flags="tests/hls_format_alias.c -Wl,--wrap=avformat_open_input"
fi
${CC:-cc} ${CFLAGS:-} -g -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
    -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels \
    -DEXTEPLAYER_FAKE_VIDEO="\"${HOST_VIDEO_OUTPUT:-/dev/null}\"" \
    -DEXTEPLAYER_FAKE_AUDIO="\"${HOST_AUDIO_OUTPUT:-/dev/null}\"" \
    -DEXTEPLAYER_TERM_SOCKET='""' \
    -Wno-deprecated-declarations -Iinclude -Iexternal -Iexternal/flv2mpeg4 \
    -I"$FFMPEG_SOURCE" -I"$FFMPEG_BUILD" \
    main/exteplayer.c tools/debug.c tools/strbuffer.c tools/pcm_export.c $alias_flags \
    container/container.c container/container_ffmpeg.c \
    manager/manager.c manager/audio.c manager/video.c manager/subtitle.c \
    playback/playback.c output/output.c output/output_subtitle.c output/graphic_subtitle.c \
    output/linuxdvb_fake.c output/linuxdvb_buffering.c \
    output/writer/common/pes.c output/writer/common/misc.c output/writer/common/writer.c \
    output/writer/mipsel/*.c external/ffmpeg/src/bitstream.c \
    external/ffmpeg/src/latmenc.c external/ffmpeg/src/mpeg4audio.c \
    external/flv2mpeg4/src/*.c external/plugins/src/png.c \
    "$FFMPEG_BUILD/libavformat/libavformat.a" "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libswresample/libswresample.a" "$FFMPEG_BUILD/libswscale/libswscale.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" \
    ${FFMPEG_EXTRA_LIBS:-} -pthread -lm -ldl -o "$output"
# ASan global registration retains unrelated container command handlers.
${CC:-cc} ${CFLAGS:-} -fno-sanitize=address -g -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -I"$FFMPEG_SOURCE" -I"$FFMPEG_BUILD" tests/http_seek_test.c tools/debug.c \
    "$FFMPEG_BUILD/libavformat/libavformat.a" "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libswresample/libswresample.a" "$FFMPEG_BUILD/libavutil/libavutil.a" \
    ${FFMPEG_EXTRA_LIBS:-} -Wl,--gc-sections -pthread -lm -o "$output-http-seek"
