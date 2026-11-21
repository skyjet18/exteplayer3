#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include "pcm.h"
#define access test_access
int test_access(const char *path, int mode);
#define fopen test_fopen
FILE *test_fopen(const char *path, const char *mode);
#include "../output/writer/common/misc.c"
#undef access
#undef fopen
#define write test_write
ssize_t test_write(int fd, const void *buf, size_t size);
#define select test_select
int test_select(int count, fd_set *readable, fd_set *writable, fd_set *failed, struct timeval *timeout);
#include "../output/writer/mipsel/writer.c"
#undef write
#undef select

/* HiSilicon receivers: the detection by GetSTBType() with the files of a box
 * stubbed, and the write of the buffering thread (WriteWithRetry) to a device
 * that the case describes. GetSTBType() keeps its result for the process, so
 * every case runs in a child of its own. */

static const char *present[4];
static const char *chipset;

int test_access(const char *path, int mode)
{
    int i;
    assert(mode == F_OK);
    for (i = 0; i < 4 && present[i]; i++)
        if (!strcmp(path, present[i]))
            return 0;
    errno = ENOENT;
    return -1;
}

FILE *test_fopen(const char *path, const char *how)
{
    if (strcmp(path, "/proc/stb/info/chipset") || !chipset)
    {
        errno = ENOENT;
        return NULL;
    }
    return fmemopen((void *)chipset, strlen(chipset), how);
}

static void detected(stb_type_t expected)
{
    assert(GetSTBType() == expected);
}

static const char NODE[] = "/sys/firmware/devicetree/base/soc/hisilicon_clock/name";

static void nothing(void) { detected(STB_OTHER); }
static void device_tree_node(void) { present[0] = NODE; detected(STB_HISILICON); }
static void proc_hisi(void) { present[0] = "/proc/hisi"; detected(STB_HISILICON); }
static void dev_hi_mem(void) { present[0] = "/dev/hi_mem"; detected(STB_HISILICON); }
static void dev_hi_dmx(void) { present[0] = "/dev/hi_dmx"; detected(STB_HISILICON); }
static void module_hi_media(void) { present[0] = "/sys/module/hi_media"; detected(STB_HISILICON); }
static void chipset_hi3798(void) { chipset = "hi3798mv200\n"; detected(STB_HISILICON); }
static void chipset_upper_case(void) { chipset = "Hi3716MV430\n"; detected(STB_HISILICON); }
static void chipset_hisi(void) { chipset = "HiSilicon 3798\n"; detected(STB_HISILICON); }
static void chipset_broadcom(void) { chipset = "BCM7252S\n"; detected(STB_OTHER); }

static void vuplus_stays_vuplus(void)
{
    present[0] = "/proc/stb/info/vumodel";
    present[1] = "/proc/hisi";
    chipset = "hi3798mv200\n";
    detected(STB_VUPLUS);
}

static void dreambox_stays_dreambox(void)
{
    present[0] = "/proc/stb/tpm/0/serial";
    present[1] = "/dev/hi_mem";
    detected(STB_DREAMBOX);
}

/* The BCMA audio writer (Vorbis, Opus, WMA): on HiSilicon the PES of Vorbis
 * and Opus carries no payload size and stream description (22 bytes), only
 * the codec data and the frame; every other case keeps them. A PES header
 * with a timestamp takes 14 bytes. */
static uint8_t audio_frame[4] = {1, 2, 3, 4};

static ssize_t pes_size(int fd, const struct iovec *iov, int count)
{
    ssize_t size = 0;
    int i;
    (void)fd;
    for (i = 0; i < count; i++)
        size += iov[i].iov_len;
    return size;
}

static int bcma_write(Writer_t *writer, enum AVCodecID codec_id, uint8_t *codec_data, int codec_size)
{
    pcmPrivateData_t private = {.codec_id = codec_id, .channels = 2, .sample_rate = 48000,
                                .private_data = codec_data, .private_size = codec_size};
    WriterAVCallData_t call = {.fd = 1, .data = audio_frame, .len = sizeof(audio_frame),
                               .Pts = 90000, .private_data = (uint8_t *)&private,
                               .private_size = sizeof(private), .WriteV = pes_size};
    return writer->writeData(&call);
}

/* three Vorbis headers of 30, 5 and 7 bytes in Xiph lacing */
static int vorbis_write(void)
{
    static uint8_t xiph[45] = {2, 30, 5};
    return bcma_write(&WriterAudioVORBIS, AV_CODEC_ID_VORBIS, xiph, sizeof(xiph));
}

static uint8_t opus_head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2};
static uint8_t wma_data[10];

static void vorbis_on_hisilicon(void)
{
    present[0] = NODE;
    assert(vorbis_write() == 14 + 3 * 2 + 42 + 4);
}

static void opus_on_hisilicon(void)
{
    present[0] = NODE;
    assert(bcma_write(&WriterAudioOPUS, AV_CODEC_ID_OPUS, opus_head, sizeof(opus_head)) == 14 + 19 + 4);
}

static void wma_on_hisilicon(void)
{
    present[0] = NODE;
    assert(bcma_write(&WriterAudioWMA, AV_CODEC_ID_WMAV2, wma_data, sizeof(wma_data)) == 14 + 22 + 10 + 4);
}

static void vorbis_and_opus_elsewhere(void)
{
    assert(vorbis_write() == 14 + 22 + 3 * 2 + 42 + 4);
    assert(bcma_write(&WriterAudioOPUS, AV_CODEC_ID_OPUS, opus_head, sizeof(opus_head)) == 14 + 22 + 19 + 4);
}

