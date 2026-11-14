#!/bin/sh
set -eu
: "${FFMPEG_SOURCE:?Set FFMPEG_SOURCE to the FFmpeg source tree}"
: "${FFMPEG_BUILD:?Set FFMPEG_BUILD to the configured native FFmpeg build}"
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -f "$tmp/manager" "$tmp/container" "$tmp/output" "$tmp/dvb" "$tmp/pts" "$tmp/pcm_export" "$tmp/playback" "$tmp/buffering"; rmdir "$tmp"' EXIT HUP INT TERM
cc=${CC:-cc}
for test in manager container output dvb pts pcm_export playback buffering; do
    sources="tests/${test}_test.c tools/debug.c"
    if [ "$test" = manager ]; then
        sources="$sources manager/manager.c manager/audio.c manager/video.c manager/subtitle.c"
    fi
    if [ "$test" = output ]; then
        sources="$sources output/writer/common/pes.c output/writer/common/misc.c output/writer/mipsel/h264.c output/writer/mipsel/writer.c"
    fi
    if [ "$test" = pcm_export ]; then
        sources="$sources tools/pcm_export.c"
    fi
    if [ "$test" = buffering ]; then
        sources="$sources output/writer/mipsel/writer.c output/writer/common/writer.c"
    fi
    libs=""
    if [ "$test" = container ]; then
        libs="$FFMPEG_BUILD/libavformat/libavformat.a $FFMPEG_BUILD/libavcodec/libavcodec.a $FFMPEG_BUILD/libswresample/libswresample.a $FFMPEG_BUILD/libavutil/libavutil.a -lm"
    fi
    # ASan global registration retains the full container command table.
    # UBSan and section GC let the IO harness test production helpers in isolation.
    $cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
        -Wno-deprecated-declarations -Iinclude -Iexternal \
        -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" $sources \
        -Wl,--gc-sections -pthread $libs ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/$test"
    timeout 20 "$tmp/$test"
done
# The subtitle and writer programs link no command table, so ASan checks them too.
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export ASAN_OPTIONS=detect_leaks=0
for test in subtitle writers buffering_chunk progressive; do
    sources="tests/${test}_test.c tools/debug.c"
    sanitizers=undefined
    libs=""
    if [ "$test" = subtitle ]; then
        sources="$sources tools/strbuffer.c"
        sanitizers=address,undefined
    fi
    if [ "$test" = writers ]; then
        sources="$sources output/writer/common/pes.c output/writer/common/misc.c output/writer/common/writer.c"
        sources="$sources output/writer/mipsel/h264.c output/writer/mipsel/h265.c output/writer/mipsel/mpeg2.c"
        sources="$sources output/writer/mipsel/vc1.c output/writer/mipsel/wmv.c output/writer/mipsel/vp.c"
        sources="$sources output/writer/mipsel/bcma.c output/writer/mipsel/pcm.c output/writer/mipsel/aac.c"
        sources="$sources external/ffmpeg/src/latmenc.c external/ffmpeg/src/mpeg4audio.c external/ffmpeg/src/bitstream.c"
        libs="$FFMPEG_BUILD/libavcodec/libavcodec.a $FFMPEG_BUILD/libavutil/libavutil.a -lm"
        sanitizers=address,undefined
    fi
    if [ "$test" = buffering_chunk ]; then
        sources="$sources output/writer/mipsel/writer.c output/writer/common/writer.c"
    fi
    if [ "$test" = progressive ]; then
        libs="$FFMPEG_BUILD/libavformat/libavformat.a $FFMPEG_BUILD/libavcodec/libavcodec.a $FFMPEG_BUILD/libswresample/libswresample.a $FFMPEG_BUILD/libavutil/libavutil.a -lm"
    fi
    $cc -g -fsanitize=$sanitizers -ffunction-sections -fdata-sections \
        -Wno-deprecated-declarations -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels \
        -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" $sources \
        -Wl,--gc-sections -pthread $libs ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/$test"
    timeout 20 "$tmp/$test"
done
