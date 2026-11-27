# Versions of the libraries built from source and bundled with the player
# (sourced by the other scripts; the rootfs cache key contains this file).
# OpenSSL: 3.5 is the LTS series (support until April 2030). FFmpeg 6.1 and
# FFmpeg 9 both build with it; FFmpeg 9 needs >= 1.1.1, so the OpenSSL 1.0.x
# of the earlier packages is not possible there any more.
OPENSSL_VERSION=3.5.9
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
# libxml2 of the generation of the package (2.9.2), last 2.9 release
LIBXML2_VERSION=2.9.14
LIBXML2_URL=https://download.gnome.org/sources/libxml2/2.9/libxml2-2.9.14.tar.xz
LIBXML2_SHA256=60d74a257d1ccec0475e749cba2f21559e48139efba6ff28224357c7c798dfee
# libbluray 1.3.x (libbluray.so.2, as in the package) and its libudfread
UDFREAD_VERSION=1.1.2
UDFREAD_URL=https://download.videolan.org/pub/videolan/libudfread/libudfread-1.1.2.tar.bz2
UDFREAD_SHA256=d5946f19b27288ce0911eb11fe583c136767421220ece3bafc96245efd208915
BLURAY_VERSION=1.3.4
BLURAY_URL=https://download.videolan.org/pub/videolan/libbluray/1.3.4/libbluray-1.3.4.tar.bz2
BLURAY_SHA256=478ffd68a0f5dde8ef6ca989b7f035b5a0a22c599142e5cd3ff7b03bbebe5f2b
# librtmp of rtmpdump 2.6 (OpenSSL 3 support), no release tarballs
RTMPDUMP_GIT=https://git.ffmpeg.org/rtmpdump.git
RTMPDUMP_COMMIT=138fdb258d9fc26f1843fd1b891180416c9dc575
# libsrt for the ARM and AArch64 packages (OE-Alliance enables SRT in its
# FFmpeg 9 recipe). 1.5.4 configures with the CMake 3.0 of jessie; the current
# release 1.5.7 needs CMake 3.5
SRT_VERSION=1.5.4
SRT_URL=https://github.com/Haivision/srt/archive/refs/tags/v1.5.4.tar.gz
SRT_SHA256=d0a8b600fe1b4eaaf6277530e3cfc8f15b8ce4035f16af4a5eb5d4b123640cdd
# target (ARCH: Debian architecture armhf, mipsel or arm64, armhf by default)
case ${ARCH:-armhf} in
    armhf)
        # code generation of the package: ARMv7-A, ARM (not Thumb) mode,
        # VFPv2, hard-float (read from the .ARM.attributes of its libraries)
        ARCHFLAGS="-march=armv7-a -mfloat-abi=hard -marm -mfpu=vfp"
        QEMU=arm
        TRIPLET=arm-linux-gnueabihf
        OPENSSL_TARGET=linux-armv4
        HASHSTYLE="-Wl,--hash-style=gnu"
        LABEL=armv7ahf
        ;;
    mipsel)
        # the MIPS package: MIPS32 (release 1), o32, hard-float
        ARCHFLAGS="-mel -mabi=32 -mhard-float -march=mips32"
        QEMU=mipsel
        TRIPLET=mipsel-linux-gnu
        OPENSSL_TARGET=linux-mips32
        # MIPS has no GNU hash style (not in the line of the package either)
        HASHSTYLE=""
        LABEL=mips32el
        ;;
    arm64)
        # 64-bit ARM (Dreambox One/Two): ARMv8-A, the AArch64 baseline
        ARCHFLAGS="-march=armv8-a"
        QEMU=aarch64
        TRIPLET=aarch64-linux-gnu
        OPENSSL_TARGET=linux-aarch64
        HASHSTYLE="-Wl,--hash-style=gnu"
        LABEL=aarch64
        ;;
esac
