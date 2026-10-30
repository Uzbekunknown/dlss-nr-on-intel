/* The layer's own pacing: three settings that are not knobs, and the conflicts between them.
 *
 * Asynchronous mode, the GPU image path, and how many presents each result covers live in the
 * proxy's `nr-env.txt` rather than the settings file, because the LAYER reads them once at
 * start-up while the daemon reads the settings file every frame. So the panel writes that file,
 * and the setup window writes the same one - which is why the conflicts have to be applied in one
 * place and not duplicated per caller.
 *
 * The rule the setup window states, and the one asserted here: the GPU image path requires
 * asynchronous mode OFF and every frame processed. Turning the path on must therefore clear both,
 * and turning either of those on must clear the path. A panel that let all three be true at once
 * would silently skip the effect, because the layer refuses that combination.
 */
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_strings.h"   /* nr_menu_text: the table the pacing rows are read from */
#include "nr_layer_cadence.h"  /* CADENCE_NAME_STORE: the same number nr_layer.c sizes its store with */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>   /* CreateDirectoryA, FindFirstFileA, DeleteFileA - no subshell needed */
#else
#include <sys/stat.h>
#include <ftw.h>       /* nftw, for wipe_tree */
#include <unistd.h>
#endif

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

#ifdef _WIN32
/* A directory and everything under it, gone. `RemoveDirectoryA` refuses a non-empty one, so this
 * walks it depth-first. Failures are ignored: a scratch folder that will not delete is untidy, not
 * wrong, and the assertions above do not depend on it being gone. */
static void wipe_tree(const char *root)
{
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", root);
    WIN32_FIND_DATAA found;
    HANDLE scan = FindFirstFileA(pattern, &found);
    if (scan == INVALID_HANDLE_VALUE) { RemoveDirectoryA(root); return; }
    do {
        if (found.cFileName[0] == '.' &&
            (found.cFileName[1] == '\0' ||
             (found.cFileName[1] == '.' && found.cFileName[2] == '\0')))
            continue;
        char child[1024];
        snprintf(child, sizeof child, "%s\\%s", root, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            wipe_tree(child);
        else
            DeleteFileA(child);
    } while (FindNextFileA(scan, &found));
    FindClose(scan);
    RemoveDirectoryA(root);
}

/* A whole path's directories, one level at a time. See `make_installation` for why this exists
 * rather than `system("mkdir")`. */
static int make_dir_chain(const char *path)
{
    char partial[1024];
    for (const char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            size_t len = (size_t)(p - path);
            if (len == 0 || (len == 2 && path[1] == ':')) continue;   /* the drive, or a leading \ */
            if (len + 1 > sizeof partial) return 0;
            memcpy(partial, path, len);
            partial[len] = '\0';
            if (!CreateDirectoryA(partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return 0;
        }
    }
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    return 1;
}
#else
/* POSIX: `nftw` does the same walk, and `mkdir -p` does the same chain. This fixture is
 * Windows-only in practice - the pacing rows live in the layer, which is Windows-only - but the
 * source is compiled everywhere, so the non-Windows arms have to exist. */
static int wipe_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return flag == FTW_D ? rmdir(path) : unlink(path);
}
static void wipe_tree(const char *root)
{
    nftw(root, wipe_cb, 8, FTW_DEPTH | FTW_PHYS);
}
static int make_dir_chain(const char *path)
{
    char cmd[1100];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    return system(cmd) == 0;
}
#endif

/* A whole installation, as far as this is concerned: `work/nr_settings.json` beside
 * `dlss-nr/nr-env.txt`, which is the layout `env_file_path` derives.
 *
 * The directories are made one level at a time with `CreateDirectoryA`, not `system("mkdir ...")`.
 * Two reasons:
 *
 *   - `system` starts a subshell, and a subshell is one more thing a non-interactive session can
 *     refuse us.
 *   - **`CreateDirectoryA` creates ONE level and fails with ERROR_PATH_NOT_FOUND when the parent is
 *     missing.** `mkdir a\b` builds both. So the chain has to be walked, or the fixture dies on its
 *     first `fopen` with the path looking perfectly writable.
 *
 * A direct syscall is also the same call `nr_file.c` uses, so the fixture exercises the real thing.
 *
 * **What this does NOT fix, and is not this file's to fix:** running the fixture from a working
 * directory under %TEMP% while the executable itself lives on another drive can fail with
 * ERROR_ACCESS_DENIED on the very first write - Windows refusing a process started from `F:\` the
 * right to create files in the user's temporary folder. `test_menu_ui_fixtures.py` runs the
 * fixtures from `%TEMP%\nr-menu-fixture-<pid>` for exactly that reason on some machines and works on
 * others; copying the executable into the scratch directory first makes it work everywhere. That is
 * a property of the harness, not of the assertions below, and the assertions are unchanged by it. */
static int make_installation(const char *root, const char *env_body)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s\\work", root);
    if (!make_dir_chain(dir)) return 0;
    snprintf(dir, sizeof dir, "%s\\dlss-nr", root);
    if (!make_dir_chain(dir)) return 0;

    char path[1024];
    snprintf(path, sizeof path, "%s\\work\\nr_settings.json", root);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs("{\n  \"render_scale\": 0.8\n}\n", f);
    fclose(f);

    /* The setup window's profile, beside it. `NR_ROOT` in the environment file names this
     * directory, which is how the panel finds the profile to keep in step. */
    snprintf(path, sizeof path, "%s\\work\\windows-profile.json", root);
    f = fopen(path, "wb");
    if (!f) return 0;
    fputs("{\n"
          "  \"root\": \"" , f);
    fputs(root, f);
    fputs("\",\n"
          "  \"game_exe\": \"C:\\\\game\\\\game.exe\",\n"
          "  \"nr_async\": true,\n"
          "  \"nr_every\": 4,\n"
          "  \"gpu_image_path\": false,\n"
          "  \"preset\": \"A\",\n"
          "  \"keep_me\": \"the panel must not drop what it does not know\"\n"
          "}\n", f);
    fclose(f);

    snprintf(path, sizeof path, "%s\\dlss-nr\\nr-env.txt", root);
    f = fopen(path, "wb");
    if (!f) return 0;
    fputs(env_body, f);
    fclose(f);
    return 1;
}

