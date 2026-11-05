#ifndef PCM_EXPORT_H
#define PCM_EXPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef PCM_EXPORT_PATH
#define PCM_EXPORT_PATH "/tmp/exteplayer3_pcm_audio.fifo"
#endif

typedef struct
{
    int fd;
    const char *path;
    dev_t device;
    ino_t inode;
    size_t atomic_size;
    int64_t next_open_us;
    uint64_t dropped_chunks;
} PcmExport;

int pcm_export_init(PcmExport *state, const char *path);
/* 0: delivered, 1: best-effort drop, -1: error (errno is preserved). */
int pcm_export_write(PcmExport *state, const uint8_t *data, size_t size,
                     unsigned int rate, unsigned int channels, int big_endian);
void pcm_export_close(PcmExport *state);

#endif
