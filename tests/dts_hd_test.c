#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/types.h>
#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS _IOR('o', 57, __u64)
#endif
#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif
#define ioctl test_ioctl
int test_ioctl(int fd, unsigned long request, ...);
#define access test_access
int test_access(const char *path, int mode);
#define fopen test_fopen
FILE *test_fopen(const char *path, const char *mode);
#include "../output/linuxdvb_mipsel.c"
#undef ioctl
#undef access
#undef fopen

/* The DTS-HD bypass mode: the first DTS frame after a start or a switch
 * decides. The driver is a list of the ioctls it was given; whether it has
 * /proc/stb/audio/dtshd and which /proc/stb/info/vumodel it reports is up to
 * the case. The decision whether the mode is
 * usable is taken once per process, so every case runs in a child of its own. */

static stb_type_t stb = STB_VUPLUS;
static int has_dtshd = 1;
static const char *model = NULL;
static unsigned long requests[32];
static int arguments[32];
static int request_count;
static int refused_mode = -1;
static int requests_at_write[8];
static int writes;

int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    (void)fd;
    assert(request_count < 32);
    requests[request_count] = request;
    arguments[request_count] = -1;
    if (request == AUDIO_SET_BYPASS_MODE)
    {
        va_start(args, request);
        arguments[request_count] = va_arg(args, int);
        va_end(args);
    }
    request_count++;
    if (request == AUDIO_SET_BYPASS_MODE && arguments[request_count - 1] == refused_mode)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int test_access(const char *path, int mode)
{
    assert(mode == F_OK && !strcmp(path, "/proc/stb/audio/dtshd"));
    return has_dtshd ? 0 : -1;
}

FILE *test_fopen(const char *path, const char *how)
{
    assert(!strcmp(how, "r"));
    /* /proc reads may block: never with the output mutex held */
    assert(!pthread_mutex_trylock(&LinuxDVBmutex));
    pthread_mutex_unlock(&LinuxDVBmutex);
    if (strcmp(path, "/proc/stb/info/vumodel") || !model)
        return NULL;
    return fmemopen((void *)model, strlen(model), how);
}

stb_type_t GetSTBType(void) { return stb; }
int8_t PlaybackDieNow(int8_t value) { (void)value; return 0; }
void E2iSendMsg(const char *format, ...) { (void)format; }

