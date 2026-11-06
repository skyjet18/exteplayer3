# exteplayer3
Core of movie player for E2 based on the libeplayer using the ffmpeg solution

## Adaptive playback and metadata

- `-Q 0|1|2`: Auto (default, highest advertised bandwidth), lowest, or highest
  HLS/DASH quality. This selects a representation; it is not bandwidth-adaptive
  switching during playback.
- `-D 0|1`: retain all HLS audio renditions (default), or prefer default audio.
  Groups without a default retain their available tracks.
- `-M WIDTHxHEIGHT`: optional automatic H.264 resolution limit; `0x0` (default)
  disables it. Unknown dimensions remain eligible. HEVC is not restricted.
  Explicit positive program IDs selected with `-P` override automatic limits.
- HTTP MPEG-TS uses approximate byte seeking when duration, resource size,
  transport packet layout (188/192/204 bytes) and HTTP seekability are known.
  Positions are packet-aligned; timestamp seeking is the fallback. Variable
  bitrate material can land away from the requested time. Track-switch flushes
  do not use approximate seeking. Unknown live duration is reported as zero.
- Audio switching captures the presentation position before clearing decoder
  buffers or reconfiguring audio output. Internal switch seeks retain
  subsecond precision and reset discontinuity tracking after a successful VOD
  seek. Unknown-duration non-DASH network audio switches do not seek or clear
  video. All DASH audio switches reset A/V buffers as described below.
  The companion FFmpeg DASH demuxer must include the single-file
  seek/reactivation fix so a reselected audio file does not restart at zero.
- Mixed-TS HLS (video plus embedded audio, with separate TS audio renditions)
  needs both the player's unique HLS track IDs and the companion FFmpeg runtime
  playlist-reactivation fix. FFmpeg's HLS stream IDs identify playlists and
  can be shared by embedded audio and video; the player uses stream indices
  as unique IDs instead, recognizing both `hls` and legacy `hls,applehttp`
  format names. Packet routing matches the actual stream and input context,
  never a potentially shared playlist ID. The demuxer rechecks selection during
  playback, resets suspended input buffers on reactivation and tracks sliding-window timing
  separately for each playlist. Live audio switches do not seek or reset video.
  Stdin `a<ID>` selects an ID from `a_l`; `ai<index>` selects a list position.
  Do not assume a track ID equals its audio-list position.
  VOD switch seeks also require the companion's excluded-playlist guard:
  quality preselection can leave other playlists without segments.
  LIVE switching additionally requires the packet-derived live-window anchor
  and timestamp-preserving rendition reactivation fix. The first packet played
  near the live edge is not the first segment in the manifest. Re-anchoring
  external audio to that packet can falsely advance old audio by several seconds.
  When an inactive window has completely expired, the demuxer catches up using
  actual packet timestamps rather than inventing durations for missing segments.
  A genuine discontinuity without a recoverable live-window origin is reported
  as an error instead of silently assigning a guessed presentation time.

`EP3_LIVE_SYNC=0|1` controls the optional live-output hold/watch; the variable
name includes `3`. Known positive-duration inputs, including static DASH VOD,
bypass this mechanism regardless of that setting. For receiver-only startup
problems, `-L FFMPEG_DEBUG_LEVEL:10 -L FFMPEG_SILENT:0` adds input duration,
start time, initial raw/mapped A/V timestamps, live-sync bypass and seek
diagnostics to stdout. `track identity` lines report the input context, exact
format name, stream index, previous ID, assigned ID and media type.
For receiver timing retests, `audio selection timing` logs the first selected packet's raw
PTS/time base, mapped output PTS and latest injected video PTS after each change.
These are injection timestamps, not proof of receiver presentation or lip sync.
For DASH, the player now preserves the common media timeline across audio
selections instead of anchoring a new track to the last delivered audio packet.
The non-SH4 DVB backend honors requested hardware A/V sync for DASH and reapplies
it after configuring the selected audio codec, before restarting audio.
Other formats retain their existing sync policy. All DASH audio switches,
including live and static/VOD streams, reset both A/V decoder buffers and
invalidate queued output before restarting selected audio, without seeking
the input. A brief video interruption is expected.
This restores the buffer-reset behavior present in `2a7459f` and removed by
`989093c`, which receiver testing identified as the switching regression.
Segmented DASH reactivation additionally
requires the companion correction separating packet-drop state from single-file
seeks; the earlier `79390c5c6d` fix alone does not cover real reopening.
If the receiver rejects the DASH sync ioctl, startup/switch reports an error
rather than silently running with hardware sync disabled.

