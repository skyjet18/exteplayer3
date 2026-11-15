#include <assert.h>
#include <stdarg.h>
#include "../output/output_subtitle.c"

int __asan_address_is_poisoned(void const volatile *address);

static char message[16384];
static const char *encoding;
static char *encoding_copy;
static int current_track = 1, closed, graphic_packets;

void E2iSendMsg(const char *format, ...)
{
    va_list args;
    int size;
    va_start(args, format);
    size = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    assert(size >= 0 && size < (int)sizeof(message));
}

static int subtitle_manager(void *opaque, ManagerCmd_t command, void *argument)
{
    (void)opaque;
    if (command == MANAGER_GET)
        *(int *)argument = current_track;
    else {
        assert(command == MANAGER_GETENCODING);
        *(char **)argument = encoding_copy = strdup(encoding);
    }
    return 0;
}

static int32_t graphic_close(void)
{
    closed++;
    return 0;
}

static int32_t graphic_reset(void)
{
    return 0;
}

static int32_t graphic_write(WriterSubCallData_t *call)
{
    assert(call->trackId == 1 && call->codecId == SUBTITLE_CODEC_ID_PGS);
    graphic_packets++;
    return 0;
}

SubWriter_t WriterSubPGS = {NULL, graphic_close, graphic_reset, graphic_write};

static Manager_t manager = {.Command = subtitle_manager};
static ManagerHandler_t managers = {.subtitle = &manager};
static Context_t context = {.manager = &managers};

/* Packet data is followed by zero padding, as the demuxer delivers it. */
static void deliver(const char *codec, const char *data, size_t size)
{
    SubtitleOut_t out = {.trackId = 1, .len = size, .pts = 90000, .durationMS = 2000};
    out.data = calloc(1, size + 1);
    assert(out.data);
    memcpy(out.data, data, size);
    encoding = codec;
    message[0] = 0;
    assert(!SubtitleOutput.Write(&context, &out));
    free(out.data);
}

static void expect(const char *text)
{
    static char expected[sizeof(message)];
    snprintf(expected, sizeof(expected),
             "{\"s_a\":{\"id\":1,\"s\":1000,\"e\":3000,\"t\":\"%s\"}}\n", text);
    assert(!strcmp(message, expected));
}

static size_t quotes(char *text, char *escaped, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
    {
        text[i] = '"';
        escaped[2 * i] = '\\';
        escaped[2 * i + 1] = '"';
    }
    text[count] = escaped[2 * count] = 0;
    return count;
}

static void subrip_long_cue(void)
{
    char cue[1600] = "a\\b\n\t", escaped[3200] = "a\\\\b\\n\\t";
    size_t size = 5 + quotes(cue + 5, escaped + 8, 1500);
    deliver("S_TEXT/SUBRIP", cue, size);
    expect(escaped); /* 3008 bytes and the terminator. */
}

static void mov_text_quotes(void)
{
    /* 16 bit length, then the text; every quote takes two bytes in JSON. */
    char cue[1200] = {1101 >> 8, 1101 & 255, ' '}, escaped[2300] = " ";
    size_t size = 3 + quotes(cue + 3, escaped + 1, 1100);
    deliver("S_TEXT/MOV", cue, size);
    expect(escaped);
}

static void ass_text_is_escaped(void)
{
    const char *event = "1,0,Default,,0,0,0,,He said \"hi\"\\N{\\i1}C:\\dir{\\i0}";
    deliver("S_TEXT/ASS", event, strlen(event));
    expect("He said \\\"hi\\\"\\nC:\\\\dir");
}

static void encoding_is_freed(void)
{
    deliver("S_TEXT/SUBRIP", "text", 4);
    assert(__asan_address_is_poisoned(encoding_copy));
}

