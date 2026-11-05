# exteplayer3
Core of movie player for E2 based on the libeplayer using the ffmpeg solution

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
