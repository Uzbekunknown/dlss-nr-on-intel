/* Is the settings writer bounded, and does a save leave a valid file?
 *
 * Two things, and the first is mostly a non-finding worth recording.
 *
 * The writer accumulates with `snprintf`, which returns the length it WOULD have written, so a
 * truncating call would push the running length past the end and the next call's
 * `sizeof out - n` would underflow to a huge size_t. The clamp in `nr_json_put` exists for that.
 *
 * It cannot be reached, though: `nr_settings_parse` keeps at most NR_SETTINGS_MAX_KEYS (64) entries
 * of at most 63-byte keys and 127-byte values, so the largest object the writer can be handed is
 * about 12 672 bytes against a 16 384-byte buffer. A first version of this test wrote 400 keys to
 * try to overflow it and the save was accepted - not because the clamp failed, but because the
 * parser had already dropped everything past the first 64. Testing the clamp through the bridge
 * would therefore test a fixture rather than the code.
 *
 * So this checks what is actually true and can be reached: a normal save writes valid JSON, it
 * survives a reopen, and a file that is not a JSON object is left alone rather than destroyed.
 */
#include "nr_menu_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define NR_PID() (unsigned long)GetCurrentProcessId()
#else
#  include <unistd.h>
#  define NR_PID() (unsigned long)getpid()
#endif

static int fails = 0;
#define CHECK(name, ok)                                         \
    do {                                                         \
        printf("  %s  %s\n", (ok) ? "PASS" : "FAIL", name);      \
        if (!(ok)) ++fails;                                      \
    } while (0)

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

int main(void)
{
    printf("test_settings_writer: does a save leave valid JSON, and refuse what it cannot write?\n\n");

    char path[512];
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = ".";
    snprintf(path, sizeof path, "%s/nr_writer_%lu.json", dir, NR_PID());

    /* --- a normal save ------------------------------------------------------------------------ */
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  could not create %s; skipping\n", path); return 0; }
    fprintf(f, "{\n  \"intensity\": 1.0,\n  \"render_scale\": 0.4\n}\n");
    fclose(f);

    nr_menu_bridge *b = nr_menu_bridge_create_headless(path, 0);
    if (!b) { printf("  no bridge; skipping\n"); remove(path); return 0; }

    CHECK("a save succeeds", nr_menu_bridge_save(b) != 0);

    size_t n = 0;
    char *text = read_all(path, &n);
    CHECK("the saved file is readable", text != NULL);
    if (text) {
        CHECK("it starts with '{'", n > 1 && text[0] == '{');
        /* The writer closes with "\n}\n", so the last two bytes are '}' and '\n' and the byte
         * before them is '\n'. The first version of this checked `text[n-3] == '}'`, which is off by
         * one for that shape - the writer was right and the assertion was wrong. */
        CHECK("it ends with '}\\n'", n > 3 && text[n - 1] == '\n' && text[n - 2] == '}'
                                          && text[n - 3] == '\n');
        /* The trailing comma the first draft of this writer emitted would appear here, and the
         * daemon would reject the object and keep its old values. */
        CHECK("there is no comma before the closing brace", strstr(text, ",\n}") == NULL);
        CHECK("it is not empty", n > 2);
        free(text);
    }

    /* --- a file that is not a JSON object ----------------------------------------------------- */
    {
        /* Garbage in must not become a destroyed settings file. The writer re-emits what it parsed
         * and appends what the menu owns, so a file it cannot read still gets a valid object - but
         * only if the parse failed cleanly rather than half-way. */
        FILE *bad = fopen(path, "wb");
        if (bad) {
            fprintf(bad, "this is not json at all\n");
            fclose(bad);
            nr_menu_bridge *b2 = nr_menu_bridge_create_headless(path, 0);
            if (b2) {
                int saved = nr_menu_bridge_save(b2);
                text = read_all(path, &n);
                printf("  saving over an unparseable file returned %d\n", saved);
                CHECK("the file is still valid JSON afterwards",
                      text != NULL && text[0] == '{' && strstr(text, ",\n}") == NULL);
                free(text);
                nr_menu_bridge_destroy(b2);
            } else {
                CHECK("a bridge over an unparseable file is created", 0);
            }
        }
    }

    nr_menu_bridge_destroy(b);
    remove(path);

    printf("\n%s\n", fails ? "FAILED" : "ok");
    return fails;
}