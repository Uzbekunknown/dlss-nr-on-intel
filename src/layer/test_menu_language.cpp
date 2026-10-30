/* Does the panel's language selector agree with the language the rows are drawn in?
 *
 * The report: a settings file with `menu_language: 1` gave a panel whose rows were all Chinese
 * and whose footer selector still read "English". Two fields held the same setting: the bridge's
 * `b->language`, which `load_settings` fills and the row labels are built from, and the menu's
 * `m->language`, which the selector is drawn from and nothing wrote after start-up.
 *
 * `nr_menu_bridge_refresh` is the one function every path that changes the language goes through,
 * so the fix is a push there - and the reason it uses `nr_menu_set_language` rather than
 * `nr_menu_inject_language` is checked here too: the injector raises `language_changed`, which
 * makes the bridge rewrite the settings file on the next update, so a refresh would look like the
 * user had just asked for the language.
 */
#include "nr_menu.h"
#include "nr_menu_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("the panel's language, as the settings file sets it\n");

    /* A settings file that asks for Chinese, which is language 1. */
    const char *path = "nr-menu-lang-test-settings.json";
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FAIL  could not write the fixture\n"); return 1; }
    fputs("{\n  \"menu_language\": 1,\n  \"render_scale\": 0.8\n}\n", f);
    fclose(f);

    /* The bridge is created with English - the startup default the selector used to get stuck on. */
    nr_menu_bridge *b = nr_menu_bridge_create_headless(path, 0);
    if (!b) { printf("  FAIL  no bridge\n"); remove(path); return 1; }

    char detail[160];
    snprintf(detail, sizeof detail, "bridge language = %d (1 is Chinese)",
             nr_menu_bridge_language(b));
    check(nr_menu_bridge_language(b) == 1,
          "the file's language is what the bridge reports", detail);

    /* The row label proves the rows are drawn in Chinese, which is what the user saw working. */
    const char *label = nr_menu_bridge_row_label(b, 0, nr_menu_bridge_language(b));
    snprintf(detail, sizeof detail, "\"%s\"", label ? label : "(null)");
    check(label && strcmp(label, "Render scale") != 0,
          "and the rows are translated", detail);

    /* The selector's own value. A headless bridge has no menu, so this checks the source-level
     * contract instead: the refresh must push the bridge's language into the menu through the
     * setter that does not raise the change flag. */
    FILE *layer = fopen("src/layer/nr_menu_bridge.c", "rb");
    if (!layer) { printf("  FAIL  nr_menu_bridge.c was not readable\n"); remove(path); return 1; }
    static char source[400000];
    size_t n = fread(source, 1, sizeof source - 1, layer);
    source[n] = '\0';
    fclose(layer);

    check(strstr(source, "nr_menu_set_language(b->menu, b->language)") != NULL,
          "the refresh pushes the language into the menu",
          strstr(source, "nr_menu_set_language(b->menu, b->language)")
              ? "nr_menu_set_language" : "MISSING - the selector will fall back to English");

    check(strstr(source, "nr_menu_inject_language(b->menu, b->language)") == NULL,
          "and does it WITHOUT raising the change flag",
          strstr(source, "nr_menu_inject_language(b->menu, b->language)")
              ? "the injector is used - every refresh queues a settings rewrite"
              : "injector not used");

    /* And the file untouched: a refresh plus an update must not rewrite `menu_language`. */
    nr_menu_bridge_update(b, 1.0f / 60.0f);
    nr_menu_bridge_refresh(b);
    nr_menu_bridge_update(b, 1.0f / 60.0f);

    f = fopen(path, "rb");
    char text[4096] = {0};
    int ok = 0;
    if (f) { size_t got = fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); ok = 1; }
    snprintf(detail, sizeof detail, "%s", ok ? "readable" : "unreadable");
    check(ok, "the settings file is still there", detail);

    /* `menu_language` must still say 1: a refresh that raised the flag would have made the bridge
     * write the language back, and a bug there shows up as a different value rather than a lost
     * key. */
    int mentions = strstr(text, "\"menu_language\"") != NULL;
    /* The value, not its spelling: the writer's own spacing is not this test's business. */
    const char *at = strstr(text, "\"menu_language\"");
    int value_ok = 0;
    if (at) {
        const char *colon = strchr(at, ':');
        if (colon) value_ok = (atoi(colon + 1) == 1);
    }
    snprintf(detail, sizeof detail, "%.40s", at ? at : "(missing)");
    check(value_ok, "and still says menu_language 1", detail);

    nr_menu_bridge_destroy(b);
    remove(path);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
