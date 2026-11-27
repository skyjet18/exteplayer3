#!/bin/bash
# Build environment of the ipk, made on the runner: a Debian 8 "jessie"
# armhf, mipsel or arm64 userland that runs under qemu-user (GCC 4.9.2,
# glibc 2.19, binutils 2.25, kernel headers 3.16), the toolchain generation
# of OpenPLi 4
# (GCC 4.9.1, glibc 2.20) the packages in ipk/ were built with. Everything
# built in it needs glibc <= 2.19, so it runs on old and new images alike.
#
# The libraries that are bundled with the player are compiled from source
# into /opt/deps (deps.sh); the jessie packages provide only what the image
# has to provide itself (zlib, liblzma, ALSA, FreeType) and the build tools.
# Not installed on purpose: libssl-dev, libxml2-dev, librtmp-dev,
# libbluray-dev (the versions of /opt/deps must be the only ones).
#
# usage: rootfs.sh <directory> <archive.tar.zst>   (environment: ARCH, armhf,
#        mipsel or arm64, armhf by default)
set -eux
R=$1
TARBALL=$2
HERE=$(cd "$(dirname "$0")" && pwd)
export ARCH=${ARCH:-armhf}
. "$HERE/versions.sh"

sudo apt-get update -q
sudo apt-get install -y -q qemu-user-static debootstrap zstd
bash "$HERE/binfmt.sh" $QEMU

sudo debootstrap --arch=$ARCH --variant=minbase --no-check-gpg jessie "$R" http://archive.debian.org/debian
bash "$HERE/binfmt.sh" $QEMU "$R"

echo 'deb http://archive.debian.org/debian jessie main' | sudo tee "$R/etc/apt/sources.list"
sudo tee "$R/etc/apt/apt.conf.d/99old" <<'EOF'
Acquire::Check-Valid-Until "false";
APT::Get::AllowUnauthenticated "true";
APT::Install-Recommends "false";
EOF
sudo cp -L /etc/resolv.conf "$R/etc/resolv.conf"
sudo mount -t proc proc "$R/proc"
sudo chroot "$R" apt-get update -q
sudo chroot "$R" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --force-yes \
    build-essential pkg-config perl file chrpath binutils make cmake \
    zlib1g-dev libbz2-dev liblzma-dev libasound2-dev libfreetype6-dev libtheora-dev
sudo chroot "$R" apt-get clean

# sources of the bundled libraries, fetched and checked on the runner
sudo mkdir -p "$R/build"
SRCS=/tmp/depsrc
mkdir -p $SRCS
fetch() {
    # $1 url, $2 sha256
    local f
    f=$SRCS/$(basename "$1")
    curl -sSL --retry 5 --retry-all-errors -o "$f" "$1"
    echo "$2  $f" | sha256sum -c
    sudo tar -xf "$f" -C "$R/build"
}
fetch "$OPENSSL_URL" "$OPENSSL_SHA256"
fetch "$LIBXML2_URL" "$LIBXML2_SHA256"
fetch "$UDFREAD_URL" "$UDFREAD_SHA256"
fetch "$BLURAY_URL" "$BLURAY_SHA256"
fetch "$SRT_URL" "$SRT_SHA256"
git init -q $SRCS/rtmpdump
# git.ffmpeg.org answers with an HTTP error now and then: a few attempts
for i in 1 2 3 4 5; do
    git -C $SRCS/rtmpdump fetch -q --depth 1 "$RTMPDUMP_GIT" "$RTMPDUMP_COMMIT" && break
    [ $i = 5 ] && exit 1
    sleep 30
done
git -C $SRCS/rtmpdump checkout -q FETCH_HEAD
test "$(git -C $SRCS/rtmpdump rev-parse HEAD)" = "$RTMPDUMP_COMMIT"
sudo cp -r $SRCS/rtmpdump "$R/build/rtmpdump"
sudo rm -rf "$R/build/rtmpdump/.git"
git init -q $SRCS/x264
for i in 1 2 3 4 5; do
    git -C $SRCS/x264 fetch -q --depth 1 "$X264_GIT" "$X264_COMMIT" && break
    [ $i = 5 ] && exit 1
    sleep 30
done
git -C $SRCS/x264 checkout -q FETCH_HEAD
test "$(git -C $SRCS/x264 rev-parse HEAD)" = "$X264_COMMIT"
sudo cp -r $SRCS/x264 "$R/build/x264"
sudo rm -rf "$R/build/x264/.git"

sudo cp "$HERE/versions.sh" "$HERE/deps.sh" "$R/build/"
sudo chroot "$R" /usr/bin/env ARCH=$ARCH /bin/bash /build/deps.sh
sudo rm -rf "$R/build"

sudo tee "$R/versions.sh" <<'EOF'
echo "gcc:      $(gcc --version | head -1)"
echo "binutils: $(ld --version | head -1)"
echo "glibc:    $(ldd --version | head -1)"
echo "headers:  linux-libc-dev $(dpkg-query -W -f='${Version}' linux-libc-dev)"
echo "system:   $(dpkg-query -W -f='${Package} ${Version}, ' zlib1g-dev libbz2-dev liblzma-dev libasound2-dev libfreetype6-dev libtheora-dev)"
echo "/opt/deps:"
cat /opt/deps/VERSIONS
EOF
sudo chroot "$R" /bin/bash /versions.sh

sudo umount "$R/proc"
sudo tar --numeric-owner -C "$R" -c . | zstd -T0 -6 > "$TARBALL"
ls -la "$TARBALL"