static void packet_of_previous_graphic_track(void)
{
    deliver("S_GRAPHIC/PGS", "p", 1);
    assert(graphic_packets == 1 && !closed);
    current_track = 2; /* Switched away, one packet of track 1 is still queued. */
    deliver("S_GRAPHIC/PGS", "p", 1);
    assert(closed == 1 && graphic_packets == 1); /* Dropped with the flush. */
    assert(!strcmp(message, "{\"s_f\":{\"r\":0}}\n"));
    current_track = 1;
}

/* The track lists for the client are collected in a strbuffer. */
static void strbuffer_exact_fit(void)
{
    static const char text[] = "0123456789abcdef";
    strbuffer_t buffer;
    assert(!strbuffer_init2(&buffer, 16));
    assert(!strbuffer_add(&buffer, text)); /* As long as one block. */
    assert(STRBUFFER_USED(&buffer) == 16 && buffer.size > 16 && !strcmp(buffer.buffer, text));
    assert(!strbuffer_add(&buffer, text)); /* Fills the second block to the last byte. */
    assert(STRBUFFER_USED(&buffer) == 32 && buffer.size > 32 && !strcmp(buffer.buffer + 16, text));
    strbuffer_free2(&buffer);
}

/* Output follow-up: text of a mov_text sample, control characters, track switch. */

static void deliver_mov_text(const char *text, const char *boxes, size_t boxes_size)
{
    char sample[64];
    size_t size = strlen(text);
    assert(2 + size + boxes_size <= sizeof(sample));
    sample[0] = size >> 8;
    sample[1] = size & 255;
    memcpy(sample + 2, text, size);
    memcpy(sample + 2 + size, boxes, boxes_size);
    deliver("S_TEXT/MOV", sample, 2 + size + boxes_size);
}

static void mov_text_by_its_length_field(void)
{
    /* One style record for the two characters: bold, size 18, white. */
    static const char style[] = {0, 0, 0, 22, 's', 't', 'y', 'l', 0, 1,
                                 0, 0, 0, 2, 0, 1, 1, 18, -1, -1, -1, -1};
    deliver_mov_text("Hello", "", 0);
    expect("Hello");
    deliver_mov_text("Hi there", "", 0);
    expect("Hi there");
    deliver_mov_text("Hi", style, sizeof(style));
    expect("Hi");
}

static void mov_text_line_break(void)
{
    deliver_mov_text("line1\\Nline2", "", 0);
    expect("line1\\nline2");
}

static void control_characters_are_escaped(void)
{
    deliver_mov_text("a\vb\x1f", "", 0);
    expect("a\\u000bb\\u001f");
    deliver("S_TEXT/SUBRIP", "a\vb\x1f", 4);
    expect("a\\u000bb\\u001f");
}

/* No length field in front and nothing behind the packet. */
static void mov_text_ends_with_the_packet(void)
{
    static const char sample[] = {'a', 'b', ' ', 'c', 'd'};
    SubtitleOut_t out = {.trackId = 1, .len = sizeof(sample), .pts = 90000, .durationMS = 2000};
    out.data = malloc(sizeof(sample));
    assert(out.data);
    memcpy(out.data, sample, sizeof(sample));
    encoding = "S_TEXT/MOV";
    message[0] = 0;
    assert(!SubtitleOutput.Write(&context, &out));
    free(out.data);
    expect(" cd");
}

static void packet_of_previous_text_track(void)
{
    current_track = 2; /* Switched away, one packet of track 1 is still queued. */
    deliver("S_TEXT/SUBRIP", "text", 4);
    assert(!strcmp(message, "{\"s_f\":{\"r\":0}}\n"));
    current_track = 1;
}

int main(void)
{
    subrip_long_cue();
    mov_text_quotes();
    ass_text_is_escaped();
    encoding_is_freed();
    packet_of_previous_graphic_track();
    strbuffer_exact_fit();
    mov_text_by_its_length_field();
    mov_text_line_break();
    control_characters_are_escaped();
    mov_text_ends_with_the_packet();
    packet_of_previous_text_track();
    return 0;
}
