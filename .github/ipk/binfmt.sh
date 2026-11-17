#!/bin/bash
# Makes sure the kernel starts programs of the target architecture with
# qemu-user. With a directory: copies the interpreter into that root, for the
# case that the registration does not keep the interpreter open.
# usage: binfmt.sh <arm|mipsel> [root directory]
set -eu
Q=$1
E=/proc/sys/fs/binfmt_misc/qemu-$Q
if [ ! -e $E ]; then
    sudo systemctl restart systemd-binfmt || true
fi
if [ ! -e $E ]; then
    docker run --privileged --rm tonistiigi/binfmt --install all
fi
cat $E
if [ -n "${2:-}" ]; then
    I=$(sed -n 's/^interpreter //p' $E)
    sudo install -D -m 755 "$I" "$2$I"
fi
