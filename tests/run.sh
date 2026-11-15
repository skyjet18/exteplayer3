#!/bin/sh
set -eu
: "${FFMPEG_SOURCE:?Set FFMPEG_SOURCE to the FFmpeg source tree}"
: "${FFMPEG_BUILD:?Set FFMPEG_BUILD to the configured native FFmpeg build}"
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -f "$tmp/manager" "$tmp/container" "$tmp/output" "$tmp/dvb" "$tmp/pts" "$tmp/pcm_export" "$tmp/playback" "$tmp/buffering"; rmdir "$tmp"' EXIT HUP INT TERM
# Graphic subtitles without exteplayer3png.so: the PNG encoder of libavcodec,
# read back with its decoder; LeakSanitizer checks the encoder of each picture.
${CC:-cc} -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/png_test.c tools/debug.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libswresample/libswresample.a" "$FFMPEG_BUILD/libavutil/libavutil.a" \
    -lm -ldl ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/png"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/png"
rm -f "$tmp/png"
cc=${CC:-cc}
# Decoder restarts of the DVB output: the audio restart behind a buffer clear
# and the video writer on OUTPUT_SWITCH "video".
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/dvb_restart_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/dvb_restart"
timeout 20 "$tmp/dvb_restart"
rm -f "$tmp/dvb_restart"
# The live start hold: the default of the box taken by the play thread, and
# EP3_LIVE_SYNC, at the first video packet of a stream without a duration.
# The play thread needs what the player links with the container: all of
# tools/ and the bundled mpeg4audio.c; section GC drops what is not used.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels external/ffmpeg/src/mpeg4audio.c \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/live_sync_test.c tools/*.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/live_sync"
timeout 20 "$tmp/live_sync"
rm -f "$tmp/live_sync"
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
        # WriteWithRetry() asks GetSTBType()
        sources="$sources output/writer/common/misc.c"
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
# The stall limit of http inputs: open, stream info and read, with the clock
# of the container and those three calls taken over by the test. The play
# thread needs what the player links with the container: all of tools/, the
# box type of misc.c and the bundled mpeg4audio.c; section GC drops what is
# not used.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels external/ffmpeg/src/mpeg4audio.c \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/net_stall_test.c tools/*.c output/writer/common/misc.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/net_stall"
timeout 20 "$tmp/net_stall"
rm -f "$tmp/net_stall"
# The play thread of the container, started through the pthread_create() of
# the test: the thread has ended when container_ffmpeg_play() goes on.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels external/ffmpeg/src/mpeg4audio.c \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/play_thread_test.c tools/debug.c tools/pcm_export.c tools/hevc_hdr.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/play_thread"
status=0
timeout 20 "$tmp/play_thread" || status=$?
rm -f "$tmp/play_thread"
[ "$status" = 0 ] || exit "$status"
# The HDR type of HEVC streams: SPS and SEI written bit by bit, Annex-B and
# hvcC extradata, the inspection limits, cut and random input. ASan sees reads
# beyond the input and LeakSanitizer the copies the detector keeps.
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iinclude \
    tests/hevc_hdr_test.c tools/hevc_hdr.c -Wl,--gc-sections -o "$tmp/hevc_hdr"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/hevc_hdr"
rm -f "$tmp/hevc_hdr"
# The subtitle and writer programs link no command table, so ASan checks them too.
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# DTS core frames padded to a multiple of four bytes: the writer with the box
# type and EP3_DTS_PAD of the case, then the frame helpers on their own.
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/dts_pad_test.c tools/debug.c \
    output/writer/common/pes.c -Wl,--gc-sections -pthread -o "$tmp/dts_pad"
ASAN_OPTIONS=detect_leaks=0 timeout 20 "$tmp/dts_pad"
$cc -g -fsanitize=address,undefined -Iinclude tests/dts_frame_test.c -o "$tmp/dts_frame"
ASAN_OPTIONS=detect_leaks=0 timeout 20 "$tmp/dts_frame"
export ASAN_OPTIONS=detect_leaks=0
# The DD+ restart after the first passthrough start, with the model and the
# mode from stubbed /proc files and a clock moved by the test.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/eac3_restart_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/eac3_restart"
timeout 20 "$tmp/eac3_restart"
# Output follow-up: the writers with allocations that fail (malloc() of these
# sources is the one of the test), and a leak check of the buffering queue.
sources="tests/writers_alloc_test.c tools/debug.c output/writer/common/pes.c output/writer/common/misc.c"
sources="$sources output/writer/common/writer.c"
sources="$sources output/writer/mipsel/h264.c output/writer/mipsel/h265.c output/writer/mipsel/aac.c"
sources="$sources external/ffmpeg/src/latmenc.c external/ffmpeg/src/mpeg4audio.c external/ffmpeg/src/bitstream.c"
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Dmalloc=test_malloc \
    -Wno-deprecated-declarations -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels \
    -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" $sources -Wl,--gc-sections -pthread \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libavutil/libavutil.a" -lm \
    ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/writers_alloc"
timeout 20 "$tmp/writers_alloc"
# The DTS-HD bypass mode: the ioctls of the DVB output before the first DTS frame.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/dts_hd_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/dts_hd"
timeout 20 "$tmp/dts_hd"
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/buffering_leak_test.c tools/debug.c \
    -Wl,--gc-sections -pthread -o "$tmp/buffering_leak"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/buffering_leak"
# HiSilicon receivers: the detection with stubbed box files, the write of the
# buffering thread to a stubbed device, and the optional software playback
# clock with a clock moved by its own sleeps.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/hisilicon_test.c tools/debug.c \
    output/writer/common/writer.c -Wl,--gc-sections -pthread -o "$tmp/hisilicon"
timeout 20 "$tmp/hisilicon"
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/pacing_test.c tools/debug.c \
    -Wl,--gc-sections -pthread -o "$tmp/pacing"
timeout 20 "$tmp/pacing"
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
        # WriteWithRetry() asks GetSTBType()
        sources="$sources output/writer/common/misc.c"
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
# The SPS level limit of the H.264 writer with the default of a MIPS build.
sources="tests/h264_level_test.c tools/debug.c output/writer/common/pes.c output/writer/common/misc.c"
sources="$sources output/writer/mipsel/h264.c output/writer/mipsel/writer.c"
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections -DH264_LEVEL41_DEFAULT=1 \
    -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" $sources \
    -Wl,--gc-sections -pthread -o "$tmp/h264_level"
timeout 20 "$tmp/h264_level"
# The open program asks LeakSanitizer itself after single cases. Without ASan
# it registers no globals, so section GC still drops the command table.
$cc -g -fsanitize=leak,undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/container_open_test.c tools/debug.c tools/pcm_export.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/container_open"
timeout 20 "$tmp/container_open"
# The DVB output against a stub driver: video events, the decoder's answers and the track description.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels \
    -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/video_info_test.c tools/debug.c manager/manager.c manager/audio.c manager/video.c manager/subtitle.c \
    -Wl,--gc-sections -pthread ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/video_info"
timeout 20 "$tmp/video_info"
