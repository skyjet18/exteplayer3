#!/bin/bash
# Runs inside the jessie userland: configures, compiles and installs FFmpeg 9
# with the configure line of the packages in ipk/ (OpenPLi 4 build, FFmpeg
# 6.1), adapted to a native build and to FFmpeg 9.
#   /w/src/ffmpeg   source tree
#   /opt/ffstage    installation (DESTDIR, prefix /usr)
#   /w/out/ffmpeg   logs, config.h, the options used
# environment: ARCH (armhf, mipsel or arm64)
#
# Differences to the line of the package:
#   left out, cross compilation (this is a native build in the chroot):
#     --cross-prefix --ld --cxx --enable-cross-compile --sysroot,
#     --sysroot and -g in --extra-cflags (the package is stripped anyway)
#   left out as unknown to FFmpeg 9:
#     --disable-yasm, --enable-postproc (libpostproc was removed in FFmpeg 8)
#   left out on purpose:
#     --enable-libx264: FFmpeg 9 needs X264_BUILD >= 155 (jessie: 142), and
#     the line disables the libx264 encoder, the only user of the library
#     (libavcodec of the package does not even link it)
#   changed:
#     ARM --cc / --extra-cflags: -marm -mfpu=vfp added, the code generation
#     of the package (Debian's GCC defaults to Thumb-2 and VFPv3-D16);
#     MIPS: the flags of the package unchanged (-mel -mabi=32 -mhard-float
#     -march=mips32, no hash style)
#     AArch64 (no package in ipk/): --arch=aarch64 with the options of
#     OE-Alliance (--enable-armv8 --enable-vfp --enable-neon), -march=armv8-a
#     --enable-libsrt (libsrt 1.5.4 from /opt/deps)
#     --extra-ldflags: -L/-rpath-link of /opt/deps added
#     OpenSSL 3.5 instead of 1.0.1j, librtmp 2.6, libbluray 1.3.4,
#     libxml2 2.9.14 (all from /opt/deps)
set -eu
. /w/.github/ipk/versions.sh
SRC=/w/src/ffmpeg
OUT=/w/out/ffmpeg
B=/b/ffmpeg
STAGE=/opt/ffstage
RPATH=${FF_RPATH:-/usr/lib/exteplayer3_deps}
J=$(nproc)
rm -rf $OUT $B $STAGE
mkdir -p $OUT $B $STAGE

# the architecture options of the two packages, at their places in the line
case ${ARCH:-armhf} in
    armhf)
        FFARCH=arm
        ARCHOPTS=(--enable-armv6 --enable-armv6t2 --enable-vfp --enable-neon)
        LATEOPTS=(--disable-neon)
        ;;
    mipsel)
        FFARCH=mipsel
        ARCHOPTS=(--enable-mipsfpu --extra-libs=-latomic --disable-mips32r5 --disable-mipsdsp --disable-mipsdspr2
                  --disable-loongson2 --disable-loongson3 --disable-mmi --disable-msa)
        LATEOPTS=()
        ;;
    arm64)
        # as OE-Alliance for AArch64 machines (TUNE_FEATURES aarch64)
        FFARCH=aarch64
        ARCHOPTS=(--enable-armv8 --enable-vfp --enable-neon)
        LATEOPTS=()
        ;;
esac
# SRT (as OE-Alliance: srt in the PACKAGECONFIG of its FFmpeg 9 recipe)
SRTOPT=--enable-libsrt
SRTWANT=libsrt

