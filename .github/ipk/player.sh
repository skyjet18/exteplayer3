#!/bin/bash
# Runs inside the jessie userland: compiles exteplayer3 against the FFmpeg of
# /opt/ffstage, puts the files of the ipk together, installs them where the
# ipk puts them and runs smoke tests under qemu-user.
#   /w              this repository (the scripts)
#   $PLAYER_SRC     the exteplayer3 source to build (default /w; another
#                   commit, e.g. of an older version, checked out there)
#   /opt/ffstage    FFmpeg (ffmpeg.sh), /opt/deps the other libraries
#   /samples        test media, crt.pem of the HTTPS test server
#   /w/out/player   bundle/ (bin/exteplayer3, lib/exteplayer3_deps/), logs,
#                   smoke.txt
# environment: ARCH (armhf, mipsel or arm64), HTTPS_PORT, PLAYER_SRC
# Exit status 1 when compiling, bundling or a smoke test fails.
set -u
. /w/.github/ipk/versions.sh
PLAYER_SRC=${PLAYER_SRC:-/w}
# where the ipk installs the libraries; RPATH of the player and of FFmpeg
DEPS_RPATH=/usr/lib/exteplayer3_deps
FF=/opt/ffstage
OUT=/w/out/player
BD=$OUT/bundle
L=$BD/lib/exteplayer3_deps
# NEEDED libraries the image has to provide (the packages in ipk/ take the
# same ones from the image); everything else is bundled. The MIPS
# package bundles liblzma itself, so does this one. libstdc++ (needed by
# libsrt, ARM and AArch64): every Enigma2 image has it (Enigma2 is
# C++); the one of jessie (GCC 4.9) sets the highest GLIBCXX_ version used.
# Player sources and defines per architecture: the AArch64 receivers with
# Enigma2 are the Dreambox One/Two, their video output is the DreamNextGen
# backend (HAVE_DREAMNEXTGEN, output/dream_video.c), as OE-Alliance builds
# it for dreamone/dreamtwo.
PLAYER_DEFS=""
PLAYER_EXTRA=""
case ${ARCH:-armhf} in
    armhf)
        IMAGE_LIBS="ld-linux-armhf.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libgcc_s.so.1
                    libstdc++.so.6 libz.so.1 liblzma.so.5 libasound.so.2 libfreetype.so.6" ;;
    mipsel)
        IMAGE_LIBS="ld.so.1 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libgcc_s.so.1
                    libstdc++.so.6 libz.so.1 libasound.so.2 libfreetype.so.6" ;;
    arm64)
        IMAGE_LIBS="ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libgcc_s.so.1
                    libstdc++.so.6 libz.so.1 liblzma.so.5 libasound.so.2 libfreetype.so.6"
        # -std=gnu11: dream_video.c declares variables in for loops; GCC
        # 4.9 defaults to gnu89, the GCC of OE-Alliance to gnu17
        PLAYER_DEFS="-std=gnu11 -DHAVE_DREAMNEXTGEN"
        PLAYER_EXTRA="output/dream_video.c" ;;
esac
rm -rf $OUT
mkdir -p $OUT $BD/bin $L
SMOKE=$OUT/smoke.txt
: > $SMOKE
FAILED=0

res() {
    echo "$1=$2" >> $SMOKE
    echo "RESULT $1=$2"
}
fail() {
    res "$1" "FAILED: $2"
    FAILED=1
}

echo "======== compile"
cd "$PLAYER_SRC"
git -C "$PLAYER_SRC" log -1 --format='source: %H %s' 2> /dev/null || echo "source: $PLAYER_SRC"
# the source list of the recipe (exteplayer3_git.bb, shell globs); names
# an older tree does not have (a glob without a match) are left out
SOURCES=""
for f in main/*.c tools/*.c container/container.c container/container_ffmpeg.c \
    manager/*.c playback/*.c \
    output/output.c output/output_subtitle.c output/graphic_subtitle.c output/linuxdvb_buffering.c \
    output/writer/common/*.c external/ffmpeg/src/*.c external/flv2mpeg4/src/*.c external/plugins/src/*.c \
    output/linuxdvb_mipsel.c output/writer/mipsel/*.c $PLAYER_EXTRA; do
    if [ -f "$f" ]; then
        SOURCES="$SOURCES $f"
    else
        echo "not in this tree: $f"
    fi
done
echo "$SOURCES" | tr ' ' '\n' | sed '/^$/d' > $OUT/sources.txt
for f in $PLAYER_EXTRA; do
    if [ ! -f "$f" ]; then
        res compile "FAILED: $f is not in this tree ($PLAYER_DEFS for $ARCH)"
        exit 1
    fi
done
echo "defines: ${PLAYER_DEFS:-none}"
gcc $ARCHFLAGS -Os -pipe -O2 -Wl,-O1 $HASHSTYLE -Wl,--as-needed -Wl,--disable-new-dtags \
    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER $PLAYER_DEFS \
    "-DAUDIO_GET_PTS=_IOR('o', 19, __u64)" \
    -Iinclude -Iexternal -Iexternal/flv2mpeg4 -I$FF/usr/include \
    $SOURCES \
    -L$FF/usr/lib -Wl,-rpath-link,$FF/usr/lib:/opt/deps/lib "-Wl,-rpath,$DEPS_RPATH" \
    -lavformat -lavcodec -lswresample -lswscale -lavutil -lpthread -ldl -lm \
    -o $OUT/exteplayer3 > $OUT/compile.log 2>&1
rc=$?
res compile "exit $rc, $(grep -c 'warning:' $OUT/compile.log) warnings"
if [ $rc != 0 ]; then
    grep -E 'error|undefined reference' $OUT/compile.log | head -40
    exit 1
fi
grep -E 'warning: (implicit declaration|.*incompatible pointer|.*makes (integer|pointer)|cast (to|from) pointer|format .* expects)' $OUT/compile.log | head -30

echo "======== bundle"
cp $FF/FFMPEG_RELEASE $OUT/ 2> /dev/null
cp $OUT/exteplayer3 $BD/bin/
cp $FF/usr/bin/ffmpeg $FF/usr/bin/ffprobe $L/
soname() {
    objdump -p "$1" | sed -n 's/^ *SONAME *//p'
}
for f in $FF/usr/lib/lib*.so.*; do
    [ -L "$f" ] && continue
    cp "$f" "$L/$(soname "$f")"