/* One key of a JSON file, as its literal text, into `out`. Enough for the three booleans and the
 * one integer the panel writes back, and it does not need a parser to say what is in the file. */
static int profile_raw(const char *root, const char *key, char *out, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s\\work\\windows-profile.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    char needle[96];
    snprintf(needle, sizeof needle, "\"%s\": ", key);
    const char *at = strstr(text, needle);
    if (!at) return 0;
    const char *p = at + strlen(needle);
    size_t i = 0;
    while (*p && *p != ',' && *p != '\n' && *p != '}' && i + 1 < size) out[i++] = *p++;
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\r')) --i;
    out[i] = '\0';
    return 1;
}

static int env_line(const char *root, const char *key, char *out, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s\\dlss-nr\\nr-env.txt", root);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    char needle[96];
    snprintf(needle, sizeof needle, "%s=", key);
    const char *at = strstr(text, needle);
    if (!at) return 0;
    const char *p = at + strlen(needle);
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i + 1 < size) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static int env_mentions_count(const char *root, const char *key)
{
    char path[1024];
    snprintf(path, sizeof path, "%s\\dlss-nr\\nr-env.txt", root);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    char needle[96];
    snprintf(needle, sizeof needle, "%s=", key);
    int count = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; ++p) ++count;
    return count;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("the layer's pacing, and the conflicts between the three\n");

    const char *root = "nr-pacing-test";
    /* An untouched install: the setup window's defaults, plus a line the panel must not lose.
     * `NR_ROOT` is where the release lives - the panel finds `work/windows-profile.json` under it
     * - so it names this fixture's own directory, which is the release as far as the panel knows. */
    char body[1024];
    snprintf(body, sizeof body,
        "# Set by the setup window.\n"
        "NR_ROOT=%s\n"
        "NR_LAYER_ASYNC=0\n"
        "NR_GPU_IMAGE=0\n"
        "NR_GPU_TRANSPORT=0\n"
        "NR_GPU_ONLY=0\n"
        "NR_LAYER_LIVE=1\n"
        /* The network, as the installer writes it. The row seeds its value from
         * this line, and two assertions below require the line to survive the panel
         * switching - which it can only do if it is here. */
        "NR_PRESET=A\n"
        "NR_PYTHON=C:\\python.exe\n", root);
    if (!make_installation(root, body)) { printf("  FAIL  could not build the fixture\n"); return 1; }

    /* The production path is the `nr-env.txt` beside the layer DLL. A fixture has no installation
     * around it, so it names the file it just built. */
    char env_path[1024];
    snprintf(env_path, sizeof env_path, "%s\\dlss-nr\\nr-env.txt", root);
    nr_menu_bridge_use_env_file(env_path);

    char settings[1024];
    snprintf(settings, sizeof settings, "%s\\work\\nr_settings.json", root);
    nr_menu_bridge *b = nr_menu_bridge_create_headless(settings, 0);
    if (!b) { printf("  FAIL  no bridge\n"); return 1; }

    char detail[192];
    char value[32];

    /* ---- the file is found and read ---- */
    check(nr_menu_bridge_pacing_available(b), "the environment file is located",
          nr_menu_bridge_pacing_available(b) ? "available" : "NOT FOUND - the panel would offer nothing");

    snprintf(detail, sizeof detail, "async=%d gpu=%d every=%d",
             nr_menu_bridge_pacing_async(b), nr_menu_bridge_pacing_gpu_image(b),
             nr_menu_bridge_pacing_every(b));
    check(!nr_menu_bridge_pacing_async(b) && !nr_menu_bridge_pacing_gpu_image(b) &&
          nr_menu_bridge_pacing_every(b) == 1,
          "and its values are what the panel starts from", detail);

    /* ---- turning the GPU path on clears the other two ---- */
    {
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 1);
        nr_menu_bridge_set_pacing(b, NR_PACING_EVERY, 3);
        snprintf(detail, sizeof detail, "before: async=%d every=%d",
                 nr_menu_bridge_pacing_async(b), nr_menu_bridge_pacing_every(b));
        check(nr_menu_bridge_pacing_async(b) && nr_menu_bridge_pacing_every(b) == 3,
              "asynchronous and a slower cadence can be set", detail);

        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        snprintf(detail, sizeof detail, "after: gpu=%d async=%d every=%d",
                 nr_menu_bridge_pacing_gpu_image(b), nr_menu_bridge_pacing_async(b),
                 nr_menu_bridge_pacing_every(b));
        /* The path requires both, so both are cleared - and this is the conflict the setup
         * window's note describes rather than a preference. */
        check(nr_menu_bridge_pacing_gpu_image(b) == 1 &&
              nr_menu_bridge_pacing_async(b) == 0 &&
              nr_menu_bridge_pacing_every(b) == 1,
              "turning the GPU path on clears asynchronous mode and the cadence", detail);
    }

    /* ---- and each of the other two clears the path ---- */
    {
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 1);
        snprintf(detail, sizeof detail, "gpu=%d after turning asynchronous on",
                 nr_menu_bridge_pacing_gpu_image(b));
        check(nr_menu_bridge_pacing_gpu_image(b) == 0,
              "turning asynchronous mode on clears the GPU path", detail);

        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        nr_menu_bridge_set_pacing(b, NR_PACING_EVERY, 2);
        snprintf(detail, sizeof detail, "gpu=%d after a slower cadence",
                 nr_menu_bridge_pacing_gpu_image(b));
        check(nr_menu_bridge_pacing_gpu_image(b) == 0,
              "choosing fewer than every frame clears the GPU path", detail);
    }

    /* ---- Save and Reset do not govern the pacing rows, and the rows do not wait for Save ---- */
    {
        /* The three rows are the LAYER's start-up settings and live in `nr-env.txt`; Save and
         * Reset are the settings file's, which is the daemon's. These checks pin the separation
         * down in both directions, because it is the thing a reader is most likely to assume
         * otherwise: "a panel with a Save button must save everything" is a reasonable guess and
         * it is wrong here.
         *
         * Direction 1: moving a row writes the file on its own. Nothing is saved afterwards.
         * Direction 2: Save and Reset leave that file byte for byte alone. */
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 0);
        nr_menu_bridge_set_pacing(b, NR_PACING_EVERY, 4);

        char path[1024];
        snprintf(path, sizeof path, "%s\\dlss-nr\\nr-env.txt", root);
        char before[8192] = "";
        FILE *f = fopen(path, "rb");
        if (!f) { printf("  FAIL  the environment file vanished\n"); return 1; }
        size_t n = fread(before, 1, sizeof before - 1, f);
        before[n] = 0;
        fclose(f);

        snprintf(detail, sizeof detail, "NR_LAYER_ASYNC=%s, NR_LAYER_LIVE=%s",
                 env_line(root, "NR_LAYER_ASYNC", value, sizeof value) ? value : "?",
                 env_line(root, "NR_LAYER_LIVE", value, sizeof value) ? value : "?");
        check(strstr(before, "NR_LAYER_ASYNC=0") && strstr(before, "NR_LAYER_LIVE=4"),
              "a row moved writes the file with no Save pressed", detail);

        /* Reset has two paths and both must leave the pacing alone. This is the headless one
         * (`nr_menu_bridge_reset`, taken when there is no menu); the menu's own is the
         * `nr_menu_take_reset` branch in `nr_menu_bridge_update`, which runs the same body -
         * `load_defaults`, the moved flags, the preview - and likewise writes no pacing.
         *
         * The preview is what proves Reset ran at all. Without that, "the pacing file did not
         * change" would also pass for a Reset that never happened, which is the shape of check
         * that passes for the wrong reason - mutation testing the reset branch caught exactly
         * that, the file check going green because nothing had run. */
        char preview[1024];
        snprintf(preview, sizeof preview, "%s.live", settings);
        remove(preview);
        nr_menu_bridge_reset(b);
        f = fopen(preview, "rb");
        check(f != NULL, "Reset wrote the preview, so it ran", f ? "preview present" : "NO PREVIEW");
        if (f) fclose(f);
        char after_reset[8192] = "";
        f = fopen(path, "rb");
        n = f ? fread(after_reset, 1, sizeof after_reset - 1, f) : 0;
        after_reset[n] = 0;
        if (f) fclose(f);
        check(strcmp(before, after_reset) == 0, "Reset leaves the pacing file byte for byte",
              strcmp(before, after_reset) == 0 ? "unchanged" : "REWRITTEN");

        /* Save: it writes the settings file and the preview, nothing of the pacing. */
        nr_menu_bridge_save(b);
        char after_save[8192] = "";
        f = fopen(path, "rb");
        n = f ? fread(after_save, 1, sizeof after_save - 1, f) : 0;
        after_save[n] = 0;
        if (f) fclose(f);
        check(strcmp(before, after_save) == 0, "Save leaves the pacing file byte for byte",
              strcmp(before, after_save) == 0 ? "unchanged" : "REWRITTEN");

        char saved[1024];
        snprintf(saved, sizeof saved, "%s\\work\\nr_settings.json", root);
        f = fopen(saved, "rb");
        check(f != NULL, "and Save is what leaves the settings file", f ? "written" : "MISSING");
        if (f) fclose(f);

        /* Reset must not have reached the pacing values either, on screen or in the bridge. */
        snprintf(detail, sizeof detail, "async=%d every=%d after Reset",
                 nr_menu_bridge_pacing_async(b), nr_menu_bridge_pacing_every(b));
        check(nr_menu_bridge_pacing_async(b) == 0 && nr_menu_bridge_pacing_every(b) == 4,
              "and Reset does not reset the rows it does not own", detail);
    }

    /* ---- Reset returns the picture to its defaults but keeps the NETWORK ---- */
    {
        /* Reset used to put the network back to the first name in the table, `ht` (A, 71 blocks).
         * A user who had chosen `cc_small_fp8` (B, 31 blocks, about half the time) was moved back
         * onto the slower model by pressing Reset, with no message: the row simply showed the
         * other name. Seen on this machine - a Reset took `weights` from cc_small_fp8 to ht along
         * with detail 1.75 -> 0.75 and intensity 2.0 -> 1.0, and the daemon's next start logged
         * `network preset A, 71 blocks`.
         *
         * The network is a choice about what runs, not how much of it is applied, so Reset leaves
         * it; everything that shapes the picture still goes back. */
        int network = -1, detail_row = -1;
        for (int i = 0; i < nr_menu_bridge_knob_count(); ++i) {
            const char *name = nr_menu_bridge_knob_name(i);
            if (strcmp(name, "weights") == 0) network = i;
            if (strcmp(name, "detail_strength") == 0) detail_row = i;
        }
        snprintf(detail, sizeof detail, "weights=%d detail=%d", network, detail_row);
        /* **`weights` must NOT be in the table.** It was a `choice` row naming a model file, and
         * it gave the project two settings for one fact: `NR_PRESET` in the environment and this
         * row in the panel. The two disagreed in the games - a B chosen here logged as "network
         * preset A" - and `NR_PRESET=B` loaded A on an install whose manifest the row's own
         * extractor had not written. The network is one setting now, chosen in the setup window;
         * this fails if a second path comes back. */
        check(network < 0, "the network is not a knob any more", detail);
        check(detail_row >= 0, "the picture knobs are still there", detail);

        /* Reset is a plain reset now: a picture parameter goes back to its default, and there is
         * no network index for it to preserve. The old special case existed so a Reset would not
         * move a user off B; with no row, that cannot happen. */
        nr_menu_bridge_set(b, detail_row, 1.75f);
        nr_menu_bridge_reset(b);
        snprintf(detail, sizeof detail, "detail after Reset = %s",
                 nr_menu_bridge_row_value(b, detail_row));
        check(strcmp(nr_menu_bridge_row_value(b, detail_row), "1") == 0,
              "Reset returns a picture parameter to its default", detail);
    }

    /* ---- what the four variables mean, and one of them is NOT the panel's ---- */
    {
        /* `NR_GPU_ONLY` is the STRICT half of the GPU path: with it on, the layer refuses the CPU
         * pixel fallback and keeps the game's own image. The setup window writes it always as
         * "0" and says why - "Never strict: where a game cannot keep its frames on the GPU, NR
         * takes its usual path and the layer's line in the log says why, where the strict mode
         * left the game's frame alone and said nothing a user could see" (issue #17).
         *
         * The panel used to write it as a copy of the GPU image path, so turning that row on also
         * turned strict mode on: in a game whose shared path cannot run, the user got the game's
         * own frame and no effect at all - the exact failure issue #17 removed. This is the
         * regression check for that, and it fails if the two are ever tied together again. */
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        char strict[32] = "";
        int have = env_line(root, "NR_GPU_ONLY", strict, sizeof strict);
        snprintf(detail, sizeof detail, "NR_GPU_ONLY=%s beside NR_GPU_IMAGE=1",
                 have ? strict : "absent");
        check(have && strcmp(strict, "0") == 0,
              "the GPU path does not turn strict mode on with it", detail);
        /* Still written, not omitted: an install that had strict mode on is cleared by this. */
        check(have, "and strict mode is written as off rather than left alone", detail);
    }

    /* ---- the file is rewritten, and only those lines ---- */
    {
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        check(env_line(root, "NR_GPU_IMAGE", value, sizeof value) && strcmp(value, "1") == 0,
              "the written file says the path is on", value);
        check(env_line(root, "NR_LAYER_ASYNC", value, sizeof value) && strcmp(value, "0") == 0,
              "and that asynchronous mode is off", value);
        check(env_line(root, "NR_LAYER_LIVE", value, sizeof value) && strcmp(value, "1") == 0,
              "and that the cadence is every frame", value);

        /* One line each: the substitute-in-place and the append must not both run. */
        char counts[96];
        snprintf(counts, sizeof counts, "GPU_IMAGE x%d, ASYNC x%d, LIVE x%d",
                 env_mentions_count(root, "NR_GPU_IMAGE"),
                 env_mentions_count(root, "NR_LAYER_ASYNC"),
                 env_mentions_count(root, "NR_LAYER_LIVE"));
        check(env_mentions_count(root, "NR_GPU_IMAGE") == 1 &&
              env_mentions_count(root, "NR_LAYER_ASYNC") == 1 &&
              env_mentions_count(root, "NR_LAYER_LIVE") == 1,
              "and each appears exactly once", counts);

        /* Every line the panel does not own is still there, byte for byte. */
        check(env_line(root, "NR_PYTHON", value, sizeof value) &&
              strcmp(value, "C:\\python.exe") == 0,
              "the lines the panel does not own survive", value);
        check(env_line(root, "NR_ROOT", value, sizeof value) &&
              strcmp(value, root) == 0,
              "including the release root", value);

        FILE *f = fopen("nr-pacing-test\\dlss-nr\\nr-env.txt", "rb");
        int kept_comment = 0;
        if (f) {
            static char text[8192];
            size_t n = fread(text, 1, sizeof text - 1, f);
            text[n] = '\0';
            fclose(f);
            kept_comment = strstr(text, "# Set by the setup window.") != NULL;
        }
        check(kept_comment, "and the header comment", kept_comment ? "kept" : "LOST");
    }

    /* ---- round trip: what the panel wrote is what the next start reads ---- */
    {
        /* This is the start-up half of the same question, and it is the one that cannot be
         * answered by looking at the writer: a value written in a shape the reader does not
         * recognise would be a setting that silently reverts at the next launch. So write the
         * rows, throw the bridge away, and build a new one over the same files - which is what a
         * game restart does.
         *
         * The cadence is the interesting one (1-4 becomes the text the row shows and back), and
         * the conflicts are the pair that could be written inconsistently by a half-applied
         * change. */
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 0);
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 0);
        nr_menu_bridge_set_pacing(b, NR_PACING_EVERY, 3);

    /* ---- the Network row: live in the game, saved only on Save ---- */
    {
        /* **This row is what replaced the `weights` knob**, and its contract is the one the owner
         * asked for: switching it changes the picture now, and commits nothing.
         *
         * That is why it is NOT a pacing write any more. The three pacing rows go into
         * `nr-env.txt` the moment they change, because the layer reads that file once at start-up
         * and there is no Save for them. The network is a choice the user may be trying, so it
         * goes into the live preview - which the daemon polls - and only Save writes it down. */
        snprintf(detail, sizeof detail, "preset=%d (0=A 1=B)", nr_menu_bridge_preset(b));
        check(nr_menu_bridge_preset(b) == 0, "the Network row starts on A, from the layer's file",
              detail);

        int set_ok = nr_menu_bridge_set_pacing(b, NR_PACING_PRESET, 1);
        snprintf(detail, sizeof detail, "returned %d, preset now %d",
                 set_ok, nr_menu_bridge_preset(b));
        check(set_ok && nr_menu_bridge_preset(b) == 1, "the row can be switched to B", detail);

        /* ---- 1. the live preview carries it: this is the swap the user sees ---- */
        char live_path[1200];
        snprintf(live_path, sizeof live_path, "%s.live", settings);
        static char text[65536];
        long got = -1;
        {
            FILE *f = fopen(live_path, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        snprintf(detail, sizeof detail, "%s", got > 0 ? text : "(no live file)");
        check(got > 0 && strstr(text, "\"preset\": \"B\""),
              "the live preview carries the network, which is what the daemon polls", detail);

        /* ---- 2. the layer's file is untouched: a later start still loads A ---- */
        got = -1;
        {
            FILE *f = fopen(env_path, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        snprintf(detail, sizeof detail, "%s", got > 0 ? text : "(missing)");
        check(got > 0 && strstr(text, "NR_PRESET=A") && !strstr(text, "NR_PRESET=B"),
              "and the layer's file is NOT rewritten, so a later start still loads A", detail);

        /* ---- 3. the settings file is untouched: nothing is remembered yet ---- */
        got = -1;
        {
            FILE *f = fopen(settings, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        snprintf(detail, sizeof detail, "%s", got > 0 ? text : "(missing)");
        check(got > 0 && !strstr(text, "preset"),
              "and the settings file is NOT written, so nothing is committed", detail);

        /* ---- 4. a pacing change before Save writes the SAVED letter, not the previewed one ---- */
        nr_menu_bridge_set_pacing(b, NR_PACING_EVERY, 3);
        got = -1;
        {
            FILE *f = fopen(env_path, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        snprintf(detail, sizeof detail, "%s", got > 0 ? text : "(missing)");
        check(got > 0 && strstr(text, "NR_PRESET=A") && !strstr(text, "NR_PRESET=B"),
              "a pacing change keeps the layer's file on A", detail);
        char raw[64];
        int have = profile_raw(root, "preset", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile preset=%s", have ? raw : "ABSENT");
        check(have && strcmp(raw, "\"A\"") == 0,
              "and the profile on A: an unsaved switch is not the setup window's choice", detail);

        /* ---- 5. Save commits it where the setup window keeps it ---- */
        nr_menu_bridge_save(b);
        got = -1;
        {
            FILE *f = fopen(env_path, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        int lines = 0;
        for (const char *q = text; (q = strstr(q, "NR_PRESET=")) != NULL; ++q) ++lines;
        snprintf(detail, sizeof detail, "%d NR_PRESET line(s): %s", lines,
                 got > 0 ? text : "(missing)");
        check(got > 0 && strstr(text, "NR_PRESET=B") && lines == 1,
              "Save writes NR_PRESET=B, once, which is what a later start reads", detail);
        have = profile_raw(root, "preset", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile preset=%s", have ? raw : "ABSENT");
        check(have && strcmp(raw, "\"B\"") == 0, "and the profile follows it", detail);
        got = -1;
        {
            FILE *f = fopen(settings, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        snprintf(detail, sizeof detail, "%s", got > 0 ? text : "(missing)");
        check(got > 0 && !strstr(text, "preset"),
              "and the settings file carries no network: one setting, one place", detail);

        /* ---- 6. and back to A the same way, which later checks here rely on ---- */
        nr_menu_bridge_set_pacing(b, NR_PACING_PRESET, 0);
        nr_menu_bridge_save(b);
        got = -1;
        {
            FILE *f = fopen(env_path, "rb");
            if (f) { got = (long)fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f); }
            else text[0] = '\0';
        }
        have = profile_raw(root, "preset", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile preset=%s; %s", have ? raw : "ABSENT",
                 got > 0 ? text : "(missing)");
        check(got > 0 && strstr(text, "NR_PRESET=A") && have && strcmp(raw, "\"A\"") == 0,
              "a Save of A puts both back", detail);
    }

        nr_menu_bridge_destroy(b);
        b = nr_menu_bridge_create_headless(settings, 0);
        if (!b) { printf("  FAIL  no bridge on the restart\n"); return 1; }
        snprintf(detail, sizeof detail, "after a restart: async=%d gpu=%d every=%d",
                 nr_menu_bridge_pacing_async(b), nr_menu_bridge_pacing_gpu_image(b),
                 nr_menu_bridge_pacing_every(b));
        check(!nr_menu_bridge_pacing_async(b) && !nr_menu_bridge_pacing_gpu_image(b) &&
              nr_menu_bridge_pacing_every(b) == 3,
              "a restart reads back the cadence that was written", detail);

        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        nr_menu_bridge_destroy(b);
        b = nr_menu_bridge_create_headless(settings, 0);
        if (!b) { printf("  FAIL  no bridge after the GPU path restart\n"); return 1; }
        snprintf(detail, sizeof detail, "after a restart: async=%d gpu=%d every=%d",
                 nr_menu_bridge_pacing_async(b), nr_menu_bridge_pacing_gpu_image(b),
                 nr_menu_bridge_pacing_every(b));
        check(nr_menu_bridge_pacing_gpu_image(b) == 1 && !nr_menu_bridge_pacing_async(b) &&
              nr_menu_bridge_pacing_every(b) == 1,
              "and a restart reads back the GPU path with the two it cleared", detail);
    }

    /* ---- an install that is not shaped as expected offers nothing ---- */
    {
        nr_menu_bridge_destroy(b);
        const char *odd = "nr-pacing-test-odd";
        char path[1024];
        snprintf(path, sizeof path, "%s", odd);
        (void)path;
        /* A settings file with no `work/` parent: `env_file_path` refuses, and the panel must
         * then not offer the three rows at all. */
        nr_menu_bridge_use_env_file("Z:\\no\\such\\nr-env.txt");
        b = nr_menu_bridge_create_headless("nr-pacing-test-odd/settings.json", 0);
        if (!b) { printf("  FAIL  no bridge on the odd shape\n"); return 1; }
        snprintf(detail, sizeof detail, "available=%d", nr_menu_bridge_pacing_available(b));
        check(!nr_menu_bridge_pacing_available(b),
              "an install of an unexpected shape offers no pacing rows", detail);

        int wrote = nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 1);
        snprintf(detail, sizeof detail, "set_pacing returned %d", wrote);
        check(wrote == 0, "and refuses to write", detail);

        /* Back to the real installation: the override is process-wide, so leaving it here would
         * make every later block run against a path that cannot be written - which is how the
         * profile and blocker checks first failed, for a reason that had nothing to do with what
         * they were testing. */
        nr_menu_bridge_destroy(b);
        nr_menu_bridge_use_env_file(env_path);
        b = nr_menu_bridge_create_headless(settings, 0);
        if (!b) { printf("  FAIL  no bridge after the odd shape\n"); return 1; }
        snprintf(detail, sizeof detail, "available=%d", nr_menu_bridge_pacing_available(b));
        check(nr_menu_bridge_pacing_available(b),
              "and the real installation is reachable again", detail);
    }

    /* ---- the profile follows the file the panel owns ---- */
    {
        /* Two writers used to share the five pacing lines of `nr-env.txt` with nothing ordering
         * them: this panel, and the setup window's `set_pacing`, which wrote them from the
         * profile. A panel change followed by any window pacing save silently reverted it -
         * measured here as `NR_GPU_IMAGE=1 NR_LAYER_ASYNC=0 NR_LAYER_LIVE=1` in the file against
         * `gpu_image_path=false nr_async=true nr_every=4` in the profile.
         *
         * The rule now: this panel writes both, and the profile is the projection the window
         * reads. The window writes the file too, which is what lets its own frame-pacing controls
         * reach an installed game - it used to write the profile alone and then claim the change
         * took effect at the next start, which it did not. */
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 0);
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);

        char raw[64] = "";
        int have = profile_raw(root, "nr_async", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile nr_async=%s", have ? raw : "ABSENT");
        check(have && strcmp(raw, "false") == 0, "the profile follows asynchronous mode off", detail);

        have = profile_raw(root, "gpu_image_path", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile gpu_image_path=%s", have ? raw : "ABSENT");
        check(have && strcmp(raw, "true") == 0, "and the GPU path on", detail);

        /* The GPU path forces every frame, so the cadence the profile sees is 1. */
        have = profile_raw(root, "nr_every", raw, sizeof raw);
        snprintf(detail, sizeof detail, "profile nr_every=%s", have ? raw : "ABSENT");
        check(have && strcmp(raw, "1") == 0, "and the cadence the path requires", detail);

        /* Whatever this code does not know about is still there: the panel edits the file, it
         * does not regenerate it, and a profile it rebuilt would quietly drop the game. */
        have = profile_raw(root, "keep_me", raw, sizeof raw);
        snprintf(detail, sizeof detail, "keep_me=%s", have ? raw : "LOST");
        check(have && strstr(raw, "must not drop") != NULL,
              "and a key the panel does not know survives", detail);
        have = profile_raw(root, "game_exe", raw, sizeof raw);
        snprintf(detail, sizeof detail, "game_exe=%s", have ? raw : "LOST");
        check(have && strstr(raw, "game.exe") != NULL, "including the game it is for", detail);

        /* The file the layer reads and the profile agree on all three, which is the whole point. */
        char async_env[32] = "", gpu_env[32] = "", every_env[32] = "";
        env_line(root, "NR_LAYER_ASYNC", async_env, sizeof async_env);
        env_line(root, "NR_GPU_IMAGE", gpu_env, sizeof gpu_env);
        env_line(root, "NR_LAYER_LIVE", every_env, sizeof every_env);
        char async_prof[32] = "", gpu_prof[32] = "";
        profile_raw(root, "nr_async", async_prof, sizeof async_prof);
        profile_raw(root, "gpu_image_path", gpu_prof, sizeof gpu_prof);
        snprintf(detail, sizeof detail, "env async=%s gpu=%s every=%s / profile async=%s gpu=%s",
                 async_env, gpu_env, every_env, async_prof, gpu_prof);
        check(strcmp(async_env, "0") == 0 && strcmp(async_prof, "false") == 0 &&
              strcmp(gpu_env, "1") == 0 && strcmp(gpu_prof, "true") == 0 &&
              strcmp(every_env, "1") == 0,
              "and the file and the profile say the same thing", detail);

        /* The file must still be JSON. Replacing a value used to eat the comma after it, so the
         * profile came out as
         *     "nr_async": false
         *     "nr_every": 1
         * which the setup window could not read at all - `nr_every must be a whole number` from a
         * file the panel had just written. Every member but the last has to end in a comma. */
        char path[1024];
        snprintf(path, sizeof path, "%s\\work\\windows-profile.json", root);
        FILE *f = fopen(path, "rb");
        char whole[2048] = "";
        size_t got = f ? fread(whole, 1, sizeof whole - 1, f) : 0;
        whole[got] = 0;
        if (f) fclose(f);
        int members = 0, commas = 0;
        for (size_t i = 0; i + 4 < got; ++i) {
            if (whole[i] == ':' && whole[i + 1] == ' ') {
                ++members;
                const char *p = whole + i;
                while (*p && *p != '\n' && *p != '}') ++p;
                const char *last = p;
                while (last > whole && (last[-1] == ' ' || last[-1] == '\r')) --last;
                if (last > whole && last[-1] == ',') ++commas;
            }
        }
        snprintf(detail, sizeof detail, "%d members, %d with a comma", members, commas);
        check(members >= 4 && commas >= members - 1,
              "and every member but the last still ends in a comma", detail);

        /* A profile from before these keys existed: `nr_async`, `nr_every` and `gpu_image_path`
         * are absent, so they have to be added inside the object rather than appended after the
         * closing brace - which would be a file the window cannot read at all. */
        f = fopen(path, "wb");
        if (f) {
            fputs("{\n  \"root\": \"old\",\n  \"preset\": \"A\"\n}\n", f);
            fclose(f);
        }
        nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 1);
        int have_async = profile_raw(root, "nr_async", raw, sizeof raw);
        snprintf(detail, sizeof detail, "nr_async=%s", have_async ? raw : "ABSENT");
        check(have_async && strcmp(raw, "true") == 0,
              "a profile without the keys gains them", detail);
        have = profile_raw(root, "preset", raw, sizeof raw);
        snprintf(detail, sizeof detail, "preset=%s", have ? raw : "LOST");
        check(have && strcmp(raw, "\"A\"") == 0, "and keeps what it had", detail);
        /* Still valid JSON: the brace has to come last, not before what was added. */
        f = fopen(path, "rb");
        whole[0] = 0;
        got = f ? fread(whole, 1, sizeof whole - 1, f) : 0;
        whole[got] = 0;
        if (f) fclose(f);
        char *brace = strrchr(whole, '}');
        int trailing_ok = brace && strspn(brace + 1, " \t\r\n") == strlen(brace + 1);
        snprintf(detail, sizeof detail, "ends: %s", brace ? brace : "NO BRACE");
        check(trailing_ok, "and the object still closes after what was added", detail);

        /* And that case's commas too: only some members were replaced, so the untouched ones
         * keep theirs while the added ones have to bring one. */
        members = commas = 0;
        for (size_t i = 0; i + 4 < got; ++i) {
            if (whole[i] == ':' && whole[i + 1] == ' ') {
                ++members;
                const char *p = whole + i;
                while (*p && *p != '\n' && *p != '}') ++p;
                const char *last = p;
                while (last > whole && (last[-1] == ' ' || last[-1] == '\r')) --last;
                if (last > whole && last[-1] == ',') ++commas;
            }
        }
        snprintf(detail, sizeof detail, "%d members, %d with a comma", members, commas);
        check(members >= 4 && commas >= members - 1,
              "and it is still comma-separated after a partial rewrite", detail);
    }

    /* ---- the three pacing rows are translated in all four languages ---- */
    {
        /* They were `zh ? ... : ...` in `nr_layer.c`: a two-language choice over a four-language
         * enum, so Russian and Spanish players read English for these three rows while the eleven
         * knobs beside them were translated. Reported from a real game with the panel set to
         * Español and every one of these rows in English. */
        static const int keys[] = {
            NR_STR_ASYNC, NR_STR_ASYNC_HINT, NR_STR_GPU_IMAGE, NR_STR_GPU_IMAGE_HINT,
            NR_STR_EVERY, NR_STR_EVERY_HINT, NR_STR_EVERY_FRAME,
            NR_STR_EVERY_NTH_FEW, NR_STR_EVERY_NTH_MANY,
            NR_STR_GPU_BLOCKED_BOTH, NR_STR_GPU_BLOCKED_DETAIL, NR_STR_GPU_BLOCKED_COLOUR,
        };
        static const char *const key_names[] = {
            "async", "async_hint", "gpu_image", "gpu_image_hint", "every", "every_hint",
            "every_frame", "every_nth_few", "every_nth_many", "gpu_blocked_both",
            "gpu_blocked_detail", "gpu_blocked_colour",
        };
        int count = (int)(sizeof keys / sizeof keys[0]);
        for (int i = 0; i < count; ++i) {
            char shown[256] = "";
            size_t used = 0;
            int ok = 1;
            for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
                const char *text = nr_menu_text(lang, keys[i]);
                if (!text || !*text) ok = 0;
                used += (size_t)snprintf(shown + used, sizeof shown - used, "%s%s",
                                         lang ? " / " : "", (text && *text) ? "ok" : "EMPTY");
            }
            snprintf(detail, sizeof detail, "%s: %s", key_names[i], shown);
            check(ok, "the pacing row text is present in all four languages", detail);
        }
        /* And they differ: a copy of the English row in the Spanish column is what a missing
         * translation looks like, and it would pass the emptiness check above. */
        for (int lang = 1; lang < NR_MENU_LANG_COUNT; ++lang) {
            int same = 0;
            for (int i = 0; i < count; ++i)
                if (strcmp(nr_menu_text(lang, keys[i]), nr_menu_text(0, keys[i])) == 0) ++same;
            snprintf(detail, sizeof detail, "%d of %d still English in %s", same, count,
                     lang == 1 ? "zh" : lang == 2 ? "ru" : "es");
            check(same == 0, "and no language is left saying the English row", detail);
        }
        /* The cadence options are built by replacing the braces in the plural forms, so a
         * translation that dropped them would render "every frames" with the number missing.
         * Both forms carry one, and the English/Spanish/Chinese `many` being the same string as
         * their `few` is fine - only Russian inflects, and this checks the mechanism, not the
         * grammar. The grammar is checked below, where the built options are read back. */
        int braces = 1;
        for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
            if (strstr(nr_menu_text(lang, NR_STR_EVERY_NTH_FEW), "{}") == NULL) braces = 0;
            if (strstr(nr_menu_text(lang, NR_STR_EVERY_NTH_MANY), "{}") == NULL) braces = 0;
        }
        check(braces, "both cadence plural forms keep a {} in all four languages",
              braces ? "four of four" : "ONE OR MORE MISSING");

        /* ---- the store is wide enough, and Russian inflects ----
         *
         * Two separate bugs lived here, and both have to be checked because fixing either one
         * alone leaves the other:
         *
         *   1. `every_names_store[4][24]` held 23 bytes plus the terminator, and the longest
         *      option any language builds is 27 - Russian "каждые 2 кадров". Every Russian option
         *      was cut mid-word: the panel read "каждые 2 кадр". Cyrillic is 2 bytes a character,
         *      which is why counting characters instead of bytes gets the wrong answer.
         *   2. One `{}` in one string cannot choose between Russian's three plural forms. 2, 3
         *      and 4 all want кадра, and 5 and up want кадров.
         *
         * The first is checked by measuring what the loop builds against the store the loop
         * actually uses; the second by reading back what the two forms actually say. Neither is
         * something a running test on a 64-bit machine can observe, which is why both are stated
         * as facts about the table.
         *
         * `CADENCE_NAME_STORE` is the same constant `nr_layer.c` sizes its store with, declared
         * in `nr_layer_cadence.h` so there is one number. An earlier version of this check wrote
         * `<= 48` literally, which meant shrinking the store to 24 did not fail it - the check
         * was measuring the strings and comparing them to a number nobody else used. */
        for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
            const char *forms[2] = { nr_menu_text(lang, NR_STR_EVERY_NTH_FEW),
                                     nr_menu_text(lang, NR_STR_EVERY_NTH_MANY) };
            int worst = 0;
            for (int f = 0; f < 2; ++f) {
                const char *brace = strstr(forms[f], "{}");
                int head = brace ? (int)(brace - forms[f]) : (int)strlen(forms[f]);
                char built[CADENCE_NAME_STORE];
                int n = snprintf(built, sizeof built, "%.*s%d%s", head, forms[f], 2,
                                 brace ? brace + 2 : "");
                if (n > worst) worst = n;
            }
            const char *frame = nr_menu_text(lang, NR_STR_EVERY_FRAME);
            if ((int)strlen(frame) > worst) worst = (int)strlen(frame);
            snprintf(detail, sizeof detail, "longest option %d bytes, store holds %d",
                     worst, CADENCE_NAME_STORE - 1);
            check(worst < CADENCE_NAME_STORE,
                  "the cadence store is wide enough in every language", detail);
        }
        /* Russian is the only language that inflects, so it is the only one whose two forms can
         * differ. English, Spanish and Chinese saying the same thing is correct, not a missing
         * translation - which is why this asserts the Russian forms are the ones that differ. */
        const char *ru_few = nr_menu_text(NR_LANG_RU, NR_STR_EVERY_NTH_FEW);
        const char *ru_many = nr_menu_text(NR_LANG_RU, NR_STR_EVERY_NTH_MANY);
        check(strcmp(ru_few, ru_many) != 0,
              "Russian's two cadence forms differ",
              strcmp(ru_few, ru_many) != 0 ? "кадра for 2-4, кадров for 5+"
                                           : "SAME STRING - one of the counts is wrong");
        check(strstr(ru_few, "\u043a\u0430\u0434\u0440\u0430") != NULL,
              "and the 2-4 form says кадра",
              strstr(ru_few, "\u043a\u0430\u0434\u0440\u0430") ? "кадра" : "NOT кадра");
    }

    /* ---- the GPU row explains itself when the path cannot run ---- */
    {
        /* The daemon declines a frame whose detail or colour strength is not 1, with a sentence
         * that used to go only to its log. The row read "on" while every frame was composed on
         * the CPU - the same shape as issue #17, where the reason was in a place nobody reads. */
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        snprintf(detail, sizeof detail, "blocker with both at 1: %s",
                 nr_menu_bridge_pacing_gpu_blocker(b, 0) ? "SOMETHING" : "none");
        check(nr_menu_bridge_pacing_gpu_blocker(b, 0) == NULL,
              "the GPU path has nothing to explain while both strengths are 1", detail);

        int row = -1;
        for (int i = 0; i < nr_menu_bridge_knob_count(); ++i)
            if (strcmp(nr_menu_bridge_knob_name(i), "detail_strength") == 0) row = i;
        snprintf(detail, sizeof detail, "detail_strength is knob %d", row);
        check(row >= 0, "the detail knob is in the table", detail);

        /* Default 1.0, and 1.05 is "not 1" - the value the daemon refuses. Set rather than moved:
         * `nr_menu_bridge_move` writes the daemon's live preview as well, which this fixture has
         * no installation for, so it reports failure for a reason that has nothing to do with the
         * row's value. The path must be ON for the row to have anything to explain, and the block
         * above left it off. */
        int turned_on = nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 1);
        nr_menu_bridge_set(b, row, 1.05f);
        const char *why = nr_menu_bridge_pacing_gpu_blocker(b, 0);
        snprintf(detail, sizeof detail, "on=%d detail=%s gpu=%d blocker=%s", turned_on,
                 nr_menu_bridge_row_value(b, row), nr_menu_bridge_pacing_gpu_image(b),
                 why ? "reported" : "NONE");
        check(why && strstr(why, "Detail strength") != NULL,
              "and says so when detail strength is not 1", detail);
        const char *why_zh = nr_menu_bridge_pacing_gpu_blocker(b, 1);
        check(why_zh && why_zh != why, "in the panel's own language",
              why_zh ? "translated" : "ENGLISH ONLY");

        /* Off, there is nothing to explain: the row already says "off". */
        nr_menu_bridge_set_pacing(b, NR_PACING_GPU_IMAGE, 0);
        snprintf(detail, sizeof detail, "blocker while off: %s",
                 nr_menu_bridge_pacing_gpu_blocker(b, 0) ? "SOMETHING" : "none");
        check(nr_menu_bridge_pacing_gpu_blocker(b, 0) == NULL,
              "and says nothing while the path is off", detail);
        nr_menu_bridge_set(b, row, 1.0f);
    }

    /* ---- the pacing rows do not depend on the settings file ---- */
    {
        /* They live in `nr-env.txt`; the settings file is the daemon's. Reading one inside the
         * other's parse branch made them die together, and the consequence was visible: a
         * settings file that was absent or empty left the panel offering no pacing rows at all,
         * with `set_pacing` refusing to write - so the three settings that decide how the
         * network is fed became unreachable because an unrelated JSON file was missing. */
        nr_menu_bridge_destroy(b);
        const char *bare = "nr-pacing-test-bare";
        char dir[1024];
        snprintf(dir, sizeof dir, "%s\\dlss-nr", bare);
        if (!make_dir_chain(dir)) { printf("  FAIL  no scratch dir\n"); return 1; }
        snprintf(dir, sizeof dir, "%s\\work", bare);
        make_dir_chain(dir);
        char path[1024];
        snprintf(path, sizeof path, "%s\\dlss-nr\\nr-env.txt", bare);
        FILE *f = fopen(path, "wb");
        fputs("NR_ROOT=C:\\somewhere\\release\n"
              "NR_LAYER_ASYNC=1\n"
              "NR_GPU_IMAGE=0\n"
              "NR_GPU_TRANSPORT=0\n"
              "NR_GPU_ONLY=0\n"
              "NR_LAYER_LIVE=3\n", f);
        fclose(f);

        nr_menu_bridge_use_env_file(path);
        char bare_settings[1024];
        snprintf(bare_settings, sizeof bare_settings, "%s\\work\\nr_settings.json", bare);

        /* No settings file at all. */
        b = nr_menu_bridge_create_headless(bare_settings, 0);
        if (!b) { printf("  FAIL  no bridge on the bare shape\n"); return 1; }
        snprintf(detail, sizeof detail, "available=%d async=%d every=%d",
                 nr_menu_bridge_pacing_available(b), nr_menu_bridge_pacing_async(b),
                 nr_menu_bridge_pacing_every(b));
        check(nr_menu_bridge_pacing_available(b) && nr_menu_bridge_pacing_async(b) &&
              nr_menu_bridge_pacing_every(b) == 3,
              "a missing settings file leaves the pacing rows readable", detail);
        int wrote = nr_menu_bridge_set_pacing(b, NR_PACING_ASYNC, 0);
        snprintf(detail, sizeof detail, "set_pacing returned %d", wrote);
        check(wrote == 1, "and writable", detail);

        /* And an empty one: present, no JSON in it, still nothing to do with pacing. */
        f = fopen(bare_settings, "wb");
        fclose(f);
        nr_menu_bridge_destroy(b);
        b = nr_menu_bridge_create_headless(bare_settings, 0);
        if (!b) { printf("  FAIL  no bridge on the empty settings shape\n"); return 1; }
        snprintf(detail, sizeof detail, "available=%d", nr_menu_bridge_pacing_available(b));
        check(nr_menu_bridge_pacing_available(b),
              "an empty settings file leaves them readable too", detail);
    }

    nr_menu_bridge_destroy(b);
    nr_menu_bridge_use_env_file(NULL);
    /* The scratch installations, removed without a subshell for the same reason they were made
     * without one. `rmdir /s /q` said nothing either way, which is how a stale directory could
     * survive into the next run and have THAT run test the leftovers instead of its own. */
    wipe_tree("nr-pacing-test");
    wipe_tree("nr-pacing-test-odd");
    wipe_tree("nr-pacing-test-bare");
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
