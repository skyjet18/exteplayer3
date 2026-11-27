<!--
Text of the releases the workflow ipk-release.yml creates. Edit it as you
like; release_notes.py fills in the {{...}} placeholders:
  {{VERSION}}          version of the player, e.g. 188
  {{REPOSITORY}}       owner/repository, e.g. skyjet18/exteplayer3
  {{TAG}}              tag of the release, e.g. v188
  {{FFMPEG_REPO}}      repository of FFmpeg, e.g. skyjet18/FFmpeg
  {{FFMPEG9}}          FFmpeg version of the packages, e.g. 9.0.2
  {{FFMPEG_VERSIONS}}  the FFmpeg versions of the attached ipks, e.g. 9.0.2
  {{PACKAGES}}         table of the attached ipks (file, images, FFmpeg,
                       highest glibc version needed)
  {{MISSING}}          a note when a variant could not be built for this
                       version (empty otherwise)
  {{CHANGES}}          merged pull requests and commits since the previous
                       version
This comment is not part of the release text.
-->
# Prebuilt packages of exteplayer3 {{VERSION}}

Prebuilt packages of exteplayer3 based on this repository. They are built with the patched FFmpeg {{FFMPEG_VERSIONS}} (available [here](https://github.com/{{FFMPEG_REPO}})) and most dependencies are included directly in the ipk. The packages can be installed on most distributions and replace exteplayer3 from the distribution feeds.

{{PACKAGES}}
{{MISSING}}
## Which package

- **Architecture**: `armv7ahf` for ARM receivers (e.g. Vu+ 4K models, Zgemma H7, Octagon SF8008), `mips32el` for MIPS receivers (e.g. Vu+ Solo2, Duo2, Uno, Ultimo), `aarch64` for the 64-bit ARM Dreambox One and Two with an OpenEmbedded image (opkg, e.g. OE-Alliance images; not for Dreambox OS). `uname -m` on the box shows `armv7l` for ARM, `mips` for MIPS and `aarch64` for 64-bit ARM.
- The `aarch64` package is built with the Dreambox One/Two video output (DreamNextGen) and has **not been tested on a receiver yet**; reports are welcome.
- **FFmpeg 9** (`_ffmpeg9_` in the file name): the current FFmpeg series (everything from 7.0 to 9.0): newer decoders (e.g. VVC/H.266, xHE-AAC), the fixes of HLS, DASH and the network code since 6.1, SRT streams (`srt://`, libsrt; not in the MIPS package, to keep it small), and TLS server certificates are verified by default (with the CA certificates of the image in `/etc/ssl`). The earlier prebuilt packages had FFmpeg 6.1.

## Install

Copy the file to `/tmp` on the box and run:

```
opkg install --force-reinstall /tmp/<file>.ipk
```

or install it directly from the release (the links in the table above):

```
opkg install --force-reinstall https://github.com/{{REPOSITORY}}/releases/download/{{TAG}}/<file>.ipk
```

If the image has a newer version of exteplayer3 installed, add `--force-downgrade`. Restart Enigma2 (or the plugin that uses exteplayer3) afterwards. To go back to the player of the image: `opkg remove exteplayer3` and install it again from the feed.

## What is in the package

- `/usr/bin/exteplayer3`
- `/usr/lib/exteplayer3_deps/`: FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libavfilter`, `libavdevice`, `libswresample`, `libswscale`, the `ffmpeg` and `ffprobe` tools) and the libraries an image may not have: OpenSSL 3.5, libxml2 2.9, libbluray 1.3 with libudfread, librtmp (rtmpdump 2.6), libx264 (the H.264 encoder of FFmpeg, `-c:v libx264`), libsrt 1.5 (ARM and AArch64), libbz2, libatomic, and liblzma on MIPS. The player and FFmpeg find them there (RPATH); the FFmpeg and the libraries of the image are not touched.

## Requirements

- glibc of the image at least the version in the table above (2.17 or 2.19; the images of the last ten years have 2.20 or newer); the package depends on `libc6` of that version, so opkg checks it;
- from the image: libc, libm, libdl, libpthread, librt, libgcc_s, libz, libasound (ALSA), libfreetype, liblzma on ARM, and libstdc++ (SRT, ARM and AArch64); every image with Enigma2 has them.

{{CHANGES}}