/* The device: how much it takes per write, failures before that, and
 * whether select() ever reports it as writable. */
static int stopped;
static size_t take = (size_t)-1;
static int busy, failure = EAGAIN, interrupted, writable;
static size_t written;
static char events[64];
static int event_count;
static long timeouts[16];
static int selects;

int8_t PlaybackDieNow(int8_t value)
{
    (void)value;
    return stopped;
}

static void event(char what)
{
    if (event_count < (int)sizeof(events) - 1)
        events[event_count++] = what;
}

ssize_t test_write(int fd, const void *buf, size_t size)
{
    (void)buf;
    assert(fd == 9);
    event('w');
    if (busy)
    {
        busy--;
        errno = failure;
        return -1;
    }
    if (size > take)
        size = take;
    written += size;
    return size;
}

int test_select(int count, fd_set *readable, fd_set *writable_set, fd_set *failed, struct timeval *timeout)
{
    (void)count;
    (void)failed;
    event('s');
    if (selects < 16)
        timeouts[selects] = timeout ? timeout->tv_sec * 1000000L + timeout->tv_usec : -1;
    /* a device that never becomes writable: give up after many rounds */
    if (++selects == 100)
        stopped = 1;
    FD_ZERO(readable);
    if (interrupted)
    {
        interrupted--;
        FD_ZERO(writable_set);
        errno = EINTR;
        return -1;
    }
    if (!writable)
    {
        FD_ZERO(writable_set);
        return 0;
    }
    return 1;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Context_t context = {.playback = &playback};
static char data[10] = "123456789";

/* 0 when everything was written (or the write was given up for a stop or
 * a seek), -1 for an error of the device */
static void write_packet_returns(ssize_t expected)
{
    int wake[2];
    assert(!pipe(wake));
    assert(WriteWithRetry(&context, wake[0], 9, NULL, data, sizeof(data), 1) == expected);
    events[event_count] = '\0';
    close(wake[0]);
    close(wake[1]);
}

static void write_packet(void)
{
    write_packet_returns(0);
}

/* Written at once although the device never reports that it is writable. */
static void never_writable(void)
{
    present[0] = NODE;
    write_packet();
    assert(written == sizeof(data) && !strcmp(events, "w") && !selects);
}

static void part_by_part(void)
{
    present[0] = NODE;
    take = 3;
    write_packet();
    assert(written == sizeof(data) && !strcmp(events, "wwww") && !selects);
}

/* Full: wait for the device or the wake-up pipe, 50 ms at most, then again. */
static void full_twice(void)
{
    present[0] = "/proc/hisi";
    busy = 2;
    write_packet();
    assert(written == sizeof(data) && !strcmp(events, "wswsw"));
    assert(selects == 2 && timeouts[0] == 50000 && timeouts[1] == 50000);
}

static void interrupted_select(void)
{
    present[0] = NODE;
    busy = 1;
    failure = EINTR;
    interrupted = 1;
    write_packet();
    assert(written == sizeof(data) && !strcmp(events, "wsw"));
}

static void hard_error(void)
{
    present[0] = NODE;
    busy = 1;
    failure = EIO;
    write_packet_returns(-1);
    assert(!written && !strcmp(events, "w"));
}

/* Every other receiver waits for the device first, without a time limit. */
static void other_receiver_waits(void)
{
    writable = 1;
    write_packet();
    assert(written == sizeof(data) && !strcmp(events, "sw") && timeouts[0] == -1);
}

/* The buffering thread drops the rest of its node only when it learns of
 * the error: no longer 0 as for a complete write. */
static void other_receiver_hard_error(void)
{
    writable = 1;
    busy = 1;
    failure = EIO;
    write_packet_returns(-1);
    assert(!written && !strcmp(events, "sw"));
}

static int run(const char *name, void (*test)(void))
{
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        test();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && !WEXITSTATUS(status))
        return 0;
    fprintf(stderr, "hisilicon: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    failed += run("no marker", nothing);
    failed += run("device tree node", device_tree_node);
    failed += run("/proc/hisi", proc_hisi);
    failed += run("/dev/hi_mem", dev_hi_mem);
    failed += run("/dev/hi_dmx", dev_hi_dmx);
    failed += run("/sys/module/hi_media", module_hi_media);
    failed += run("chipset hi3798mv200", chipset_hi3798);
    failed += run("chipset Hi3716MV430", chipset_upper_case);
    failed += run("chipset HiSilicon", chipset_hisi);
    failed += run("chipset BCM7252S", chipset_broadcom);
    failed += run("VU+ with SDK paths", vuplus_stays_vuplus);
    failed += run("Dreambox with SDK paths", dreambox_stays_dreambox);
    failed += run("Vorbis on HiSilicon", vorbis_on_hisilicon);
    failed += run("Opus on HiSilicon", opus_on_hisilicon);
    failed += run("WMA on HiSilicon", wma_on_hisilicon);
    failed += run("Vorbis and Opus on other receivers", vorbis_and_opus_elsewhere);
    failed += run("write to a device that is never writable", never_writable);
    failed += run("write part by part", part_by_part);
    failed += run("device full twice", full_twice);
    failed += run("interrupted select", interrupted_select);
    failed += run("hard write error", hard_error);
    failed += run("other receiver waits for the device", other_receiver_waits);
    failed += run("other receiver, hard write error", other_receiver_hard_error);
    return failed ? 1 : 0;
}
