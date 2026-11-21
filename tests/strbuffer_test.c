#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* The string buffer of the subtitle output when it cannot grow: the text in
 * it stays, nothing leaks (LeakSanitizer at the exit). */

static int fail_realloc;

static void *test_realloc(void *ptr, size_t size)
{
    if (fail_realloc)
        return NULL;
    return realloc(ptr, size);
}

#define realloc test_realloc
#include "../tools/strbuffer.c"
#undef realloc

int main(void)
{
    strbuffer_t *buffer = strbuffer_init(8);
    assert(buffer);
    assert(!strbuffer_add(buffer, "first"));
    fail_realloc = 1;
    assert(strbuffer_add(buffer, "a line longer than the buffer") < 0);
    fail_realloc = 0;
    assert(buffer->buffer && !strcmp(buffer->buffer, "first") && STRBUFFER_USED(buffer) == 5);
    assert(!strbuffer_add(buffer, ", second"));
    assert(!strcmp(buffer->buffer, "first, second") && buffer->end - buffer->buffer == (long)buffer->size);
    strbuffer_free(buffer);
    return 0;
}