static int write_frame(void *call)
{
    (void)call;
    assert(writes < 8);
    requests_at_write[writes++] = request_count;
    return 100;
}
static int reset_writer(void) { return 0; }
static WriterCaps_t dts_caps = {.dvbStreamType = AUDIOTYPE_DTS};
static WriterCaps_t ac3_caps = {.dvbStreamType = AUDIOTYPE_AC3};
static Writer_t dts_writer = {.reset = reset_writer, .writeData = write_frame, .caps = &dts_caps};
static Writer_t ac3_writer = {.reset = reset_writer, .writeData = write_frame, .caps = &ac3_caps};
static const char *encoding = "A_DTS";
Writer_t *getWriter(char *name)
{
    return !strcmp(name, "A_DTS") ? &dts_writer : &ac3_writer;
}
Writer_t *getDefaultVideoWriter(void) { return NULL; }
Writer_t *getDefaultAudioWriter(void) { return NULL; }
ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
ssize_t BufferingWriteV(int fd, const struct iovec *iov, int ic) { (void)fd; (void)iov; return ic; }
int32_t LinuxDvbBuffOpen(Context_t *context, char *type, int outfd, void *mtx)
{
    (void)context;
    (void)type;
    (void)outfd;
    (void)mtx;
    return 0;
}
int32_t LinuxDvbBuffClose(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlush(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffFlushAudio(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffResume(Context_t *context) { (void)context; return 0; }
int32_t LinuxDvbBuffSetSize(const uint32_t size) { (void)size; return 0; }
uint32_t LinuxDvbBuffGetSize(void) { return 0; }

static int audio_command(void *ctx, ManagerCmd_t command, void *argument)
{
    (void)ctx;
    assert(command == MANAGER_GETENCODING);
    *(char **)argument = strdup(encoding);
    return 0;
}

static PlaybackHandler_t playback = {.isPlaying = 1};
static Manager_t audio = {.Command = audio_command};
static ManagerHandler_t managers = {.audio = &audio};
static Context_t context = {.playback = &playback, .manager = &managers};

/* A 16-byte 16-bit big-endian core frame (FSIZE 15), alone or followed by
 * the sync of a DTS-HD extension substream. */
static uint8_t core[16] = {0x7f, 0xfe, 0x80, 0x01, 0xfc, 0x00, 0x00, 0xf0};
static uint8_t core_hd[24] = {0x7f, 0xfe, 0x80, 0x01, 0xfc, 0x00, 0x00, 0xf0,
                              [16] = 0x64, 0x58, 0x20, 0x25};
static uint8_t substream_only[12] = {0x64, 0x58, 0x20, 0x25};

static void write_audio(uint8_t *data, uint32_t size)
{
    AudioVideoOut_t out = {.data = data, .len = size, .type = "audio"};
    assert(!LinuxDvbOutput.Write(&context, &out));
}

static void start(void)
{
    assert(!pthread_mutex_init(&LinuxDVBmutex, NULL));
    audiofd = 1;
    assert(!Command(&context, OUTPUT_PLAY, "audio"));
    /* plain DTS mode at the start, as always */
    assert(request_count == 3 && requests[0] == AUDIO_SET_BYPASS_MODE);
    assert(strcmp(encoding, "A_DTS") || arguments[0] == 2);
    request_count = 0;
}

/* The restart in DTS-HD mode, all of it before the frame is written. */
static void assert_restart(int first, int mode)
{
    assert(requests[first] == AUDIO_STOP && requests[first + 1] == AUDIO_CLEAR_BUFFER);
    assert(requests[first + 2] == AUDIO_SET_BYPASS_MODE && arguments[first + 2] == 0x10);
    if (mode != 0x10)
    {
        assert(requests[first + 3] == AUDIO_SET_BYPASS_MODE && arguments[first + 3] == mode);
        first++;
    }
    assert(requests[first + 3] == AUDIO_PLAY && requests[first + 4] == AUDIO_CONTINUE);
}

static void hd_extension_after_the_core(void)
{
    start();
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 5 && writes == 1 && requests_at_write[0] == 5);
    assert_restart(0, 0x10);
    /* decided: the next frames cost no ioctl */
    write_audio(core_hd, sizeof(core_hd));
    write_audio(core, sizeof(core));
    assert(request_count == 5 && writes == 3);
}

static void substream_without_core(void)
{
    start();
    write_audio(substream_only, sizeof(substream_only));
    assert(request_count == 5 && requests_at_write[0] == 5);
    assert_restart(0, 0x10);
}

/* Plain DTS: the first frame only clears the mark, a later HD sync changes nothing. */
static void core_only(void)
{
    start();
    write_audio(core, sizeof(core));
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 0 && writes == 2);
}

static void no_restart(void)
{
    start();
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 0 && writes == 1);
}

static void without_dtshd(void) { has_dtshd = 0; no_restart(); }
static void dreambox(void) { stb = STB_DREAMBOX; no_restart(); }
static void ac3_track(void) { encoding = "A_AC3"; no_restart(); }
static void forced_off(void) { assert(!setenv("EP3_DTS_HD_MODE", "0", 1)); no_restart(); }

static void forced_on(void)
{
    has_dtshd = 0;
    assert(!setenv("EP3_DTS_HD_MODE", "1", 1));
    hd_extension_after_the_core();
}

/* Vu+ Uno 4K SE, Ultimo 4K and Duo 4K SE have no /proc/stb/audio/dtshd and
 * use the mode; other Vu+ models and other boxes with such a model file stay
 * on plain DTS. */
