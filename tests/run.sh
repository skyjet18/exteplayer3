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
# The stop of the DVB decoders: once per device for OUTPUT_STOP and
# OUTPUT_CLOSE together, with the requests of a recording driver.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/decoder_stop_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/decoder_stop"
timeout 20 "$tmp/decoder_stop"
rm -f "$tmp/decoder_stop"
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
# Closed captions in the video stream: A/53 cc_data() in H.264 and HEVC SEI
# and in MPEG-2 user data, cut and random input, the presentation order queue.
$cc -g -fsanitize=address,undefined -Wall -Iinclude tests/cc_data_test.c -o "$tmp/cc_data"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/cc_data"
rm -f "$tmp/cc_data"
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
# The FLV1 to MPEG-4 converter: the MPEG-4 decode of converted pictures against
# the FLV1 decode of the source, for 8 bit escapes and changing quantisers,
# and broken pictures. Every UBSan report ends the test.
$cc -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/flv2mpeg4_test.c external/flv2mpeg4/src/*.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libswresample/libswresample.a" "$FFMPEG_BUILD/libavutil/libavutil.a" \
    -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/flv2mpeg4"
ASAN_OPTIONS=detect_leaks=0 timeout 20 "$tmp/flv2mpeg4"
export ASAN_OPTIONS=detect_leaks=0
# The DD+ restart after the first passthrough start, with the model and the
# mode from stubbed /proc files and a clock moved by the test.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/eac3_restart_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/eac3_restart"
timeout 20 "$tmp/eac3_restart"
# The position of the DVB output with a driver whose VIDEO_GET_PTS stays 0.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/video_pts_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/video_pts"
timeout 20 "$tmp/video_pts"
# Software decoded audio of the play thread with a decoder of the test that
# fails on single frames: the packets after the error still go to it.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels external/ffmpeg/src/mpeg4audio.c \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/audio_decode_error_test.c tools/debug.c tools/pcm_export.c tools/hevc_hdr.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/audio_decode_error"
timeout 20 "$tmp/audio_decode_error"
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
# The AMR writer of the hardware path with and without codec data.
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iinclude -Iexternal \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" tests/amr_writer_test.c tools/debug.c \
    output/writer/common/pes.c output/writer/common/misc.c -Wl,--gc-sections -pthread -o "$tmp/amr_writer"
timeout 20 "$tmp/amr_writer"
# Left, stereo or right of dual mono audio: AUDIO_CHANNEL_SELECT around the
# decoder starts and the stop of the DVB output.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/audio_channel_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/audio_channel"
timeout 20 "$tmp/audio_channel"
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
# The fallback frame rate of the video decoder, with a node of the test.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/fallback_framerate_test.c tools/debug.c -Wl,--gc-sections -pthread -o "$tmp/fallback_framerate"
timeout 20 "$tmp/fallback_framerate"
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
        sources="$sources output/writer/mipsel/bcma.c output/writer/mipsel/pcm.c output/writer/mipsel/lpcm.c output/writer/mipsel/aac.c"
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
# MP4 files with the avcC of replay clips (SPS and PPS twice, a zero at the
# end of a PPS; a zero between two PPS) through the H.264 writer: each set
# once and without trailing zeros, every frame decodes from what it wrote.
python3 tests/h264_avcc_mp4.py "$FFMPEG_BUILD/ffmpeg" "$tmp" > /dev/null
sources="tests/h264_avcc_test.c tools/debug.c output/writer/common/pes.c output/writer/common/misc.c"
sources="$sources output/writer/mipsel/h264.c output/writer/mipsel/writer.c"
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections -Wno-deprecated-declarations \
    -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" $sources \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/h264_avcc"
timeout 20 "$tmp/h264_avcc" "$tmp/avcc-repeated.mp4" 1 1 10
timeout 20 "$tmp/h264_avcc" "$tmp/avcc-stray.mp4" 1 2 10
# The sh4 output is built only for sh4 receivers: compile its sources here as
# the recipe lists them, so that they keep compiling.
for f in output/linuxdvb_sh4.c output/writer/sh4/writer.c output/writer/sh4/aac.c \
         output/writer/sh4/ac3.c output/writer/sh4/divx2.c output/writer/sh4/dts.c \
         output/writer/sh4/h263.c output/writer/sh4/h264.c output/writer/sh4/mp3.c \
         output/writer/sh4/mpeg2.c output/writer/sh4/pcm.c output/writer/sh4/vc1.c \
         output/writer/sh4/wma.c output/writer/sh4/wmv.c; do
    $cc -fsyntax-only -Werror=implicit-function-declaration -Werror=format \
        -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE \
        -DHAVE_FLV2MPEG4_CONVERTER "-DAUDIO_GET_PTS=_IOR('o', 19, __u64)" \
        -Wno-deprecated-declarations -Iinclude -Iexternal -Iexternal/flv2mpeg4 \
        -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" "$f"
done
# The sh4 writers with a WriteV of the test.
$cc -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/sh4_writers_test.c output/writer/sh4/pcm.c output/writer/sh4/h264.c output/writer/common/pes.c \
    output/writer/common/misc.c tools/debug.c \
    -Wl,--gc-sections -pthread -o "$tmp/sh4_writers"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/sh4_writers"
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
# Software decoded audio of the play thread, from a demuxer and a decoder of
# the test: the timestamps the PCM goes out with.
$cc -g -fsanitize=undefined -ffunction-sections -fdata-sections \
    -Wno-deprecated-declarations -Iinclude -Iexternal \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels external/ffmpeg/src/mpeg4audio.c \
    -I"$FFMPEG_BUILD" -I"$FFMPEG_SOURCE" \
    tests/audio_pts_test.c tools/debug.c tools/pcm_export.c tools/hevc_hdr.c \
    -Wl,--gc-sections -pthread "$FFMPEG_BUILD/libavformat/libavformat.a" \
    "$FFMPEG_BUILD/libavcodec/libavcodec.a" "$FFMPEG_BUILD/libswresample/libswresample.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" -lm ${FFMPEG_EXTRA_LIBS:-} -o "$tmp/audio_pts"
timeout 20 "$tmp/audio_pts"
# DreamNextGen (Dreambox One/Two): nothing of it runs on the host, the player
# with HAVE_DREAMNEXTGEN and its receiver outputs is only compiled and linked.
$cc -g -Werror=implicit-function-declaration -Werror=int-conversion \
    -Werror=incompatible-pointer-types -DHAVE_DREAMNEXTGEN "-DAUDIO_GET_PTS=_IOR('o', 19, __u64)" \
    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER \
    -Dff_mpeg4audio_channels=exteplayer3_mpeg4audio_channels \
    -Wno-deprecated-declarations -Iinclude -Iexternal -Iexternal/flv2mpeg4 \
    -I"$FFMPEG_SOURCE" -I"$FFMPEG_BUILD" \
    main/exteplayer.c tools/debug.c tools/strbuffer.c tools/pcm_export.c tools/hevc_hdr.c tools/ca_bundle.c \
    container/container.c container/container_ffmpeg.c \
    manager/manager.c manager/audio.c manager/video.c manager/subtitle.c \
    playback/playback.c output/output.c output/output_subtitle.c output/graphic_subtitle.c \
    output/linuxdvb_mipsel.c output/dream_video.c output/linuxdvb_buffering.c \
    output/writer/common/pes.c output/writer/common/misc.c output/writer/common/writer.c \
    output/writer/mipsel/*.c external/ffmpeg/src/bitstream.c \
    external/ffmpeg/src/latmenc.c external/ffmpeg/src/mpeg4audio.c \
    external/flv2mpeg4/src/*.c external/plugins/src/png.c \
    "$FFMPEG_BUILD/libavformat/libavformat.a" "$FFMPEG_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_BUILD/libswresample/libswresample.a" "$FFMPEG_BUILD/libswscale/libswscale.a" \
    "$FFMPEG_BUILD/libavutil/libavutil.a" \
    ${FFMPEG_EXTRA_LIBS:-} -pthread -lm -ldl -o "$tmp/dreamnextgen"
# The CA bundle of https inputs: when SSL_CERT_FILE is set at startup and
# to which file, with directories and bundles of the test.
$cc -g -fsanitize=address,undefined -Iinclude tests/ca_bundle_test.c tools/ca_bundle.c \
    -ldl -o "$tmp/ca_bundle"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$tmp/ca_bundle"