Add `-L LINUXDVB_DEBUG_LEVEL:10 -L LINUXDVB_SILENT:0` for hardware diagnostics.
`hardware sync` reports the requested/applied policy. `hardware timing` samples
both decoder PTS at startup, before a switch, and on first selected-audio
injection, with separate wall-clock observation times. Unsupported queries are
reported as unavailable, never replaced with injection timestamps. Driver PTS
semantics and queued writes can still differ from the instant of audible output.
`video clock` and `audio selection clock` report raw 90-kHz timestamps, the
shared base, active offsets, and audio PTS before AAC smoothing. Retain startup
through repeated `a<ID>` selections and switch-back on both AB Pulse 4K and
VU+ Uno 4K SE. Confirm lip sync and continuously moving video, not just close
logged PTS. Host clock/packet/ioctl tests do not establish receiver lip sync.
Check startup `-t <audio-list-index>`, send `al`/`ac`/`vc`, switch to an
external audio ID and back with `a<ID>`, then request those lists again.
Embedded audio must be audible and its reported ID must differ from video.
For LIVE, start with default settings, switch embedded -> external -> external
-> embedded, and continue playback across several manifest-window rotations.
Audio must stay synchronized and video must resume after the brief DASH
buffer reset, including after returning to embedded audio. Repeat the switch
and switch-back test on static/VOD DASH as well.
If it fails, retain a redacted log spanning startup, the first
switch and the stall, then compare `EP3_LIVE_SYNC=0` separately; do not enable
extra synchronization or seek to live edge to mask the initial result.
Use `-C` to disable list caching when checking track refresh. Compare normal
output (`-a 0`) with explicit AAC software decoding (`-a 3`); the latter should
report `A_IPCM` or `A_LPCM`, not `A_AAC`. Keep
`AV_DEBUG_LEVEL` disabled: the companion's existing FFmpeg diagnostics can
include decryption material. Redact URLs/headers before sharing logs.

Subtitle `s_l` list entries and `s_c` current-track JSON add `"f":0|1` for the
forced flag, from stream disposition or a standalone `forced` title token.
Language alone never implies forced subtitles. Disabled subtitles report ID
`-1`, `f:0`. Adaptive demuxers discard unselected subtitles; embedded MP4/MKV
subtitle handling remains enabled.

Audio-only playback additionally emits
`{"m_t":{"rt":"Artist - Title","t":"Title","a":"Artist"}}` when ICY or
format/stream metadata changes. Empty strings clear old metadata; unchanged
values are deduplicated and state resets for each session. JSON control
characters are escaped, valid UTF-8 is preserved, and invalid legacy bytes
are encoded as Unicode escapes. Video/TV metadata is not emitted as radio text.
JSON IPC remains on stderr and diagnostics on stdout.

## FFmpeg companions

These backports target the local FFmpeg 6.1-derived tree, not an unmodified
system FFmpeg. Apply its accompanying logical commits:

