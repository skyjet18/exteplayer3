SUMMARY = "exteplayer3 - media player for E2"
DESCRIPTION = "Core of movie player for E2 based on the libeplayer using the ffmpeg solution"
SECTION = "multimedia"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://main/exteplayer.c;beginline=4;endline=17;md5=6043da836f7c3bed33478eac4c2f5b2e"

# "ffmpeg" = FFmpeg of the image, "ffmpeg-ext" = patched FFmpeg installed
# in /usr/lib/ffmpeg-ext (OE-Alliance)
EXTEPLAYER3_FFMPEG ?= "ffmpeg"

DEPENDS = "${EXTEPLAYER3_FFMPEG}"

inherit gitpkgv pkgconfig

SRCREV = "${AUTOREV}"
PV = "186+git${SRCPV}"
PKGV = "186+git${GITPKGV}"

SRC_URI = "git://github.com/skyjet18/exteplayer3.git;branch=master;protocol=https;destsuffix=git"

S = "${@(d.getVar('UNPACKDIR', True) or d.getVar('WORKDIR', True)) + '/git'}"

# shell globs: new source files in these directories need no recipe change
SOURCE_FILES = "main/*.c tools/*.c"
SOURCE_FILES += "container/container.c container/container_ffmpeg.c"
SOURCE_FILES += "manager/*.c playback/*.c"
SOURCE_FILES += "output/output.c output/output_subtitle.c output/graphic_subtitle.c output/linuxdvb_buffering.c"
SOURCE_FILES += "output/writer/common/*.c"
SOURCE_FILES += "external/ffmpeg/src/*.c external/flv2mpeg4/src/*.c external/plugins/src/*.c"

SOURCE_FILES += "${@bb.utils.contains("TARGET_ARCH", "sh4", "\
output/linuxdvb_sh4.c \
output/writer/sh4/writer.c \
output/writer/sh4/aac.c \
output/writer/sh4/ac3.c \
output/writer/sh4/divx2.c \
output/writer/sh4/dts.c \
output/writer/sh4/h263.c \
output/writer/sh4/h264.c \
output/writer/sh4/mp3.c \
output/writer/sh4/mpeg2.c \
output/writer/sh4/pcm.c \
output/writer/sh4/vc1.c \
output/writer/sh4/wma.c \
output/writer/sh4/wmv.c ", " \
output/linuxdvb_mipsel.c \
output/writer/mipsel/*.c ", d)}"

FFMPEG_LIBS = "libavformat libavcodec libswresample libswscale libavutil"

do_compile() {
    cd ${S}
    FFMPEG_LDFLAGS=""
    if [ "${EXTEPLAYER3_FFMPEG}" = "ffmpeg-ext" ]; then
        export PKG_CONFIG_PATH="${STAGING_LIBDIR}/ffmpeg-ext/pkgconfig:$PKG_CONFIG_PATH"
        FFMPEG_LDFLAGS="-Wl,-rpath,${libdir}/ffmpeg-ext -Wl,-rpath-link,${STAGING_LIBDIR}/ffmpeg-ext"
    fi
    # linux/dvb/audio.h has AUDIO_GET_PTS up to Linux 4.17 only
    ${CC} ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} $FFMPEG_LDFLAGS \
        -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER \
        "-DAUDIO_GET_PTS=_IOR('o', 19, __u64)" \
        -I${S}/include -I${S}/external -I${S}/external/flv2mpeg4 \
        $(pkg-config --cflags ${FFMPEG_LIBS}) \
        ${SOURCE_FILES} \
        $(pkg-config --libs ${FFMPEG_LIBS}) -lpthread -ldl -lm \
        -o ${B}/exteplayer3
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/exteplayer3 ${D}${bindir}
}
