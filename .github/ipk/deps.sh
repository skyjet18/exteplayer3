#!/bin/bash
# Runs inside the jessie userland: compiles the bundled libraries into
# /opt/deps (sources unpacked in /build by rootfs.sh).
set -eux
. /build/versions.sh
P=/opt/deps
J=$(nproc)
export CFLAGS="$ARCHFLAGS -O2 -pipe"
export LDFLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -Wl,-O1 $HASHSTYLE -Wl,--as-needed"
export PKG_CONFIG_PATH=$P/lib/pkgconfig
mkdir -p $P
: > $P/VERSIONS

log() {
    # $1 name; runs the rest, output to /build/<name>.log, tail on error
    local name=$1
    shift
    if ! "$@" > /build/$name.log 2>&1; then
        tail -60 /build/$name.log
        return 1
    fi
}

# OpenSSL; OPENSSLDIR /etc/ssl: the CA certificates and openssl.cnf of the
# image (FFmpeg 9 verifies server certificates by default)
cd /build/openssl-$OPENSSL_VERSION
ossl() {
    env -u CFLAGS -u LDFLAGS ./Configure $OPENSSL_TARGET shared no-tests no-docs "$@" --prefix=$P \
        --openssldir=/etc/ssl --libdir=lib $ARCHFLAGS -O2 -std=gnu99 || return 1
    log openssl-make make -j$J build_sw && log openssl-install make install_sw
}
if ossl; then
    OSSL_ASM="with assembler code"
else
    make clean > /dev/null 2>&1 || true
    ossl no-asm
    OSSL_ASM="no-asm (the build with assembler code failed)"
fi
echo "openssl   $(LD_LIBRARY_PATH=$P/lib $P/bin/openssl version), $OSSL_ASM, OPENSSLDIR /etc/ssl" >> $P/VERSIONS

cd /build/libxml2-$LIBXML2_VERSION
log libxml2-configure ./configure --prefix=$P --disable-static --without-python --without-icu --without-lzma \
    --with-zlib --without-debug
log libxml2-make make -j$J
log libxml2-install make install
echo "libxml2   $LIBXML2_VERSION (without ICU, without lzma)" >> $P/VERSIONS

cd /build/libudfread-$UDFREAD_VERSION
log udfread-configure ./configure --prefix=$P --disable-static
log udfread-make make -j$J
log udfread-install make install
echo "libudfread $UDFREAD_VERSION" >> $P/VERSIONS

cd /build/libbluray-$BLURAY_VERSION
log bluray-configure ./configure --prefix=$P --disable-static --disable-bdjava-jar --without-freetype \
    --without-fontconfig --disable-examples --disable-doxygen-doc
log bluray-make make -j$J
log bluray-install make install
echo "libbluray $BLURAY_VERSION (no BD-J, no FreeType)" >> $P/VERSIONS

cd /build/rtmpdump/librtmp
log rtmp-make make -j$J SYS=posix CRYPTO=OPENSSL prefix=$P XCFLAGS="-I$P/include $CFLAGS" XLDFLAGS="$LDFLAGS"
log rtmp-install make install SYS=posix CRYPTO=OPENSSL prefix=$P
echo "librtmp   rtmpdump $RTMPDUMP_COMMIT (v2.6, OpenSSL)" >> $P/VERSIONS

# libsrt (C++, NEEDs the libstdc++ of the image); linked by FFmpeg 9 only,
# so only the FFmpeg 9 packages bundle it. Crypto through OpenSSL (EVP).
mkdir -p /build/srt-build
cd /build/srt-build
log srt-cmake env CXXFLAGS="$CFLAGS" cmake /build/srt-$SRT_VERSION -DCMAKE_INSTALL_PREFIX=$P \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_INCLUDEDIR=include -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SHARED=ON -DENABLE_STATIC=OFF -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON \
    -DUSE_ENCLIB=openssl-evp -DENABLE_STDCXX_SYNC=OFF -DENABLE_UNITTESTS=OFF
log srt-make make -j$J
log srt-install make install
echo "libsrt    $SRT_VERSION (OpenSSL EVP, FFmpeg 9 packages only)" >> $P/VERSIONS

rm -rf $P/share/doc $P/share/man $P/share/gtk-doc
ls -la $P/lib
cat $P/VERSIONS