| FFmpeg commit | Purpose |
|---|---|
| `8a5ed2f3aa` | Native HLS quality/default-audio options, EVENT live-edge startup and backwards sequence resync. |
| `f9924289f9` | ATSC PMT type `0x87` identifies E-AC-3 without requiring probe packets. |
| `0ebaa163bc` | Short DASH timeline-window bounds and opt-in initial sparse-subtitle discard. |
| `6d59900cf0` | Safe initial delay for availability-clock DASH duration templates without a manifest delay. |
| `89c9657d5a` | Fractional DASH seeks, shared seek origin and bounded transient live-fragment recovery. |
| `f7e8840943` | Stable HLS stream association, discard state and seeks across changed segment layouts. |
| `2fad38b831` | Generated-media HTTP tests for live DASH recovery/startup/ranges and HLS ad transitions. |

The player requests `hls_quality_mode`, `hls_audio_default_only` and
`dash_subtitle_default_discard` through AVOptions. These option defaults remain
unchanged for other callers. Automatic HLS preselection is bypassed for an
explicit program or active resolution limit so the player can inspect all
eligible programs after probing.

Live DASH media-fragment opens now retry transient failures globally, with at
most two retries separated by interruptible 400 ms delays. VOD, initialization
sections and permanent authorization failures are not retried. Exhaustion keeps
the existing demuxer skip/error behavior; individual network opens still obey
their normal IO timeout policy. HLS association retains absent track identities
and selected/discarded state without merging multiple audio tracks.

## Optional decoded PCM export

`-K` enables `/tmp/exteplayer3_pcm_audio.fifo`, otherwise export is disabled.
It forces software decoding (including AAC/LATM, AC-3/E-AC-3, DTS, WMA,
MP3, AMR, Vorbis and Opus), increasing receiver CPU usage. The corresponding
FFmpeg decoders must be available.

Each chunk has a 16-byte little-endian header, equivalent to Python
`struct.Struct("<IIHHI")`: magic `0x504d4341` (bytes `ACMP`), sample rate,
channel count, bits per sample (16), and payload length. Payload is interleaved
signed S16LE audio. Rate/channel count may change between chunks.

Unlike the reference implementation, chunks are split on complete audio sample
frames to fit `PIPE_BUF`, then header and payload are written atomically.
No-reader, slow-reader and disconnect conditions drop complete chunks, never
leave partial frames, and never delay playback. Consumers must read exact
header/payload lengths and tolerate gaps; bytewise magic resynchronization is
not necessary with this writer alone. Repeated no-reader opens are limited to
one every two seconds. Disconnects allow an immediate reconnect.

Only an owned FIFO is accepted; symlinks, regular files and replaced inodes are
rejected. New FIFOs have mode 0600. A nonblocking advisory lock excludes a
second cooperating exporter. Use one reader: multiple readers divide bytes
rather than receiving independent copies. SIGPIPE is suppressed only around
the writer thread's operation, without changing the process-wide handler.
The first normal drop and final drop count are logged to stdout; unexpected
export errors are reported and disable export for that session without
stopping playback. Initialization errors fail playback initialization.

The FIFO remains on disk across sessions for reconnecting readers. Close
descriptors on stop; never replace the FIFO while a session is active.

## Native validation

Set `FFMPEG_SOURCE` to the companion source tree and `FFMPEG_BUILD` to its
configured, built out-of-tree native directory:

```sh
export FFMPEG_SOURCE=/path/to/FFmpeg
export FFMPEG_BUILD=/path/to/ffmpeg-build
export FFMPEG_EXTRA_LIBS="-lxml2" # plus any other dependencies of this static build
sh tests/run.sh
sh tests/build.sh /path/to/test-output/exteplayer3-host
python3 tests/integration.py \
  --player /path/to/test-output/exteplayer3-host \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg"
make -C "$FFMPEG_BUILD" fate-hls-selection fate-mpegts-probe fate-dash-live
make -C "$FFMPEG_BUILD" libavformat/tests/dash_http
python3 "$FFMPEG_SOURCE/tests/dash_http.py" \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" \
  --driver "$FFMPEG_BUILD/libavformat/tests/dash_http"
make -C "$FFMPEG_BUILD" libavformat/tests/hls_http
python3 "$FFMPEG_SOURCE/tests/hls_http.py" \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" \
  --driver "$FFMPEG_BUILD/libavformat/tests/hls_http"
```

