#!/bin/bash
# Runs on the runner after player.sh: manifest of the ELF files (with the
# GLIBC check) and the ipk, laid out as the packages in ipk/:
#   /usr/bin/exteplayer3
#   /usr/lib/exteplayer3_deps/   FFmpeg, ffmpeg, ffprobe and the libraries
#                                the image does not provide
# usage: package.sh <out/player> <dist directory>
# environment: ARCH, VERSION, MAX_GLIBC, REPOSITORY, PLAYER_SHA, FFMPEG_REPO,
#              FFMPEG_VERSION (6.1 or 9), FFMPEG_BRANCH, FFMPEG_SHA
# Writes exteplayer3_<VERSION>_ffmpeg<6|9>_<armv7ahf|mips32el>.ipk,
# .manifest.txt and .buildinfo.txt into the dist directory. Both FFmpeg
# variants are the package exteplayer3 (they replace the player of the image
# and each other). Exit status 1 when a file needs a
# newer glibc than MAX_GLIBC.
set -eux
P=$1
mkdir -p "$2"
DIST=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
export ARCH=${ARCH:-armhf}
. "$HERE/versions.sh"
BD=$P/bundle
FFMPEG_RELEASE=$(cat "$P/FFMPEG_RELEASE" 2> /dev/null || echo "?")

rc=0
python3 "$HERE/elfinfo.py" --max-glibc "$MAX_GLIBC" --bundled-dir "$BD/lib/exteplayer3_deps" \
    --json "$P/manifest.json" "$BD" > "$P/manifest.txt" || rc=$?
sed -i "s#^== $BD/#== #" "$P/manifest.txt"
cat "$P/manifest.txt"
GLIBC=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['glibc_max'])" "$P/manifest.json")
{
    echo "exteplayer3: $REPOSITORY $PLAYER_SHA (version $VERSION)"
    echo "ffmpeg:      $FFMPEG_REPO $FFMPEG_BRANCH $FFMPEG_SHA ($FFMPEG_RELEASE)"
    echo "rpath:       /usr/lib/exteplayer3_deps"
    echo "glibc:       highest GLIBC_ version required: $GLIBC (limit $MAX_GLIBC)"
    echo "built:       Debian 8 $ARCH userland under qemu-user, GCC 4.9.2, glibc 2.19, $(date -u +%Y-%m-%d)"
    echo "libraries:"
    sed 's/^/  /' "$BD/lib/exteplayer3_deps.versions" 2> /dev/null
    echo "smoke tests (qemu-user):"
    sed 's/^/  /' "$P/smoke.txt"
} > "$P/buildinfo.txt"
cat "$P/buildinfo.txt"

T=exteplayer3_${VERSION}_ffmpeg${FFMPEG_VERSION%%.*}_$LABEL
# Architecture as in the packages of ipk/: "all" for ARM (one package for
# all ARM images), mips32el for MIPS
case $ARCH in
    armhf) IPK_ARCH=all ;;
    mipsel) IPK_ARCH=mips32el ;;
esac
W=$(mktemp -d)
mkdir -p "$W/data/usr/bin" "$W/data/usr/lib" "$W/control"
cp -a "$BD/bin/exteplayer3" "$W/data/usr/bin/"
cp -a "$BD/lib/exteplayer3_deps" "$W/data/usr/lib/"
cat > "$W/control/control" <<EOF
Package: exteplayer3
Version: $VERSION
Description: exteplayer3 - media player for E2 (FFmpeg $FFMPEG_RELEASE)
 Core of movie player for E2 based on the libeplayer using the ffmpeg
 solution. Built with FFmpeg $FFMPEG_RELEASE ($FFMPEG_REPO $FFMPEG_BRANCH).
Section: multimedia
Priority: optional
Maintainer: OE-Core Developers <openembedded-core@lists.openembedded.org>
License: GPL-2.0
Architecture: $IPK_ARCH
OE: exteplayer3
Depends: libc6 (>= 2.20)
Source: https://github.com/$REPOSITORY/tree/$PLAYER_SHA
EOF
cat "$W/control/control"
echo "2.0" > "$W/debian-binary"
tar --owner=0 --group=0 --numeric-owner -C "$W/control" -czf "$W/control.tar.gz" .
tar --owner=0 --group=0 --numeric-owner -C "$W/data" -czf "$W/data.tar.gz" .
rm -f "$DIST/$T.ipk"
( cd "$W" && ar -r "$DIST/$T.ipk" debian-binary control.tar.gz data.tar.gz 2> /dev/null )
rm -rf "$W"
cp "$P/manifest.txt" "$DIST/$T.manifest.txt"
cp "$P/buildinfo.txt" "$DIST/$T.buildinfo.txt"
ls -la "$DIST"

{
    echo "### $T.ipk"
    echo '```'
    cat "$P/buildinfo.txt"
    echo
    grep -A40 '^highest GLIBC' "$P/manifest.txt"
    echo '```'
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
exit $rc
