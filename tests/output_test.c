#include <assert.h>
#include <errno.h>
#include <string.h>
#include "common.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

static int stopped, calls, inject_errors;
static uint8_t written[4096];
static size_t written_size;

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stopped;
}

ssize_t writev(int fd, const struct iovec *iov, int count)
{
    size_t remaining = 13, result = 0;
    int i;
    (void)fd;
    calls++;
    if (inject_errors && calls <= 2)
    {
        errno = calls == 1 ? EINTR : EAGAIN;
        return -1;
    }
    for (i = 0; i < count && remaining; i++)
    {
        size_t n = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
        assert(written_size + n <= sizeof(written));
        memcpy(written + written_size, iov[i].iov_base, n);
        written_size += n;
        result += n;
        remaining -= n;
    }
    return result;
}

static ssize_t capture(int fd, const struct iovec *iov, int count)
{
    int i;
    (void)fd;
    written_size = 0;
    for (i = 0; i < count; i++)
    {
        assert(written_size + iov[i].iov_len <= sizeof(written));
        memcpy(written + written_size, iov[i].iov_base, iov[i].iov_len);
        written_size += iov[i].iov_len;
    }
    return written_size;
}

static uint64_t read_pts(const uint8_t *p)
{
    assert((p[0] & 1) && (p[2] & 1) && (p[4] & 1));
    return ((uint64_t)(p[0] & 14) << 29) | ((uint64_t)p[1] << 22) |
           ((uint64_t)(p[2] & 254) << 14) | (p[3] << 7) | (p[4] >> 1);
}

int main(void)
{
    uint8_t pes[64], bytes[300];
    struct iovec vectors[300];
    uint8_t avcc[] = {1, 100, 0, 42, 255, 226, 0, 4, 103, 100, 0, 42,
                     0, 4, 103, 100, 0, 42, 2, 0, 2, 104, 1, 0, 2, 104, 2};
    uint8_t frame[] = {0, 0, 0, 2, 101, 136};
    WriterAVCallData_t call = {0};
    int i, first;
    assert(InsertPesHeaderWithDts(pes, 100, 224, 90000, 86400, 0) == 19);
    assert(pes[7] == 192 && pes[8] == 10 && pes[5] == 113);
    assert(read_pts(pes + 9) == 90000 && read_pts(pes + 14) == 86400);
    assert(InsertPesHeaderWithDts(pes, 100, 224, 90000, 90000, 0) == 14);
    assert(pes[7] == 128 && pes[8] == 5);
    assert(InsertPesHeaderWithDts(pes, 0, 224, INVALID_PTS_VALUE, 0, 0) == 9);
    assert(!pes[7] && !pes[8]);
    InsertPesHeaderWithDts(pes, -1, 224, 0x1ffffffffULL, 0x1fffffffeULL, 0);
    assert(read_pts(pes + 9) == 0x1ffffffffULL);
    assert(read_pts(pes + 14) == 0x1fffffffeULL);
    for (i = 0; i < 300; i++)
    {
        bytes[i] = i;
        vectors[i].iov_base = bytes + i;
        vectors[i].iov_len = 1;
    }
    inject_errors = 1;
    assert(writev_with_retry(1, vectors, 300) == 300);
    assert(written_size == 300 && !memcmp(written, bytes, 300));
    stopped = 1;
    assert(writev_with_retry(1, vectors, 300) == -1 && errno == ECANCELED);
    stopped = 0;
    call.fd = 1;
    call.private_data = avcc;
    call.private_size = sizeof(avcc);
    call.data = frame;
    call.len = sizeof(frame);
    call.Pts = 90000;
    call.Dts = 86400;
    call.WriteV = capture;
    WriterVideoH264.reset();
    first = WriterVideoH264.writeData(&call);
    assert(first == 53);
    assert(written[26] == 42); /* Keep the true SPS level. */
    assert(WriterVideoH264.writeData(&call) == first - 28);
    avcc[sizeof(avcc) - 1]++;
    assert(WriterVideoH264.writeData(&call) == first);
    WriterVideoH264.reset();
    return 0;
}