The mixed-TS HLS fixture generates three distinguishable AAC tones, an embedded
audio/video variant and two external TS audio renditions. It tests repeated
switches, VOD seeks, suspended-playlist recovery and sliding live windows,
including embedded and external initial audio selections. VOD checks allow up
to one two-second segment of seek preroll; live first-packet displacement is
limited to 250 ms. Source-packet fingerprints also require timestamps to stay
within 100 ms of the original media, not merely agree after rewriting.
Rendition sequence numbers are not used as cross-playlist timestamps.
Add `--realtime` to the companion HTTP command for clock-driven rolling windows,
nonzero initial windows, reactivation after the previous window expires, and a
deliberate 140 ms source offset on one audio rendition. The driver now uses
normal live startup, rather than forcing `live_start_index=0`.

For actual decoded-audio switching checks, build a host player with the isolated
`PCM_EXPORT_PATH` described below, then run:

```sh
FFMPEG_SOURCE="$FFMPEG_SOURCE" python3 tests/hls_switch.py \
  --player /path/to/test-output/exteplayer3-host \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" \
  --pcm-fifo /path/to/isolated-test.fifo
```

This tests the real `a<ID>` stdin path with live sync off/on. Each selection must
produce at least 250 ms of contiguous selected-tone PCM within five seconds,
retain presentation progress and recover when returning to the original track.
The host output is not a test of physical receiver/DVB audio hardware.

The original PCM-only fixture did not cover duplicate IDs under legacy format
aliases or the normal compressed-audio writer. The expanded fixture supports
`--multiple` (three qualities), `--video-first`, `--quality auto|lowest|all`
(`all` probes every variant before program selection), `--case vod|live`,
and `--fmp4` for raw-AAC framing regression checks. Tests disable track caching
and recheck video/audio IDs after real manager refreshes.

To validate normal compressed AAC output without `-K`, build with a new isolated
capture file. `HOST_HLS_ALIAS=1` additionally enables a host-only linker wrapper
which reports the same demuxer as `hls,applehttp`; it reproduced the duplicate-ID
failure on the preceding player commit. It is not evidence that a particular
receiver reports that format name.

```sh
HOST_AUDIO_OUTPUT=/path/to/test-output/audio.pes HOST_HLS_ALIAS=1 \
  sh tests/build.sh /path/to/test-output/exteplayer3-capture
FFMPEG_SOURCE="$FFMPEG_SOURCE" python3 tests/hls_switch.py \
  --player /path/to/test-output/exteplayer3-capture \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" \
  --pcm-fifo /path/to/test-output/audio.fifo \
  --audio-output /path/to/test-output/audio.pes \
  --mode compressed --multiple --video-first --quality all
```

The capture file must not already exist. The test removes its own capture
between sessions, parses audio PES packets and decodes the captured AAC to
identify the selected tone. This additionally requires the native FFmpeg
S16LE muxer and PCM S16LE encoder. `--mode software` tests explicit `-a 3`
without `-K`, reading the PCM writer's sample-format header and sample payload
from PES. `--mode export` (default) retains the FIFO test, which requires the
same isolated `PCM_EXPORT_PATH` at build and runtime. Do not run fixtures
concurrently against the same compiled FIFO or capture path.

Tone identity alone does **not** establish live A/V synchronization. For
timing-aware MPEG-TS coverage, capture video PES too:

```sh
HOST_AUDIO_OUTPUT=/path/to/test-output/audio.pes \
HOST_VIDEO_OUTPUT=/path/to/test-output/video.pes HOST_HLS_ALIAS=1 \
  sh tests/build.sh /path/to/test-output/exteplayer3-capture
FFMPEG_SOURCE="$FFMPEG_SOURCE" python3 tests/hls_switch.py \
  --player /path/to/test-output/exteplayer3-capture \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" \
  --pcm-fifo /path/to/test-output/audio.fifo \
  --audio-output /path/to/test-output/audio.pes \
  --video-output /path/to/test-output/video.pes \
  --timing-driver "$FFMPEG_BUILD/libavformat/tests/hls_http" \
  --mode compressed --case live --multiple --video-first --quality all \
  --sync default --backpressure
```