done
needed() {
    objdump -p "$1" | sed -n 's/^ *NEEDED *//p'
}
find_lib() {
    local d
    for d in /opt/deps/lib /lib/$TRIPLET /usr/lib/$TRIPLET /lib /usr/lib; do
        [ -e "$d/$1" ] && { readlink -f "$d/$1"; return 0; }
    done
    return 1
}
# what the files need and the image does not provide, until nothing is added
while :; do
    added=0
    for f in $BD/bin/exteplayer3 $L/*; do
        for n in $(needed "$f"); do
            [ -e "$L/$n" ] && continue
            case " $(echo $IMAGE_LIBS) " in *" $n "*) continue ;; esac
            if src=$(find_lib "$n"); then
                cp "$src" "$L/$n"
                echo "bundled $n from $src (needed by $(basename "$f"))"
                added=1
            else
                fail bundle "$n (needed by $(basename "$f")) not found"
            fi
        done
    done
    [ $added = 0 ] && break
done
# file modes as in the packages of ipk/
chmod 755 $BD/bin/exteplayer3 $L/ffmpeg $L/ffprobe $L/lib*
# as the OpenEmbedded packaging does
strip --remove-section=.comment --remove-section=.note $BD/bin/exteplayer3 $L/ffmpeg $L/ffprobe
strip --remove-section=.comment --remove-section=.note --strip-unneeded $L/lib*
# RPATH of the bundled files: FFmpeg was linked with /usr/lib/exteplayer3_deps,
# libraries that have another one get it replaced
LIBRPATH=$DEPS_RPATH
for f in $L/*; do
    old=$(objdump -p "$f" | sed -n 's/^ *R\(UN\)\{0,1\}PATH *//p')
    [ -z "$old" ] && continue
    [ "$old" = "$LIBRPATH" ] && continue
    chrpath -r "$LIBRPATH" "$f" > /dev/null 2>&1 || chrpath -d "$f" > /dev/null
    echo "$(basename "$f"): RPATH $old -> $(objdump -p "$f" | sed -n 's/^ *R\(UN\)\{0,1\}PATH *//p')"
done
cp /opt/deps/VERSIONS $L/../exteplayer3_deps.versions 2> /dev/null
ls -la $BD/bin $L
res bundle_libs "$(cd $L && ls | tr '\n' ' ')"

echo "======== smoke tests under qemu-user"
# installed where the ipk installs them
rm -rf "$DEPS_RPATH" /usr/bin/exteplayer3
mkdir -p "$DEPS_RPATH"
cp -a $BD/bin/exteplayer3 /usr/bin/
cp -a $L/. "$DEPS_RPATH"/
EXE=/usr/bin/exteplayer3
LIBDIR=$DEPS_RPATH
res test_location "player $EXE, libraries $LIBDIR"
unset LD_LIBRARY_PATH

