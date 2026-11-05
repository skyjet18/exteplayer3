#define _GNU_SOURCE
#include "pcm_export.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned int get_le(const uint8_t *p, int count)
{
    unsigned int value = 0;
    int i;
    for (i = 0; i < count; i++)
        value |= (unsigned int)p[i] << (8 * i);
    return value;
}

int main(void)
{
    char directory[] = "/tmp/exteplayer3-pcm-test-XXXXXX", path[256];
    PcmExport state = {.fd = -1}, other = {.fd = -1};
    uint8_t data[16384] = {0x12, 0x34, 0x56, 0x78}, received[32768];
    struct sigaction before, after;
    struct stat st;
    int reader, file;
    ssize_t count;
    size_t pos;
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/audio.fifo", directory);
    assert(!pcm_export_init(&state, path));
    assert(pcm_export_write(&state, data, 4, 48000, 2, 0) == 1);
    assert(state.fd == -1 && state.dropped_chunks == 1);
    reader = open(path, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    assert(fcntl(reader, F_SETPIPE_SZ, 4096) >= 4096);
    state.next_open_us = 0;
    assert(!pcm_export_write(&state, data, 4, 48000, 2, 0));
    count = read(reader, received, sizeof(received));
    assert(count == 20);
    assert(get_le(received, 4) == 0x504d4341);
    assert(get_le(received + 4, 4) == 48000);
    assert(get_le(received + 8, 2) == 2 && get_le(received + 10, 2) == 16);
    assert(get_le(received + 12, 4) == 4 && !memcmp(received + 16, data, 4));
    assert(!pcm_export_write(&state, data, 4, 44100, 1, 1));
    assert(read(reader, received, sizeof(received)) == 20);
    assert(get_le(received + 4, 4) == 44100 && get_le(received + 8, 2) == 1);
    assert(!memcmp(received + 16, "\x34\x12\x78\x56", 4));
    assert(pcm_export_write(&state, data, 3, 48000, 2, 0) == -1 && errno == EINVAL);
    assert(!pcm_export_init(&other, path));
    assert(pcm_export_write(&other, data, 4, 48000, 2, 0) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    pcm_export_close(&other);
    assert(pcm_export_write(&state, data, sizeof(data), 48000, 2, 0) == 1);
    count = read(reader, received, sizeof(received));
    assert(count > 0);
    pos = 0;
    while (pos < (size_t)count)
    {
        unsigned int payload = get_le(received + pos + 12, 4);
        assert(get_le(received + pos, 4) == 0x504d4341);
        assert(payload && payload % 4 == 0);
        pos += 16 + payload;
        assert(pos <= (size_t)count);
    }
    assert(pos == (size_t)count);
    close(reader);
    assert(!sigaction(SIGPIPE, NULL, &before));
    assert(pcm_export_write(&state, data, 4, 48000, 2, 0) == 1);
    assert(state.fd == -1);
    assert(!sigaction(SIGPIPE, NULL, &after));
    assert(before.sa_handler == after.sa_handler);
    reader = open(path, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    assert(!pcm_export_write(&state, data, 4, 48000, 2, 0));
    assert(read(reader, received, sizeof(received)) == 20);
    pcm_export_close(&state);
    close(reader);
    assert(!unlink(path));
    file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0);
    close(file);
    assert(pcm_export_init(&state, path) == -1 && errno == EACCES);
    assert(!unlink(path));
    assert(!symlink("/dev/null", path));
    assert(pcm_export_init(&state, path) == -1 && errno == EACCES);
    assert(!unlink(path));
    assert(!pcm_export_init(&state, path));
    assert(!unlink(path));
    file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0);
    close(file);
    assert(pcm_export_write(&state, data, 4, 48000, 2, 0) == -1 && errno == EACCES);
    assert(!stat(path, &st) && st.st_size == 0);
    pcm_export_close(&state);
    assert(!unlink(path));
    assert(!rmdir(directory));
    return 0;
}