Both output paths must be isolated and absent. This generates time-varying media,
matches actual audio/video PES payloads to their source timestamps, and checks
one shared PTS mapping within 100 ms (preserving the intentional source offset).
After one second the latest emitted A/V PTS must also stay within 250 ms.
Selected audio must recover within one second; video source frames must advance
without a jump or reset. It starts within an already populated live window and
holds the initial selection until inactive windows expire before switching.
`--sync default` leaves `EP3_LIVE_SYNC` unset; `--sync 0|1` tests explicit modes,
and omitting it tests both. `--initial 0|1` focuses embedded/external startup.
`--backpressure` creates a bounded nonblocking audio FIFO instead of a regular
capture file, fills it, checks recovery after draining, and quits while it is
full again. This exercises the real writer retry/cancellation path, not a DVB
driver or physical display. Existing software/export checks remain separate.

The seven UBSan harnesses cover manager ownership/locking, custom IO and selection,
PES/H.264/writev, bounded DVB busy-open and PCM recovery, timestamp continuity,
atomic FIFO framing/backpressure/reconnect/path validation, and playback
seek/switch ordering when decoder reset erases the reported position.

The full host build uses fake DVB output directed to `/dev/null` and disables
the production termination socket. It also builds an HTTP-seek helper.
The host-only build namespaces an old bundled FFmpeg table symbol to avoid
a collision with modern static libavcodec. It does not modify receiver builds.
The BitBake source list includes the new `tools/pcm_export.c`; other external
build recipes with explicit source lists must include it too.

The integration fixture needs FFmpeg's `file/http/tcp` protocols, MPEG-TS/HLS,
MOV/MP4, DASH, Matroska, rawvideo, S16LE and SRT demuxers, MPEG-TS/MP4/DASH/Matroska muxers,
rawvideo/PCM/AAC/MPEG-2/subrip/movtext decoders, MPEG-2/AAC/movtext encoders and
the `aresample` filter and libxml2. Tests generate their own media and loopback HTTP server.
They verify actual aligned HTTP Range requests and approximate seek position,
which HLS variant is fetched, strict CLI parsing, full radio JSON, embedded
MP4/MKV forced-subtitle selection/disable, static single-file DASH startup and
audio switching with live sync both off and on, and bounded host shutdown.

The companion HTTP fixture additionally needs DASH demuxing/muxing and libxml2.
It verifies exact fragment retry counts, delay/cancellation bounds, shared media
origin during delayed multi-representation startup, progress beyond a rotating
short manifest's original window, byte-range versus standalone downloads, and
A/V -> video-only ad -> A/V HLS playback. An additional single-file VOD fixture
uses MPEG-2 video and two AAC audio tracks to verify fractional seeks while an
audio demuxer is closed, backward seeks and direct audio reactivation. It does
not certify receiver HEVC/E-AC-3 output, A/V lip sync or arbitrary provider
manifest changes.
The HLS FATE test separately exercises index/type changes, ID3 insertion, multiple
audio identities, late codec probing, ambiguous/duplicate IDs, codec/time-base
updates and seeks/discard checks while a selected track is absent.

For end-to-end PCM testing, compile an **isolated, unused** FIFO path into the
test binary, then pass that same path to the integration fixture:

```sh
export CFLAGS='-DPCM_EXPORT_PATH="/path/to/test-output/audio.fifo"'
sh tests/build.sh /path/to/test-output/exteplayer3-host
python3 tests/integration.py --player /path/to/test-output/exteplayer3-host \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" --pcm-fifo /path/to/test-output/audio.fifo
```