# which file each NEEDED library of the bundle resolves to: the bundle itself
# or the image set (dependencies of the image libraries are not checked)
DIRECT=" $(for f in $EXE $LIBDIR/*; do needed "$f"; done | sort -u | tr '\n' ' ') "
REALDIR=$(readlink -f "$LIBDIR")
check_resolved() {
    # stdin: ldd output; prints the direct NEEDED libraries that are missing
    # or come neither from the bundle nor from the image set
    local lib arrow path rest
    while read -r lib arrow path rest; do
        case "$lib $arrow $path $rest" in
            *"not found"*) echo "$lib not found"; continue ;;
        esac
        [ "$arrow" = "=>" ] || continue
        [ "${path#/}" != "$path" ] || continue
        [ "${DIRECT/ $lib /}" != "$DIRECT" ] || continue
        [ "$(dirname "$(readlink -f "$path")")" = "$REALDIR" ] && continue
        local img=" $(echo $IMAGE_LIBS) "
        [ "${img/ $lib /}" != "$img" ] && continue
        echo "$lib from $path"
    done
}
for p in $EXE $LIBDIR/ffmpeg $LIBDIR/ffprobe; do
    out=$(LD_TRACE_LOADED_OBJECTS=1 $p 2>&1)
    echo "---- $p"
    echo "$out"
    bad=$(echo "$out" | check_resolved)
    if [ -n "$bad" ]; then
        fail "ldd_$(basename $p)" "$(echo $bad)"
    else
        res "ldd_$(basename $p)" "ok: $(echo "$out" | awk '$2 == "=>" {print $1 "->" $3}' | sed "s#$LIBDIR/#BUNDLE/#" | tr '\n' ' ')"
    fi
done

run() {
    # $1 name, $2 required pattern (empty: exit status 0), rest: command
    local name=$1 pat=$2 out rc
    shift 2
    out=$("$@" 2>&1 < /dev/null)
    rc=$?
    echo "---- $name (exit $rc): $*"
    echo "$out" | tail -25
    local line
    line="exit $rc: $(echo "$out" | grep -v '^$' | tail -8 | tr '\n' '|' | cut -c1-500)"
    if [ -n "$pat" ]; then
        if echo "$out" | grep -qE "$pat"; then res "$name" "ok, $line"; else fail "$name" "$line"; fi
    else
        if [ $rc = 0 ]; then res "$name" "ok, $line"; else fail "$name" "$line"; fi
    fi
}
FFM=$LIBDIR/ffmpeg
FFP=$LIBDIR/ffprobe
run usage 'Usage: exteplayer3' $EXE
run ffmpeg_version '^ffmpeg version' sh -c "$FFM -version | head -3"
run ffmpeg_input_ts 'Stream #0:0.*Video: h264' $FFM -hide_banner -i /samples/sample.ts
run ffmpeg_input_mp4 'Stream #0:1.*Audio: aac' $FFM -hide_banner -i /samples/sample.mp4
run decode_h264 '' $FFM -v error -i /samples/sample.mp4 -map 0:v -c:v rawvideo -f rawvideo -y /dev/null
run decode_aac '' $FFM -v error -i /samples/sample.ts -map 0:a -c:a aac -f adts -y /dev/null
run probe_hls_file '^h264' $FFP -v error -show_entries stream=codec_name -of csv=p=0 /samples/hls/master.m3u8
run protocols 'https' sh -c "$FFM -hide_banner -protocols 2>/dev/null | tr -d ' ' | grep -xE 'https|tls|rtmp|rtmps|ffrtmp|librtmp|bluray|srt' | sort -u | tr '\n' ' '"
if grep -q -- '--enable-libsrt' $FF/FFMPEG_OPTIONS 2> /dev/null; then
    run protocol_srt '^srt$' sh -c "$FFM -hide_banner -protocols 2>/dev/null | tr -d ' ' | grep -x srt"
fi
# H.264 encoding with libx264 (ArchivCZSK renders short clips with it): ten
# frames to MPEG-TS, read back as H.264
rm -f /tmp/x264.ts
run encode_libx264 '' $FFM -v error -f rawvideo -pix_fmt yuv420p -s 64x64 -r 25 -i /dev/zero -frames:v 10 \
    -c:v libx264 -preset veryfast -f mpegts -y /tmp/x264.ts
run encode_libx264_check '^h264$' $FFP -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 /tmp/x264.ts
rm -f /tmp/x264.ts
U=https://127.0.0.1:${HTTPS_PORT:-8443}
run https_noverify '^h264' $FFP -v error -tls_verify 0 -show_entries stream=codec_name -of csv=p=0 $U/hls/master.m3u8
# default verification with the CA file of OPENSSLDIR (/etc/ssl/cert.pem),
# as the image provides it
mkdir -p /etc/ssl
cp /samples/crt.pem /etc/ssl/cert.pem
run https_default_ca '^h264' $FFP -v error -show_entries stream=codec_name -of csv=p=0 $U/hls/master.m3u8
run player_open_ts '"PLAYBACK_OPEN".*"sts":0' sh -c "timeout 30 $EXE /samples/sample.ts 2>&1"
run player_open_mp4 '"PLAYBACK_OPEN".*"sts":0' sh -c "timeout 30 $EXE /samples/sample.mp4 2>&1"
run player_open_https_hls '"PLAYBACK_OPEN".*"sts":0' sh -c "timeout 40 $EXE $U/hls/master.m3u8 2>&1"
rm -f /etc/ssl/cert.pem

res failed $FAILED
exit $FAILED
