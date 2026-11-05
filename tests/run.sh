#!/bin/sh
set -eu
: "${FFMPEG_SOURCE:?Set FFMPEG_SOURCE to the FFmpeg source tree}"
: "${FFMPEG_BUILD:?Set FFMPEG_BUILD to the configured native FFmpeg build}"
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -f "$tmp/manager" "$tmp/container" "$tmp/output" "$tmp/dvb" "$tmp/pts"; rmdir "$tmp"' EXIT HUP INT TERM
cc=${CC:-cc}
for test in manager container output dvb pts; do
    sources="tests/${test}_test.c tools/debug.c"
    if [ "$test" = manager ]; then
        sources="$sources manager/manager.c manager/audio.c manager/video.c manager/subtitle.c"
    fi
    if [ "$test" = output ]; then
        sources="$sources output/writer/common/pes.c output/writer/common/misc.c output/writer/mipsel/h264.c output/writer/mipsel/writer.c"
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
        -Wl,--gc-sections -pthread $libs -o "$tmp/$test"
    timeout 20 "$tmp/$test"
done