ARGS=(--disable-stripping --enable-pic --enable-shared --enable-pthreads --enable-nonfree
      --cc="gcc $ARCHFLAGS" --arch=$FFARCH --target-os=linux
      --extra-cflags="-Os -pipe -feliminate-unused-debug-types $ARCHFLAGS -I/opt/deps/include -O2"
      --extra-ldflags="-L/opt/deps/lib -Wl,-rpath-link,/opt/deps/lib -Wl,-O1 $HASHSTYLE -Wl,--as-needed -Wl,-rpath,$RPATH"
      --prefix=/usr --disable-static --disable-runtime-cpudetect --enable-ffprobe
      --disable-altivec --disable-amd3dnow --disable-amd3dnowext --disable-mmx --disable-mmxext
      --disable-sse --disable-sse2 --disable-sse3 --disable-ssse3 --disable-sse4 --disable-sse42
      --disable-avx --disable-xop --disable-fma3 --disable-fma4 --disable-avx2
      --disable-inline-asm --disable-x86asm --disable-fast-unaligned
      --enable-protocol=http --enable-demuxer=dash --disable-muxers
      --enable-muxer=adts --enable-muxer=mpeg1video --enable-muxer=h264 --enable-muxer=mp4
      --enable-muxer=image2 --enable-muxer=mjpeg --enable-muxer=rawvideo --enable-muxer=mpeg2video
      --enable-muxer=matroska --enable-muxer=m4v --enable-muxer=image2pipe --enable-muxer=apng
      --enable-muxer=mpegts --enable-muxer=asf --enable-muxer=spdif
      --disable-encoders --disable-encoder=libx264 --enable-encoder=ac3 --enable-encoder=eac3 --enable-encoder=aac
      --enable-encoder=mpeg1video --enable-encoder=ljpeg --enable-encoder=mjpeg --enable-encoder=mpeg4
      --enable-encoder=jpeg2000 --enable-encoder=jpegls --enable-encoder=png --enable-encoder=rawvideo
      --enable-encoder=wmav2 --enable-decoder=truehd --enable-decoder=mlp
      --disable-debug --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages
      "${ARCHOPTS[@]}"
      --libdir=/usr/lib --shlibdir=/usr/lib --datadir=/usr/share/ffmpeg
      --disable-mipsdsp --disable-mipsdspr2 --cpu=generic --pkg-config=pkg-config
      ${LATEOPTS[@]+"${LATEOPTS[@]}"} --enable-alsa --disable-altivec --enable-avcodec --enable-avdevice --enable-avfilter
      --enable-avformat --enable-bzlib --disable-libdav1d --disable-libfdk-aac --enable-gpl --disable-libgsm
      --disable-indev=jack --enable-libbluray --enable-protocol=bluray --enable-libfreetype --disable-libopus
      --enable-librtmp --disable-libv4l2 --disable-libvorbis --enable-libxml2 --enable-lzma --disable-libmfx
      --disable-libmp3lame --enable-openssl --disable-sdl2 --disable-libspeex $SRTOPT
      --enable-swresample --enable-swscale --enable-libtheora --disable-libv4l2 --disable-vaapi
      --disable-vdpau --disable-libvpx --disable-libx265 --disable-libxcb --disable-outdev=xv --enable-zlib)
printf '%s\n' "${ARGS[@]}" > $OUT/options.txt

echo "======== configure"
cd $B
if ! env PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig $SRC/configure "${ARGS[@]}" > $OUT/configure.log 2>&1; then
    tail -30 $OUT/configure.log
    tail -60 ffbuild/config.log
    cp ffbuild/config.log $OUT/
    exit 1
fi
cp ffbuild/config.log config.h ffbuild/config.mak $OUT/
grep -E '^(ARCH|C compiler|C library|runtime cpu detection|standalone assembly|inline assembly|ARMv|VFP|NEON|THUMB|MIPS|MMI|MSA|License|network support)' $OUT/configure.log
sed -n '/^External libraries:/,/^$/p' $OUT/configure.log
# what the line asks for and configure did not enable
for want in alsa bzlib libbluray libfreetype librtmp libtheora libxml2 lzma openssl zlib $SRTWANT; do
    grep -q "^#define CONFIG_$(echo $want | tr a-z A-Z) 1" config.h || { echo "ERROR: $want not enabled"; exit 1; }
done
# libavformat/libsrt.c uses IN6_IS_ADDR_* (via srt.h); in glibc 2.19 these
# macros need s6_addr32, which <netinet/in.h> only defines with __USE_MISC,
# not with the _XOPEN_SOURCE=600 of FFmpeg: _GNU_SOURCE for this file only
if [ -n "$SRTWANT" ]; then
    echo 'libavformat/libsrt.o: CPPFLAGS += -D_GNU_SOURCE' >> ffbuild/config.mak
fi

echo "======== make"
START=$(date +%s)
if ! make -j$J > $OUT/make.log 2>&1; then
    grep -E ': (fatal )?error: |undefined reference' $OUT/make.log | head -40
    tail -30 $OUT/make.log
    exit 1
fi
echo "make: $(( ($(date +%s) - START) / 60 )) minutes"
grep -c ': warning: ' $OUT/make.log || true
make install DESTDIR=$STAGE > $OUT/install.log 2>&1
rm -rf $STAGE/usr/share/ffmpeg/examples
ls -la $STAGE/usr/bin $STAGE/usr/lib
cp $OUT/options.txt $STAGE/FFMPEG_OPTIONS
cp $SRC/RELEASE $STAGE/FFMPEG_RELEASE
