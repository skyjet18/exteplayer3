#!/bin/bash
# Prepares the jessie userland on the runner: qemu-user for the target
# architecture, the root file system of rootfs.sh unpacked to $R, /proc and
# the workspace (this repository) mounted at $R/w; with a second archive,
# the FFmpeg installation of ffmpeg.sh unpacked to $R/opt/ffstage.
# Programs then run with: sudo chroot $R ...
# usage: chroot.sh <rootfs.tar.zst> [ffstage.tar.zst]
# environment: ARCH (armhf or mipsel), R (directory of the root)
set -eux
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/versions.sh"
: "${R:?}"
sudo apt-get update -q
sudo apt-get install -y -q qemu-user-static zstd
bash "$HERE/binfmt.sh" $QEMU
sudo mkdir -p "$R"
sudo tar --numeric-owner -I zstd -xpf "$1" -C "$R"
rm -f "$1"
if [ -n "${2:-}" ]; then
    sudo mkdir -p "$R/opt/ffstage"
    sudo tar --numeric-owner -I zstd -xpf "$2" -C "$R/opt/ffstage"
    rm -f "$2"
fi
sudo mount -t proc proc "$R/proc"
sudo mkdir -p "$R/w"
sudo mount --bind "$GITHUB_WORKSPACE" "$R/w"
sudo chroot "$R" /bin/bash /versions.sh