static void vuplus_model(const char *name)
{
    has_dtshd = 0;
    model = name;
}
static void uno4kse(void) { vuplus_model("uno4kse\n"); hd_extension_after_the_core(); }
static void ultimo4k(void) { vuplus_model("ultimo4k\n"); hd_extension_after_the_core(); }
static void duo4kse(void) { vuplus_model("duo4kse"); hd_extension_after_the_core(); }
static void solo4k(void) { vuplus_model("solo4k\n"); no_restart(); }
static void zero4k(void) { vuplus_model("zero4k\n"); no_restart(); }
static void uno4k(void) { vuplus_model("uno4k\n"); no_restart(); }
static void not_a_vuplus(void) { vuplus_model("uno4kse\n"); stb = STB_UNKNOWN; no_restart(); }
static void vuplus_forced_off(void)
{
    vuplus_model("uno4kse\n");
    assert(!setenv("EP3_DTS_HD_MODE", "0", 1));
    no_restart();
}

/* The first DTS track comes with a switch after an AC-3 start: the model is
 * read before the mutex is taken (test_fopen asserts it). */
static void uno4kse_switch_to_dts(void)
{
    vuplus_model("uno4kse\n");
    encoding = "A_AC3";
    start();
    encoding = "A_DTS";
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(requests[2] == AUDIO_SET_BYPASS_MODE && arguments[2] == 2);
    request_count = 0;
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 5);
    assert_restart(0, 0x10);
}

/* The driver refuses DTS-HD: plain DTS again, the decoder runs on. */
static void mode_refused(void)
{
    refused_mode = 0x10;
    start();
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 6 && requests_at_write[0] == 6);
    assert_restart(0, 2);
}

/* Paused: the decoder is left alone, the first frame after the continue decides. */
static void paused(void)
{
    start();
    playback.isPaused = 1;
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 0 && writes == 1);
    playback.isPaused = 0;
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 5 && requests_at_write[1] == 5);
    assert_restart(0, 0x10);
}

/* Every switch decides again: HD -> core only stays plain DTS, -> HD restarts. */
static void decided_again_after_a_switch(void)
{
    start();
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 5);
    request_count = 0;
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    assert(requests[2] == AUDIO_SET_BYPASS_MODE && arguments[2] == 2);
    request_count = 0;
    write_audio(core, sizeof(core));
    assert(request_count == 0);
    assert(!Command(&context, OUTPUT_SWITCH, "audio"));
    request_count = 0;
    write_audio(core_hd, sizeof(core_hd));
    assert(request_count == 5);
    assert_restart(0, 0x10);
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
    fprintf(stderr, "dts_hd: %s failed\n", name);
    return 1;
}

int main(void)
{
    int failed = 0;
    unsetenv("EP3_DTS_HD_MODE");
    failed += run("HD extension after the core", hd_extension_after_the_core);
    failed += run("extension substream without core", substream_without_core);
    failed += run("core only", core_only);
    failed += run("no /proc/stb/audio/dtshd", without_dtshd);
    failed += run("Dreambox", dreambox);
    failed += run("AC-3 track", ac3_track);
    failed += run("EP3_DTS_HD_MODE=0", forced_off);
    failed += run("EP3_DTS_HD_MODE=1", forced_on);
    failed += run("Vu+ Uno 4K SE", uno4kse);
    failed += run("Vu+ Ultimo 4K", ultimo4k);
    failed += run("Vu+ Duo 4K SE", duo4kse);
    failed += run("Vu+ Solo 4K", solo4k);
    failed += run("Vu+ Zero 4K", zero4k);
    failed += run("Vu+ Uno 4K", uno4k);
    failed += run("vumodel on another box", not_a_vuplus);
    failed += run("Vu+ Uno 4K SE, EP3_DTS_HD_MODE=0", vuplus_forced_off);
    failed += run("Vu+ Uno 4K SE, switch to DTS", uno4kse_switch_to_dts);
    failed += run("DTS-HD mode refused", mode_refused);
    failed += run("paused", paused);
    failed += run("decided again after a switch", decided_again_after_a_switch);
    return failed ? 1 : 0;
}