The integration test creates and removes that FIFO. Never use a production
endpoint. It checks software decoding despite a later `-a 0`, nonzero PCM,
header fields and complete payload framing. Full-player ASan/UBSan runs also
pass with `-fsanitize=address,undefined -fno-omit-frame-pointer` in `CFLAGS`
and `ASAN_OPTIONS=detect_leaks=0`; this does not assert leak-free legacy teardown.
The isolated HTTP helper excludes ASan global registration, which otherwise
retains unrelated player command handlers; it retains UBSan instrumentation.

Known unrelated baseline: the existing FFmpeg `fate-mpegtsenc` AAC-dejitter
expectations fail against the unchanged muxer. All three companion FATE targets
and the generated-media HTTP fixture pass; this series does not alter that muxer
behavior.

## Backport provenance and exclusions

Reference: [boingbasti/e2-serviceapp-vti](https://github.com/boingbasti/e2-serviceapp-vti),
`exteplayer3-build/src`, compared through `2716903d48a4`.
The player baseline was `9703eb5`; the FFmpeg baseline was `3fcbef0461`.
The reference's player history was inspected, including later corrections
rather than just copying its final tree:

| Reference history | Adaptation |
|---|---|
| `ca4c076`, `29d96ba`, `eb75eb1` | Track-context refresh, lock ordering, EOF/read recovery, responsive shutdown and AV continuity. |
| `f925bcc`, `fc41d30` | Optional PES DTS, H.264 parameter-set refresh and complete partial-write handling. |
| `492b509`, `70f5c4c`, `94be6a8` | Duration/HTTP-TS seeks, PCM export and PCM reset/rearming. |
| `ea4dae6`, `7c54555`, `38e9389`, `75fe304` | Highest-bandwidth Auto, native HLS selection, DASH selection and unknown dimensions. |
| `ab3cd73`, `191ae3f`, `d09423c`, `40bbdd2` | Radio/forced-subtitle metadata, selection/discard and bounded DVB busy-open retry. |
| `6a94983` and earlier timestamp follow-ups | Compact, testable discontinuity handling, cached offsets, shared AV delta and pause-aware recovery. |

Intentional exclusions/adaptations:

- Keep target version 184, AAC dejitter (`-j`), multi-SPS/PPS parsing, MOV_TEXT,
  DRM-header and FFmpeg compatibility work. Existing `.m2ts`/`.mts` support
  needs no duplicate patch.
- Preserve the local FFmpeg CENC support, shared DASH timestamp/start origin,
  timeline-reload position repair, HTTP segment seekability/reuse/redirects,
  HLS discontinuity processing and EVENT duration logic. The older
  `ffmpeg-skyjet183-upstream-sync`, DASH AV-sync, CENC and HTTP-options patches
  must not be applied wholesale.
- Adapt the short-buffer and duration-template live-edge fixes to local
  media-time selection; do not assume independent rendition counters align.
  Keep the existing common startup point and interruptible refresh logic rather
  than importing a header-wide manifest freeze or repeated fixed-wait loop.
- Do not import HLS starvation scheduling based on cross-rendition sequence
  ordering, experimental forced fairness or `/tmp` diagnostic-file machinery.
  Local HLS already defaults `max_reload` to 100; no replacement is necessary.
- Sparse DASH subtitles are discarded only when the player opts in, not as a
  global FFmpeg default. H.264 levels are not rewritten; the optional resolution
  limit replaces the fork's receiver-specific hard limit.
- FIFO framing and `writev` were repaired rather than copying the reference's
  partial-frame behavior or silent 128-vector truncation.
- Full local-file tests also found an existing `sizeof`/initialization typo in
  custom IO. It is fixed, and output loops reacquire borrowed manager tracks
  after callbacks that temporarily release the container lock.
- No VTi packaging scripts, receiver paths, glibc ABI shims or serviceapp/UI
  changes are imported. These commits do not build or install an IPK.

### Complete FFmpeg patch-file audit

All 13 `exteplayer3-build/ffmpeg-*.patch` files were compared hunk by hunk at the
same reference revision. Existing local improvements were retained rather than
replaced with older fork implementations:

| Reference patch | Disposition |
|---|---|
| `ffmpeg-cenc-drm-support.patch` | MOV KID/default keys, DASH propagation and loopback DRM support already exist. Do not copy key-value logging or per-track timestamp zeroing. |
| `ffmpeg-dash-avsync-fix.patch` | Existing shared timestamp origin and timeline reload-position handling supersede it. `89c9657d5a` also uses that shared origin for seeks. |
| `ffmpeg-dash-header-parsing-manifest-freeze-fix.patch` | Not imported: delayed-header/rotating-window HTTP tests pass without freezing refresh, including progression beyond the original window. This is representative coverage, not a proof for every provider. |
| `ffmpeg-dash-http-options.patch` | Existing segment seekability and persistent/reuse handling cover its useful behavior. HTTP tests verify standalone downloads and distinct single-file initialization/media ranges. |
| `ffmpeg-dash-live-edge-delay-fix.patch` | Initial live delay adapted in `6d59900cf0`; bounded transient media-open retries added globally in `89c9657d5a`. Do not copy repeated fixed manifest waits. |
| `ffmpeg-dash-short-buffer-startup-fix.patch` | Adapted in `0ebaa163bc`, preserving a common media-time start instead of aligning unrelated segment counters. |
| `ffmpeg-dash-subtitle-starvation-fix.patch` | `0ebaa163bc` plus player `c4b8db6`; initial sparse-subtitle discard remains opt-in for generic FFmpeg callers. |
| `ffmpeg-eac3-stream-type-fix.patch` | `f9924289f9`, with native PMT/probe regression coverage. |
| `ffmpeg-hls-discontinuity-starvation-fix.patch` | Identity-preserving association adapted in `f7e8840943`, including absent selected tracks and seeks. Existing discontinuity timestamp normalization retained; no type-only merging, recursive packet skipping, cross-rendition sequence ordering or forced fairness. |
| `ffmpeg-hls-native-preselect.patch` | `8a5ed2f3aa`, without a fixed 128-rendition cap or reuse of the broken-playlist flag. |
| `ffmpeg-hls-sequence-wrap-resync-fix.patch` | `8a5ed2f3aa`, preserving bounded tolerance and VOD behavior. |
| `ffmpeg-hls-sequence-wrap-resync-fix-6.1.1.patch` | Same useful resync behavior as the previous patch, not an additional feature. |
| `ffmpeg-skyjet183-upstream-sync.patch` | XML ampersand handling, oldest-valid segment, EVENT duration and WebVTT suffix handling already exist. Remaining fractional seek precision and related time-unit fixes adapted in `89c9657d5a`. |

No additional player runtime option is needed for these follow-up companions.
Native FATE, actual HTTP fixtures, all six player UBSan harnesses and the complete
ASan/UBSan player integration suite pass with the updated libraries. Receiver
and cross-toolchain limitations below still apply.

## Receiver validation still required

Host tests do **not** certify ARM/MIPS/SH4 cross-builds, DVB driver behavior,
decoder compatibility, audible lip sync, PCM CPU load or long-running live
provider/SSAI behavior. Before deployment, check on representative receivers:

- HLS/DASH startup and explicit quality/program selection, separate audio,
  short live windows, EVENT playlists and discontinuity/ad transitions.
- Natural AV offsets, 33-bit PTS wrap, reordered video, audio/video clock
  resets, long stalls, pause/resume, repeated seeks and stream switches.
- AAC dejitter and passthrough versus software PCM; DTS/PES B-frame output,
  initial/changed H.264 SPS/PPS, busy decoder devices and stop latency.
- MP4/MKV/adaptive subtitle selection and disabling, forced flags, ICY and
  metadata clearing across stations; retained DRM/MOV_TEXT behavior.
- `-K` off/on CPU cost, export consumers joining/leaving, slow readers,
  format changes, seeks and audio switching without decoder silence.
