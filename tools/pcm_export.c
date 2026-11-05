#include "pcm_export.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void pcm_export_close(PcmExport *state)
{
    if (state->fd >= 0)
        close(state->fd);
    state->fd = -1;
    state->next_open_us = 0;
}

int pcm_export_init(PcmExport *state, const char *path)
{
    struct stat st;
    pcm_export_close(state);
    if (mkfifo(path, 0600) < 0 && errno != EEXIST)
        return -1;
    if (lstat(path, &st) < 0)
        return -1;
    if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid())
    {
        errno = EACCES;
        return -1;
    }
    state->path = path;
    state->device = st.st_dev;
    state->inode = st.st_ino;
    state->dropped_chunks = 0;
    return 0;
}

static int open_fifo(PcmExport *state)
{
    struct stat st;
    struct timespec now;
    int64_t us;
    long limit;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    us = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    if (us < state->next_open_us)
        return 1;
    state->next_open_us = us + 2000000;
    state->fd = open(state->path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (state->fd < 0)
        return errno == ENXIO ? 1 : -1;
    if (fstat(state->fd, &st) < 0)
        goto fail;
    if (!S_ISFIFO(st.st_mode) || st.st_dev != state->device || st.st_ino != state->inode ||
        st.st_uid != geteuid())
    {
        errno = EACCES;
        goto fail;
    }
    if (flock(state->fd, LOCK_EX | LOCK_NB) < 0)
        goto fail;
    limit = fpathconf(state->fd, _PC_PIPE_BUF);
    if (limit < 32)
    {
        errno = EINVAL;
        goto fail;
    }
    state->atomic_size = limit < 4096 ? limit : 4096;
    state->next_open_us = 0;
    return 0;
fail:
    {
        int error = errno;
        close(state->fd);
        state->fd = -1;
        errno = error;
    }
    return -1;
}

static void put_le(uint8_t *out, uint32_t value, unsigned int bytes)
{
    unsigned int i;
    for (i = 0; i < bytes; i++)
        out[i] = value >> (i * 8);
}

static ssize_t write_without_sigpipe(int fd, const uint8_t *data, size_t size)
{
    sigset_t mask, old, pending;
    struct timespec zero = {0, 0};
    ssize_t result;
    int error, had_pending;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    error = pthread_sigmask(SIG_BLOCK, &mask, &old);
    if (error)
    {
        errno = error;
        return -1;
    }
    if (sigpending(&pending) < 0)
    {
        error = errno;
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        errno = error;
        return -1;
    }
    had_pending = sigismember(&pending, SIGPIPE);
    result = write(fd, data, size);
    error = errno;
    if (result < 0 && error == EPIPE && !had_pending)
        while (sigtimedwait(&mask, NULL, &zero) < 0 && errno == EINTR) {}
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = error;
    return result;
}

int pcm_export_write(PcmExport *state, const uint8_t *data, size_t size,
                     unsigned int rate, unsigned int channels, int big_endian)
{
    uint8_t frame[4096];
    size_t sample_size, chunk, capacity, i;
    ssize_t written;
    int result;
    if (!data || !rate || !channels || channels > UINT16_MAX ||
        size % (channels * 2))
    {
        errno = EINVAL;
        return -1;
    }
    if (!size)
        return 0;
    if (state->fd < 0 && (result = open_fifo(state)))
    {
        state->dropped_chunks++;
        return result;
    }
    sample_size = channels * 2;
    capacity = (state->atomic_size - 16) / sample_size * sample_size;
    if (!capacity)
    {
        errno = EINVAL;
        return -1;
    }
    put_le(frame, 0x504d4341, 4);
    put_le(frame + 4, rate, 4);
    put_le(frame + 8, channels, 2);
    put_le(frame + 10, 16, 2);
    while (size)
    {
        chunk = size < capacity ? size : capacity;
        put_le(frame + 12, chunk, 4);
        if (big_endian)
            for (i = 0; i < chunk; i += 2)
            {
                frame[16 + i] = data[i + 1];
                frame[17 + i] = data[i];
            }
        else
            memcpy(frame + 16, data, chunk);
        written = write_without_sigpipe(state->fd, frame, chunk + 16);
        if (written != (ssize_t)(chunk + 16))
        {
            int error = errno;
            state->dropped_chunks++;
            if (written < 0 && (error == EAGAIN || error == EWOULDBLOCK || error == EINTR))
                return 1;
            pcm_export_close(state);
            errno = written >= 0 ? EIO : error;
            return written < 0 && error == EPIPE ? 1 : -1;
        }
        data += chunk;
        size -= chunk;
    }
    return 0;
}
