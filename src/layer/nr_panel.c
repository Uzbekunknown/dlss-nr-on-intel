/* The in-game panel's logic, shared by the Vulkan layer and the DirectX 12 proxy. See nr_panel.h.
 *
 * Everything here came out of `nr_menu_layer.c`, where it read and wrote `struct device_data`. The
 * functions kept their names and their bodies; the fields they used moved into `struct nr_panel`
 * under the same names, and the swapchain's extent is now the width and height the host passes. So
 * a comment below that says "the layer" means whichever host is drawing the panel, and its account
 * of how each decision came about is still the account. What is new is said where it is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr_panel.h"
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"
#include "nr_menu_strings.h"
#include "nr_menu_font.h"
#include "nr_raw_mouse.h"
#include "nr_focus.h"
#include "nr_input_guard.h"
#include "nr_layer_cadence.h"

#ifdef _WIN32
#  include <windows.h>
#endif

#ifndef LAYER_TRACE_LOGGING
#define LAYER_TRACE(...) ((void)0)
#else
#define LAYER_TRACE(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifdef _WIN32
/* The window handle this file's Win32 calls want. A typedef rather than `HWND` spelled out, so
 * the declaration below reads the same on both platforms and no call site casts. */
typedef HWND nr_ui_window;
#else
typedef void *nr_ui_window;
#endif

struct nr_panel {
	nr_menu *menu;                  /* the host's */
	nrex_ui *menu_ui;               /* the host's ImGui UI; NULL means the quad path */
	nr_menu_bridge *menu_bridge;    /* the panel's own */
	nr_panel_host host;

	/* This frame's keyboard, swept once in `nr_panel_input`, and the window the frames go to. */
	unsigned char keys[256];
	nr_ui_window window;
	nr_ui_window window_hint;       /* the host's, when it knows: a DXGI swap chain's window */

	int menu_ui_l_was_down;         /* L's previous state: the language key is level-polled, so
	                                 * cycling it needs an edge of its own */
	int menu_ui_ins_was_down;       /* the same, for INS: the panel's only way in */
	/* The same, for the three keys the ImGui build has to own itself.
	 *
	 * **This comment used to say all three live inside `nr_menu_update`'s
	 * `if (m->open && m->read_keys)` block, and that was never true of all three.** S and R were
	 * moved OUT of that block on purpose, because the ImGui build sets `read_keys` to 0 and they
	 * were dead inside it; Home was never in it at all. What was true is that the ImGui build gets
	 * none of the three from `nr_menu.c`, so this file reads them - which is the only part that
	 * matters here.
	 *
	 * The consequence nobody noticed at the time: being outside the block also left S, R and Home
	 * with NO focus check, so an unfocused Home toggled the effect and an S typed in a chat window
	 * wrote the settings file. `nr_menu.c` now gates all three on `read_keys`, and the panel holds
	 * that off while the game is not in front (see `menu_read_keys`), so the two readers agree
	 * about when the keyboard is the game's.
	 *
	 * Home is the effect toggle. It was END, and END is bound by enough games that the key
	 * never arrived; Home was the comparison, and the comparison is what this replaces - so
	 * the key is free and already means "change the picture", which is what the toggle does. */
	int menu_ui_toggle_was_down;
	int menu_ui_save_was_down;
	int menu_ui_reset_was_down;
	/* Whether the OS pointer is currently hidden for the panel. `ShowCursor` is a counter,
	 * so it is pushed down once on open and restored once on close rather than every frame. */
	int menu_ui_cursor_hidden;
	/* The pointer the panel uses, in the picture's pixels; only meaningful while
	 * `menu_ui_have_pointer` is set (see `menu_ui_mouse`). */
	float menu_ui_pointer_x;
	float menu_ui_pointer_y;
	int menu_ui_have_pointer;
	/* The pointer was pushed out past the picture's edge, and the panel waits for the cursor to
	 * come back in; and how far it has been pushed against an edge so far, in the picture's pixels. */
	int menu_ui_outside;
	float menu_ui_push;
	/* The OS position the last delta was measured from. */
	long menu_ui_last_cursor_x;
	long menu_ui_last_cursor_y;
	/* The game's own hold on the cursor, read when the panel pins it and given back when the panel
	 * closes (`menu_cursor_show`): 0 not read since the panel took the pointer, 1 the game held
	 * none, 2 the game held it in `menu_ui_game_clip`, kept from the client area's origin so a
	 * window that moved meanwhile gets its own box back. */
	int menu_ui_game_clip_state;
	long menu_ui_game_clip[4];
	/* Where the panel last set its own one-pixel pin, so a pin left from an earlier frame - the
	 * pointer has moved since - is not taken for the game's box. */
	long menu_ui_pin_x;
	long menu_ui_pin_y;
	int menu_ui_pin_set;
	/* When this panel was last drawn, in seconds from the same clock `nrex_repeat` uses, so ImGui's
	 * keyboard repeat is timed on the real interval between presents. Per panel, not a file
	 * static: the Vulkan layer runs a panel per device, and two devices presenting alternately
	 * would otherwise make each one's delta the time since the OTHER one drew. Zero means "not
	 * drawn yet", which is what makes the first frame fall back to the fixed step. */
	double menu_ui_last_draw;

	/* What each row past the knobs is: one of NR_PACING_*, written by `menu_ui_rows`. The rows used
	 * to be told apart by their position after the knobs, which held while the three pacing rows
	 * always came first; a DirectX 12 panel shows the network row alone, at the place the first
	 * pacing row had, so the row now says what it is. */
	int row_what[MENU_UI_MAX_ROWS];
};

/* The effect's state, through the host: 1 on, 0 off, -1 for an installation with no trigger. */
static int trigger_state(struct nr_panel *data)
{
	return data->host.trigger_get ? data->host.trigger_get(data->host.user) : -1;
}

static void trigger_write(struct nr_panel *data, int on)
{
	if (data->host.trigger_set) data->host.trigger_set(data->host.user, on);
}

/* The menu is opt-out: NR_MENU=0 for a game where even one GetAsyncKeyState a frame is unwelcome,
 * and for anyone bisecting a problem they suspect is ours. Read once. */
int nr_panel_enabled(void)
{
	const char *off = getenv("NR_MENU");
	return !(off && strcmp(off, "0") == 0);
}

/* The font the menu draws with, and the codepoints it has to cover.
 *
 * Read from the system font directory rather than shipped: Microsoft YaHei's regular face carries
 * Latin, Cyrillic, Greek and the Chinese the menu's strings use, and a layer that reads the fonts
 * Windows already has adds nothing to the repository or to a redistribution.
 *
 * The ranges are the ones the menu's own four languages need - Latin with the accents Spanish uses,
 * Cyrillic, and the CJK block - computed once, not a blanket 0x20-0xFFFF: that would bake tens of
 * thousands of glyphs into the atlas and it would take a visible moment to build. */
char *nr_panel_font(size_t *size_out)
{
	static const char *candidates[] = {
		"C:\\Windows\\Fonts\\msyh.ttc",     /* Microsoft YaHei: Latin + Cyrillic + CJK */
		"C:\\Windows\\Fonts\\simhei.ttf",   /* SimHei: a solid fallback for CJK */
		"C:\\Windows\\Fonts\\segoeui.ttf",  /* Latin and Cyrillic, no CJK */
		"C:\\Windows\\Fonts\\arial.ttf",
	};
	*size_out = 0;
	for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
		FILE *f = fopen(candidates[i], "rb");
		if (!f) continue;
		if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
		long len = ftell(f);
		if (len <= 0) { fclose(f); continue; }
		rewind(f);
		char *buf = (char *)malloc((size_t)len);
		if (!buf) { fclose(f); continue; }
		if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); continue; }
		fclose(f);
		*size_out = (size_t)len;
		return buf;
	}
	return NULL;
}
/* Every codepoint the menu can draw, sorted and deduplicated.
 *
 * The three string tables are walked directly - labels, comparison names and hints, in every
 * language - so the set is derived from the text that will actually be drawn rather than from a
 * guess about which blocks it might need. Baking whole blocks instead is what blanked the text
 * completely: the CJK block alone is 20992 glyphs, an atlas cannot hold them, and a font whose
 * atlas fails to build leaves EVERY glyph empty rather than only the Chinese ones. */
int nr_panel_codepoints(int *out, int max)
{
	int n = 0;
	for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
		for (int key = 0; key < NR_MENU_KEY_COUNT; ++key)
			n = nr_font_collect_cjk(nr_menu_strings[lang][key], out, n, max);
		for (int mode = 0; mode < NR_MENU_COMPARE_COUNT; ++mode)
			n = nr_font_collect_cjk(nr_menu_compare_names[lang][mode], out, n, max);
		for (int knob = 0; knob < NR_MENU_KNOB_COUNT; ++knob)
			n = nr_font_collect_cjk(nr_menu_hints[lang][knob], out, n, max);
	}
	/* Sort, then drop duplicates: the UI turns each run of consecutive values into one range pair,
	 * so an unsorted list would produce a range per codepoint and no packing at all. */
	for (int i = 1; i < n; ++i) {
		int v = out[i], j = i - 1;
		while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; --j; }
		out[j + 1] = v;
	}
	int unique = 0;
	for (int i = 0; i < n; ++i)
		if (i == 0 || out[i] != out[i - 1]) out[unique++] = out[i];
	return unique;
}

nr_panel *nr_panel_create(nr_menu *menu, nrex_ui *ui, const char *settings, int language,
                          const nr_panel_host *host)
{
	if (!menu) return NULL;
	struct nr_panel *data = (struct nr_panel *)calloc(1, sizeof *data);
	if (!data) return NULL;
	data->menu = menu;
	data->menu_ui = ui;
	if (host) data->host = *host;
	for (int i = 0; i < MENU_UI_MAX_ROWS; ++i) data->row_what[i] = -1;

	/* ImGui owns the panel now, so the menu stops reading the panel's keys. Leaving both readers on
	 * is what made one keypress act twice - Enter opening a combo while the bridge stepped the
	 * value, INS toggling the panel twice per press. */
	if (ui) nr_menu_set_key_reading(menu, 0);

	/* A documented test hook: open the panel as soon as it exists, so the draw path can be
	 * exercised without a keyboard. Off unless asked for. */
	{
		const char *forced = getenv("NR_MENU_OPEN");
		if (forced && *forced && strcmp(forced, "0") != 0)
			nr_menu_inject_open(menu, 1);
	}

	data->menu_bridge = nr_menu_bridge_create(menu, settings, language);
	if (data->menu_bridge)
		fprintf(stderr, "[nr_layer] settings menu ready; INS opens it (%s)\n", settings);
	else
		fprintf(stderr, "[nr_layer] settings menu ready; INS opens it\n");
	return data;
}

static int menu_ui_rows(struct nr_panel *data, nrex_row *out, int capacity)
{
	int n = 0;
	if (!data->menu_bridge) return 0;
	/* One option-name array and one set of name buffers per row, so two choice knobs cannot
	 * overwrite each other's list and no entry is a pointer into shared scratch. */
	static const char *names[MENU_UI_MAX_ROWS][MENU_UI_MAX_CHOICES];
	static char name_storage[MENU_UI_MAX_ROWS][MENU_UI_MAX_CHOICES][NR_KNOB_CHOICE_NAME_SIZE];
	int language = nr_menu_bridge_language(data->menu_bridge);
	int count = nr_menu_bridge_row_count(data->menu_bridge);
	for (int i = 0; i < count && n < capacity && i < MENU_UI_MAX_ROWS; ++i) {
		nrex_row *r = &out[n];
		memset(r, 0, sizeof *r);
		data->row_what[n] = -1;
		r->label = nr_menu_bridge_row_label(data->menu_bridge, i, language);
		r->value = nr_menu_bridge_row_value(data->menu_bridge, i);
		/* What the row means, shown when the pointer rests on it. Same source as the installer's
		 * tooltip, so the two cannot explain a knob differently. */
		r->hint = nr_menu_bridge_row_hint(data->menu_bridge, i, language);

		/* The knob's own kind decides the control. Every row used to be built as a float, which is
		 * why `profile` - the one choice knob - drew as a slider reading 0.000 and could not be
		 * switched: its value is a word, not a number. */
		int choice_count = nr_menu_bridge_knob_choice_count(i);
		if (choice_count > 0) {
			r->kind = NREX_KIND_CHOICE;
			r->choice_count = choice_count;
			r->choice_index = nr_menu_bridge_knob_choice_index(data->menu_bridge, i);
			/* ImGui wants `const char* const*`, one array per row and the strings themselves
			 * alongside: a single shared array would be overwritten by the next choice row, and a
			 * shared name buffer would make every entry the same string. */
			for (int c = 0; c < choice_count && c < MENU_UI_MAX_CHOICES; ++c) {
				nr_menu_bridge_knob_choice_name(i, c, language, name_storage[i][c],
								sizeof name_storage[i][c]);
				names[i][c] = name_storage[i][c];
			}
			names[i][choice_count < MENU_UI_MAX_CHOICES ? choice_count : MENU_UI_MAX_CHOICES - 1] = NULL;
			r->choices = (const char *const *)names[i];
		} else {
			float low = 0.0f, high = 1.0f, step = 0.01f;
			if (nr_menu_bridge_knob_range(i, &low, &high, &step, NULL) == 0) {
				low = 0.0f; high = 1.0f; step = 0.01f;
			}
			r->kind = NREX_KIND_FLOAT;
			r->min = low;
			r->max = high;
			r->step = step;
		}
		n++;
	}

	/* The layer's own pacing, after the knobs.
	 *
	 * These are not knobs: the daemon does not read them and the settings file does not hold them.
	 * They are the proxy's `nr-env.txt`, which the layer reads once when the game starts - so they
	 * are "next launch" settings, and the panel says so rather than implying a live change.
	 *
	 * Only where the host says it has them: a DirectX 12 game's frames are on the GPU, each on its
	 * own frame, and none of the three means anything there (`nr_panel_host.pacing`).
	 *
	 * The GPU image path is DISABLED, not hidden, while the network runs ahead of the game or the
	 * cadence is not every frame: those two are exactly what the path requires, so leaving it live
	 * would let the user set something the layer then silently ignores. Pressing the control is
	 * also what clears the conflict - the bridge applies it - so the row stays clickable when it
	 * is enabled and explains itself when it is not. */
	if (data->host.pacing && nr_menu_bridge_pacing_available(data->menu_bridge)) {
		/* FOUR names, not three: `choice_count` is 4 (every frame .. every 4th), so a store of
		 * three left the fourth entry pointing at whatever the static held - which ImGui showed
		 * as `*Unknown item*` in the opened combo. This is the same shape as the `*Unknown item*`
		 * the profiles row had: a NULL written over an entry the count still offered. */
		/* 48, not 24: the longest option any language builds is 27 bytes - Russian
		 * "каждые 2 кадров" - and 24 held 23 plus the terminator, so every Russian option was
		 * cut mid-word. Cyrillic is 2 bytes a character and Chinese 3, so this has to be
		 * counted in bytes and never in characters. The number lives in nr_layer_cadence.h so
		 * the test that catches this uses the same one. */
		static char every_names_store[4][CADENCE_NAME_STORE];
		static const char *every_names[5];
		int every = nr_menu_bridge_pacing_every(data->menu_bridge);
		int pipelined = nr_menu_bridge_pacing_async(data->menu_bridge);
		int gpu = nr_menu_bridge_pacing_gpu_image(data->menu_bridge);

		/* The pacing rows read the four-language table, like the knobs do. They used to be
		 * `zh ? ... : ...` written here, which is a two-language choice over a four-language enum:
		 * a Russian or Spanish player got English for these three rows and a translation for the
		 * eleven knobs beside them. `nr_menu_text` is the same lookup the knobs use, so a language
		 * added later reaches all of them at once. */
		int language = nr_menu_language(data->menu);

		if (n < capacity && n < MENU_UI_MAX_ROWS) {
			nrex_row *r = &out[n];
			memset(r, 0, sizeof *r);
			r->label = nr_menu_text(language, NR_STR_ASYNC);
			r->hint = nr_menu_text(language, NR_STR_ASYNC_HINT);
			r->kind = NREX_KIND_FLAG;
			r->choice_index = pipelined ? 1 : 0;
			r->value = pipelined ? "on" : "off";
			data->row_what[n] = NR_PACING_ASYNC;
			n++;
		}
		if (n < capacity && n < MENU_UI_MAX_ROWS) {
			nrex_row *r = &out[n];
			memset(r, 0, sizeof *r);
			r->label = nr_menu_text(language, NR_STR_GPU_IMAGE);
			/* The hint says why the path cannot run when the panel can see a reason. The daemon
			 * declines a frame whose detail or colour strength is not 1, and that refusal used to
			 * appear only in the daemon's log: the row read "on" while every frame was composed
			 * on the CPU. Same shape as issue #17, so the row says it. */
			const char *blocker = nr_menu_bridge_pacing_gpu_blocker(
				data->menu_bridge, language);
			r->hint = blocker ? blocker : nr_menu_text(language, NR_STR_GPU_IMAGE_HINT);
			r->kind = NREX_KIND_FLAG;
			r->choice_index = gpu ? 1 : 0;
			r->value = gpu ? "on" : "off";
			/* One of the two conditions the path states, and the one the panel can see: the
			 * cadence. Asynchronous mode is cleared by turning the path on, so it is not a
			 * reason to refuse; a cadence above one frame is cleared the same way, and is
			 * reported as available because pressing it fixes it. The row is disabled only when
			 * the path is already off AND cannot be turned on - which never happens today, so
			 * the enabled case is the whole of it. Kept explicit for the next condition added. */
			r->disabled = 0;
			data->row_what[n] = NR_PACING_GPU_IMAGE;
			n++;
		}
		if (n < capacity && n < MENU_UI_MAX_ROWS) {
			nrex_row *r = &out[n];
			memset(r, 0, sizeof *r);
			r->label = nr_menu_text(language, NR_STR_EVERY);
			r->hint = nr_menu_text(language, NR_STR_EVERY_HINT);
			r->kind = NREX_KIND_CHOICE;
			for (int c = 0; c < 4; ++c) {
				/* One string per option, from the table, so a language with a different word order
				 * needs no formatting in this file.
				 *
				 * Three keys, not one, because Russian has three plural forms and a single `{}`
				 * cannot choose between them: 1 kadr, 2-4 kadra, 5+ kadrov. English, Spanish and
				 * Chinese do not inflect, so their `few` and `many` are the same string and only
				 * Russian is visibly different. The count picks the key; the `{}` inside takes the
				 * number.
				 *
				 * `every_names_store` holds what was built, and the terminator at 4 is what keeps
				 * the combo from offering a fifth, empty entry. */
				int key = (c == 0) ? NR_STR_EVERY_FRAME
				         : (c + 1 < 5) ? NR_STR_EVERY_NTH_FEW
				         : NR_STR_EVERY_NTH_MANY;
				const char *shape = nr_menu_text(language, key);
				/* 48, not 24. The longest option the CURRENT tables build is 25 bytes - Russian
				 * "каждые 2 кадра" - and the old store held 23 plus the terminator, so every
				 * Russian option was cut mid-word: that is what "каждые 2 кадр" in the panel was.
				 * (27 was the figure before the plural split; `every_nth_many` builds it, and it
				 * is unreachable while the cadence stops at four. 48 covers both.) Cyrillic is 2
				 * bytes a character and Chinese 3, so this has to be counted in bytes and never in
				 * characters. `snprintf` truncates in silence, so its return is checked and an
				 * over-long one falls back to the untranslated shape rather than half a word. It
				 * cannot happen with today's tables; this is the guard for the next language. */
				/* `every_frame` is a whole phrase with no number in it - "every frame",
				 * "каждый кадр" - so it is copied, not formatted. It used to be a separate
				 * `if (c == 0)` branch doing exactly that; when the plural keys arrived it was
				 * folded into the shared path below, and the shared path appends the count when
				 * there is no `{}` - so the first option read "every frame 1". Both forms are
				 * handled here now, and which one applies is decided by the STRING, not by the
				 * index: a translation is free to write the number into its own phrase. */
				int written;
				if (c == 0 && !strstr(nr_menu_text(language, NR_STR_EVERY_FRAME), "{}")) {
					written = snprintf(every_names_store[c], sizeof every_names_store[c], "%s",
					             nr_menu_text(language, NR_STR_EVERY_FRAME));
				} else if (strstr(shape, "{}")) {
					const char *brace = strstr(shape, "{}");
					int head = (int)(brace - shape);
					written = snprintf(every_names_store[c], sizeof every_names_store[c],
					             "%.*s%d%s", head, shape, c + 1, brace + 2);
				} else {
					written = snprintf(every_names_store[c], sizeof every_names_store[c],
					             "%s %d", shape, c + 1);
				}
				if (written < 0 || written >= (int)sizeof every_names_store[c])
					snprintf(every_names_store[c], sizeof every_names_store[c], "%s", shape);
				every_names[c] = every_names_store[c];
			}
			every_names[4] = NULL;
			r->choices = (const char *const *)every_names;
			r->choice_count = 4;
			r->choice_index = every >= 1 && every <= 4 ? every - 1 : 0;
			r->value = every_names[r->choice_index];
			/* Fewer than every frame rules the GPU path out, and choosing one turns it off -
			 * which is the bridge's rule, mirrored here so the panel does not offer a combination
			 * the layer would refuse. */
			r->disabled = 0;
			data->row_what[n] = NR_PACING_EVERY;
			n++;
		}
	}
	/* The network, A or B, after the pacing rows.
	 *
	 * **Not a knob any more, and this row is what replaced one.** `weights` named a model file and
	 * resolved it through a manifest, which gave the project two settings for one fact; the
	 * maintainer's item 3 removed it. But the panel still has to be able to switch the network -
	 * he kept that half - so the row survives as a value that changes `NR_PRESET` itself.
	 *
	 * It is drawn from the bridge's own read of `nr-env.txt`, so what the panel shows is what the
	 * layer will load at the next start. Changing it writes that file immediately, through the
	 * same `set_pacing` path the three rows above use.
	 *
	 * Only offered when the file could be read: an install whose `nr-env.txt` this bridge cannot
	 * identify has no value to show, and a row that draws "A" without knowing is a claim. */
	if (nr_menu_bridge_pacing_available(data->menu_bridge)) {
		if (n < capacity && n < MENU_UI_MAX_ROWS) {
			nrex_row *r = &out[n];
			memset(r, 0, sizeof *r);
			r->label = nr_menu_text(language, NR_STR_WEIGHTS);
			/* No dedicated hint key: the row IS the choice, and the two option labels already say
			 * what A and B are. An empty hint draws nothing. */
			r->hint = NULL;
			/* **CHOICE, not FLAG.** A FLAG draws a checkbox and its label and never renders
			 * `r->value`, so this row showed an empty box labelled "network" with no way to see
			 * which model was running and no key that changed it. That is what the game showed
			 * before this was fixed. A CHOICE draws a combo over `r->choices`, which is what a
			 * two-valued setting wants, and it is the control the `profile` row already uses. */
			r->kind = NREX_KIND_CHOICE;
			int preset = nr_menu_bridge_preset(data->menu_bridge);
			r->choice_index = preset ? 1 : 0;
			/* The same two labels the installer offers for the same choice, so the panel and the
			 * setup window name the networks identically - and both are translated in all four
			 * languages because they come from the table. */
			snprintf(name_storage[n][0], sizeof name_storage[n][0], "%s",
				 nr_menu_text(language, NR_STR_CHOICE_WEIGHTS_HT));
			snprintf(name_storage[n][1], sizeof name_storage[n][1], "%s",
				 nr_menu_text(language, NR_STR_CHOICE_WEIGHTS_CC_SMALL_FP8));
			names[n][0] = name_storage[n][0];
			names[n][1] = name_storage[n][1];
			names[n][2] = NULL;
			r->choices = (const char *const *)names[n];
			r->choice_count = 2;
			r->value = nr_menu_text(language, preset ? NR_STR_CHOICE_WEIGHTS_CC_SMALL_FP8
								 : NR_STR_CHOICE_WEIGHTS_HT);
			data->row_what[n] = NR_PACING_PRESET;
			n++;
		}
	}

	return n;
}
#define window_is_foreground(hwnd) nr_window_is_foreground((void *)(hwnd))

/* Our own process's window: the process, not the thread.
 *
 * `GetWindowThreadProcessId(h, NULL)` returns a THREAD id and it was being compared against
 * `GetCurrentThreadId()`. Those agree only when the window was created by the calling thread - and
 * the panel runs on the game's rendering thread while the window belongs to whichever thread the
 * game created it on. Almost every game has those be different, so every window was rejected and
 * the panel never received a pointer position. The question the check means to ask is "is this our
 * process's window", which is a process comparison. */
#define window_is_ours(h) nr_window_is_ours((void *)(h))

#ifdef _WIN32
/* Enumerate this process's own top-level windows, for the fallback below. */
static BOOL CALLBACK nr_window_enum_proc(HWND h, LPARAM param)
{
	HWND *found = (HWND *)param;
	if (!found) return FALSE;
	if (!IsWindowVisible(h)) return TRUE;
	/* The client area has to be big enough to be a game.
	 *
	 * `GetClientRect` on a hidden or not-yet-realised helper returns 1x1, and the first version of
	 * this accepted exactly that and reported `client=1x1 origin=(0,0)` - so every pointer position
	 * mapped to a coordinate in a window nobody can see. A real game surface is hundreds of pixels
	 * across; anything smaller than this is a helper. */
	RECT r;
	if (!GetClientRect(h, &r)) return TRUE;
	if (r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
	if (!found[0]) found[0] = h;
	/* Prefer the largest: a game may own a small overlay window too, and the swapchain is on the
	 * big one. */
	if (found[1]) {
		RECT best;
		if (GetClientRect(found[1], &best)) {
			long area = (long)(r.right - r.left) * (r.bottom - r.top);
			long best_area = (long)(best.right - best.left) * (best.bottom - best.top);
			if (area <= best_area) return TRUE;
		}
	}
	found[1] = h;
	return TRUE;
}

/* The game's window, for the pointer conversion.
 *
 * The Vulkan layer has no window and no message pump - it lives inside the game's process - so the
 * handle comes from the window the user is looking at. A host that knows the window the frames go
 * to (a DXGI swap chain names it) hands it in, and that answer is taken first. */
static nr_ui_window window_for_thread_for_ui(struct nr_panel *data)
{
	if (data->window_hint && IsWindow(data->window_hint) && window_is_ours(data->window_hint))
		return data->window_hint;
	/* The window the user is looking at, if it is ours. */
	HWND h = GetForegroundWindow();
	if (window_is_ours(h)) return GetAncestor(h, GA_ROOT);
	/* Ours but unfocused: still the game's window, and the panel still wants the pointer. */
	h = GetActiveWindow();
	if (window_is_ours(h)) return GetAncestor(h, GA_ROOT);

	/* Last resort: whatever is under the cursor, walked up to its top-level window. */
	POINT p;
	if (GetCursorPos(&p)) {
		h = WindowFromPoint(p);
		if (window_is_ours(h)) return GetAncestor(h, GA_ROOT);
	}

	/* Nothing above is guaranteed.
	 *
	 * The three probes all ask "which window is the user's", and every one of them can answer with
	 * something that is not this process: a game that is loading, a launcher still in front, an
	 * overlay that took the foreground, or a `SetForegroundWindow` that the OS refused. When that
	 * happens the panel draws - it needs no pointer to draw - and silently receives no input at
	 * all, which reads as "the mouse is broken" with the log saying only "no window".
	 *
	 * Enumerating is the honest fallback: this process's own visible, client-bearing top-level
	 * window is the game, whichever one happens to be in front. */
	{
		/* Two slots: the first acceptable window, and the largest seen. The caller wants the
		 * largest, because a game can own a small overlay window as well as its swapchain. */
		HWND found[2] = { NULL, NULL };
		EnumWindows(nr_window_enum_proc, (LPARAM)found);
		if (found[1]) return found[1];
		if (found[0]) return found[0];
	}
	return NULL;
}
#else
static nr_ui_window window_for_thread_for_ui(struct nr_panel *data)
{
	(void)data;
	return NULL;
}
#endif

/* Show the OS pointer again and lift the pin. `to_game` is the panel closing: a game in front then
 * gets back the box it held the cursor in when the panel took it. Unreal confines the cursor to its
 * window while it aims, once, when it takes the mouse - so the NULL this used to be let the cursor
 * out of Hogwarts Legacy's window for the rest of the session, past the screen's edge with every
 * turn of the camera (the owner, 2026-10-31). Everywhere else - another window in front, the
 * pointer pushed out, the panel going with its host - the cursor is left free: a box around a
 * window nobody is using is the one thing worse than none. */
static void menu_cursor_show(struct nr_panel *data, int to_game)
{
#ifdef _WIN32
	ShowCursor(TRUE);
	if (to_game && data->menu_ui_game_clip_state == 2 && window_is_foreground(data->window)) {
		POINT origin = {0, 0};
		RECT box;
		ClientToScreen(data->window, &origin);
		box.left = origin.x + data->menu_ui_game_clip[0];
		box.top = origin.y + data->menu_ui_game_clip[1];
		box.right = origin.x + data->menu_ui_game_clip[2];
		box.bottom = origin.y + data->menu_ui_game_clip[3];
		ClipCursor(&box);
	} else
		ClipCursor(NULL);
#endif
}

/* Give the system cursor back, whatever holds it: shown, the pin lifted (`menu_cursor_show` says
 * what `to_game` puts back), the clip's guard thread ended and raw input handed back to the game.
 * The next frame that wants it seeds the pointer afresh. */
static void menu_cursor_free(struct nr_panel *data, int to_game)
{
	nr_clip_guard_stop();
	if (data->menu_ui_cursor_hidden) {
		menu_cursor_show(data, to_game);
		data->menu_ui_cursor_hidden = 0;
	}
	data->menu_ui_pin_set = 0;
	/* Given back, the game's box is forgotten: the next time the panel opens it is read afresh. */
	if (to_game) data->menu_ui_game_clip_state = 0;
	if (data->menu_ui_have_pointer) {
		data->menu_ui_have_pointer = 0;
		nr_raw_mouse_stop();
	}
	data->menu_ui_push = 0.0f;
}

/* Note the box the game holds the cursor in, read while the panel's pin is not in force.
 *
 * Windows reports "not confined" as a box over the whole desktop, and that is also what the clip
 * guard and a change of focus leave behind - so the whole desktop is the game's answer only the
 * first time it is read, and says nothing about the game after that. Any smaller box is the game's,
 * and the latest one is kept: a game confines the cursor afresh when its window moves or resizes. */
static void game_clip_note(struct nr_panel *data, const RECT *held, POINT origin)
{
	int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	if (held->left <= vx && held->top <= vy &&
	    held->right >= vx + GetSystemMetrics(SM_CXVIRTUALSCREEN) &&
	    held->bottom >= vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)) {
		if (!data->menu_ui_game_clip_state) data->menu_ui_game_clip_state = 1;
		return;
	}
	data->menu_ui_game_clip[0] = held->left - origin.x;
	data->menu_ui_game_clip[1] = held->top - origin.y;
	data->menu_ui_game_clip[2] = held->right - origin.x;
	data->menu_ui_game_clip[3] = held->bottom - origin.y;
	data->menu_ui_game_clip_state = 2;
}

static void menu_ui_mouse(struct nr_panel *data, uint32_t width, uint32_t height,
                          const unsigned char *keys)
{
	/* Each guard reports itself once. Four silent returns and one trace line on the far side of
	 * them is exactly the shape of a bug that costs an evening: the log says "nothing arrived" and
	 * says nothing about which of the four doors was shut. */
	static int said[8];
#define UI_MOUSE_STOP(n, msg) do { if (!said[n]) { said[n] = 1; LAYER_TRACE("[mouse] stopped: " msg "\n"); } return; } while (0)

	if (!data->menu_ui) UI_MOUSE_STOP(0, "no UI");

	/* The gate. While another window is in front, the panel is fed NOTHING.
	 *
	 * This is the whole fix for "using any other window drives the menu": the cursor position and
	 * the button state below are system-wide, so without this test a drag in a browser moved a
	 * slider and a click in Explorer moved the panel. OptiScaler's `FeedImGui` does the same thing
	 * for the same reason - when the game is not focused it reports "no position", clears the
	 * modifiers and returns before touching any widget.
	 *
	 * The window is resolved first so the focus test has something to compare against. */
	/* The panel's ownership of the system cursor. The RELEASE is not done here: this function
	 * only runs on the frames the panel is open (`nr_panel_frame` is what calls it), so a release
	 * here would never run at the one moment it is needed. It lives in `menu_read_keys`, which
	 * runs every frame whether the panel is open or shut. */
	int panel_open = data->menu && nr_menu_is_open(data->menu);

	/* `nr_ui_window`, so the declaration serves both platforms and no call site casts. */
	nr_ui_window hwnd = data->window;
	if (!hwnd) UI_MOUSE_STOP(2, "no window for this process");
	if (!window_is_foreground(hwnd)) {
		/* "No position" rather than the last one: leaving a stale position would let a click that
		 * arrives on the very frame focus is regained land on whatever was under the pointer when
		 * the user left. The modifiers are cleared for the same reason - the user may have held
		 * Ctrl+Tab in the other window and released it there. */
		nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
		nrex_set_software_cursor(data->menu_ui, 0);
		/* The panel is still open but another window has the focus, so the pointer belongs to that
		 * window: give it back. The panel re-takes it on the first frame the game is in front
		 * again, and a cursor confined to a game nobody is looking at is the one thing that would
		 * be worse than the bug this is fixing. A game that stops presenting when it loses the
		 * focus never gets here, which is what `nr_clip_guard` covers. */
		menu_cursor_free(data, 0);
		data->menu_ui_outside = 0;
		return;
	}

	POINT screen = {0, 0};
	if (!GetCursorPos(&screen)) UI_MOUSE_STOP(1, "GetCursorPos failed");


	/* Where the game's client area is on screen, and how large. */
	POINT origin = {0, 0};
	if (!ClientToScreen(hwnd, &origin)) UI_MOUSE_STOP(3, "ClientToScreen failed");
	RECT client = {0, 0, 0, 0};
	GetClientRect(hwnd, &client);

	float cw = (float)(client.right - client.left);
	float ch = (float)(client.bottom - client.top);
	if (cw <= 0.0f || ch <= 0.0f) {
		if (!said[4]) {
			said[4] = 1;
			LAYER_TRACE("[mouse] stopped: client area is %.0fx%.0f\n", cw, ch);
		}
		return;
	}
	if (!said[5]) {
		said[5] = 1;
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		LAYER_TRACE("[mouse] window ok: hwnd=%p pid=%lu client=%.0fx%.0f "
				"origin=(%ld,%ld) cursor=(%ld,%ld)\n",
				(void *)hwnd, (unsigned long)pid, cw, ch,
				(long)origin.x, (long)origin.y, (long)screen.x, (long)screen.y);
	}
#undef UI_MOUSE_STOP

	/* The pointer went out past the picture's edge (below): the panel waits for the cursor to come
	 * back into the window, and takes it again where it comes in. */
	if (panel_open && data->menu_ui_outside) {
		if (screen.x < origin.x || screen.y < origin.y || screen.x >= origin.x + (LONG)cw ||
		    screen.y >= origin.y + (LONG)ch) {
			nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
			nrex_set_software_cursor(data->menu_ui, 0);
			return;
		}
		data->menu_ui_outside = 0;
	}

	/* Client pixels -> the picture's pixels. A game rendering at a lower resolution than its window
	 * is the common case, and this is what keeps the pointer aligned when it does. */
	float sx = (float)width / cw;
	float sy = (float)height / ch;

	/* The pointer, in the picture's own coordinates.
	 *
	 * While the panel is open the layer OWNS the system cursor: it confines it to the game's
	 * client area and writes its position every frame. That is the alternative to hooking, which
	 * this project does not do, and it removes the problem instead of competing over it.
	 *
	 * Why ownership is needed. A game in a relative-input mode calls `SetCursorPos` back to its
	 * centre every frame and aims with the movement. Two writers to one cursor is a race, and
	 * everything that went wrong before came from losing it:
	 *
	 *   - the OS pointer was dragged to the centre, so `GetCursorPos` answered "centre" forever
	 *     and the panel's arrow sat there while the hand moved;
	 *   - accumulating raw movement to compensate gave the layer a SECOND opinion about where the
	 *     pointer is - one that drifts from the game's, that keeps moving after the real pointer
	 *     has been stopped at the window edge, and that cannot be corrected without disagreeing
	 *     with itself. That is the "pinned but not at the centre" the report described: not the
	 *     game's centre, but a position the layer had invented and could not resynchronise;
	 *   - and the game's cursor and the panel's were in different places, so the one being
	 *     clicked was not the one being seen.
	 *
	 * Confining and then WRITING the position settles all three by removing the race rather than
	 * winning it per frame: `ClipCursor` means the position can never leave the picture, so there
	 * is no drift to accumulate and no edge to run past, and reading it back gives one source of
	 * truth - the OS one - instead of two.
	 *
	 * The game's own mouse-look is not silenced by this, and cannot be without hooking. What it
	 * gets is a pointer that only ever moves inside the window, which is the reachable part of
	 * "the panel has the mouse". (Its raw movement is the game's no longer: `nr_raw_mouse.c` holds
	 * the registration while the panel has the pointer, and `nr_input_guard.c` the messages.) */
	float x, y;
	if (panel_open) {
		/* The panel takes the pointer away from the game, and keeps it.
		 *
		 * The measured shape of the failure: the layer wrote the position every frame and the
		 * write took (`read == wrote == after`), yet a second later the position was back at the
		 * game's centre. The race was not lost per frame - it was lost BETWEEN frames. A game in
		 * a relative-input mode re-centres the cursor whenever it likes, and the centre is inside
		 * the client area, so a clip covering the client area permits it. Confirming a write is
		 * only proof that it landed, not that it survived.
		 *
		 * So the cursor is not raced for at all: it is pinned. `ClipCursor` to a ONE PIXEL box at
		 * the panel's pointer position means the OS clamps every attempted move - the game's to
		 * the centre, the hardware's from the mouse - straight back onto that pixel. The game
		 * cannot move it, so it stops trying to interpret it, and the pointer the player sees is
		 * the only one that moves.
		 *
		 * That pixel then has to travel with the pointer, which is what `menu_ui_pointer_x/y`
		 * does: it is the panel's own position, advanced by how far the MOUSE moved, which is
		 * independent of where the cursor is being held. Seeded once from the real cursor when
		 * the panel opens, so the pointer does not jump.
		 *
		 * With the panel closed none of this runs - the pin is lifted and the game gets its
		 * cursor back as it had it (see the release in `menu_read_keys`).
		 */
		if (!data->menu_ui_have_pointer) {
			/* Seed from the real pointer, once, so the panel's starts where the hand is, and
			 * start listening for raw movement before the first delta is asked for. */
			data->menu_ui_pointer_x = ((float)screen.x - (float)origin.x) * sx;
			data->menu_ui_pointer_y = ((float)screen.y - (float)origin.y) * sy;
			data->menu_ui_last_cursor_x = screen.x;
			data->menu_ui_last_cursor_y = screen.y;
			data->menu_ui_have_pointer = 1;
			nr_raw_mouse_start();
		}

		/* Advance by how far the MOUSE moved, read from raw input.
		 *
		 * `GetCursorPos` cannot answer this once the cursor is pinned: it returns the pin. Raw
		 * input still carries the movement, because it describes the device rather than the
		 * pointer, and it is the same stream the game aims with - so this is the one number that
		 * is right in both modes and needs no reconciliation with anything.
		 *
		 * `raw_gain` converts mouse counts to pixels. A mouse at 800 DPI reports about 800 counts
		 * per inch; 0.5 makes a full sweep of a 800-pixel-wide picture take about two inches of
		 * hand travel, which is the range a settings panel wants. */
		{
			int rdx = 0, rdy = 0;
			if (nr_raw_mouse_take(&rdx, &rdy)) {
				const float raw_gain = 0.5f;
				data->menu_ui_pointer_x += (float)rdx * raw_gain * sx;
				data->menu_ui_pointer_y += (float)rdy * raw_gain * sy;
			}
		}

		/* Pushed on past the picture's edge, the pointer goes out there, as a cursor leaves any
		 * window: the pin had held it in the window until the panel was closed, so the Start menu,
		 * another window or a screenshot's selection could not be reached (the owner, Mortal
		 * Kombat 11, 2026-10-31). A little push is taken as the edge, about 48 pixels as leaving.
		 * Not where the window fills the screen: there is no outside to put the cursor in. */
		{
			float w = (float)width - 1.0f, h = (float)height - 1.0f;
			float px = data->menu_ui_pointer_x, py = data->menu_ui_pointer_y;
			float over = (px < 0.0f ? -px : px > w ? px - w : 0.0f) + (py < 0.0f ? -py : py > h ? py - h : 0.0f);
			data->menu_ui_push = over > 0.0f ? data->menu_ui_push + over : 0.0f;
			if (data->menu_ui_push > 48.0f) {
				POINT out;
				out.x = origin.x + (LONG)((px < 0.0f ? 0.0f : px > w ? w : px) / sx);
				out.y = origin.y + (LONG)((py < 0.0f ? 0.0f : py > h ? h : py) / sy);
				if (px < 0.0f) out.x = origin.x - 8;
				else if (px > w) out.x = origin.x + (LONG)cw + 8;
				if (py < 0.0f) out.y = origin.y - 8;
				else if (py > h) out.y = origin.y + (LONG)ch + 8;
				if (MonitorFromPoint(out, MONITOR_DEFAULTTONULL)) {
					menu_cursor_free(data, 0);
					SetCursorPos(out.x, out.y);
					data->menu_ui_outside = 1;
					nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
					nrex_set_software_cursor(data->menu_ui, 0);
					return;
				}
				data->menu_ui_push = 0.0f;
			}
		}

		/* Clamp to the picture: outside it there is no panel to point at, and the pin would be
		 * somewhere the player cannot see. */
		if (data->menu_ui_pointer_x < 0.0f) data->menu_ui_pointer_x = 0.0f;
		if (data->menu_ui_pointer_y < 0.0f) data->menu_ui_pointer_y = 0.0f;
		if (data->menu_ui_pointer_x > (float)width - 1.0f)
			data->menu_ui_pointer_x = (float)width - 1.0f;
		if (data->menu_ui_pointer_y > (float)height - 1.0f)
			data->menu_ui_pointer_y = (float)height - 1.0f;

		/* Pin: hide the OS pointer (the panel draws its own) and hold the real one inside a
		 * one-pixel box at the panel's position. `ClipCursor` is in SCREEN coordinates. */
		if (!data->menu_ui_cursor_hidden) {
			while (ShowCursor(FALSE) >= 0) { /* push until it is off */ }
			data->menu_ui_cursor_hidden = 1;
		}
		{
			POINT pin;
			pin.x = origin.x + (sx > 0.0f ? (LONG)(data->menu_ui_pointer_x / sx) : 0);
			pin.y = origin.y + (sy > 0.0f ? (LONG)(data->menu_ui_pointer_y / sy) : 0);
			RECT box;
			box.left = pin.x;
			box.top = pin.y;
			box.right = pin.x + 1;
			box.bottom = pin.y + 1;
			RECT held;
			int read = GetClipCursor(&held);
			if (!read || held.left != box.left || held.top != box.top ||
			    held.right != box.right || held.bottom != box.bottom) {
				/* Not this frame's pin. Unless it is the one an earlier frame set, the box the
				 * cursor is in now is the game's own: noted, for closing to give back. */
				int ours = read && data->menu_ui_pin_set && held.left == data->menu_ui_pin_x &&
				           held.top == data->menu_ui_pin_y && held.right == held.left + 1 &&
				           held.bottom == held.top + 1;
				if (read && !ours) game_clip_note(data, &held, origin);
				ClipCursor(&box);
				data->menu_ui_pin_x = box.left;
				data->menu_ui_pin_y = box.top;
				data->menu_ui_pin_set = 1;
			}
			nr_clip_guard_start(hwnd);
			/* And put it there, so a re-centre that happened before the pin is undone now. */
			SetCursorPos(pin.x, pin.y);
		}

		x = data->menu_ui_pointer_x;
		y = data->menu_ui_pointer_y;
	} else {
		x = ((float)screen.x - (float)origin.x) * sx;
		y = ((float)screen.y - (float)origin.y) * sy;
	}
	data->menu_ui_last_cursor_x = screen.x;
	data->menu_ui_last_cursor_y = screen.y;

	int down = keys[0x01];
	nrex_mouse(data->menu_ui, x, y, down);

	/* And whether the menu has to draw the pointer itself.
	 *
	 * `GetCursorInfo`'s CURSOR_SHOWING is NOT the question, and asking it is what left the panel
	 * with no pointer in the game that reported this.
	 *
	 * It was the obvious test: the flag looks like "is the cursor on screen". It answers "is the
	 * cursor shown for THIS DESKTOP", and under Vulkan exclusive fullscreen it stays set while the
	 * pointer is nowhere to be seen - the compositor that would draw it has been taken out, and the
	 * game, which draws its own pointer during play, draws none while a menu takes the mouse. This
	 * was measured in the game: `flags = 1` from outside the process, at the pointer's real
	 * position, with no pointer visible in the picture. It is a known Vulkan-wide behaviour, not a
	 * quirk of this game (Dota 2 and Path of Exile report the same thing).
	 *
	 * So the panel draws its own pointer whenever it is the one that has the mouse - which is the
	 * whole of the time it is open, because the layer reports "no position" to ImGui otherwise.
	 * The game's cursor is not double-drawn by this: the game hides or draws its own while the
	 * panel has the pointer, and this is the arrow for the panel's own widgets.
	 *
	 * The hint goes with it, and it is not the same thing as the position above: that one is
	 * "no position" outside the image, so a widget is never hovered from off-panel. ImGui draws its
	 * cursor only when it HAS a position, so without the hint the arrow appeared only over the
	 * panel - the one place the user is not looking for it. */
	nrex_set_software_cursor(data->menu_ui, panel_open);
	if (panel_open)
		nrex_cursor_hint(data->menu_ui, x, y);
}
/* Hand the system cursor back, if the panel is holding it.
 *
 * Called from `menu_read_keys` because that is the one place that runs EVERY frame, open or shut.
 * It used to live in `menu_ui_mouse`, which only runs while the panel is drawn - that is, while the
 * panel is open - so the release never executed at the one moment it was needed. The cursor stayed
 * pinned in its one-pixel box after the panel closed and the player could not move the mouse at
 * all; the raw-input queue meanwhile kept accumulating, so reopening the panel replayed every
 * movement made while it was shut.
 *
 * `ClipCursor` and `ShowCursor` are PROCESS state, not per-window: leaking either one leaves a
 * cursor that cannot leave the game or cannot be seen, and the only way back is to end the
 * process. So this is driven by "is the panel open" alone, not by whether anything else
 * succeeded. What it gives back is the game's own hold, not none: see `menu_cursor_show`.
 */
static void menu_cursor_release(struct nr_panel *data)
{
	if (!data) return;
	if (data->menu && nr_menu_is_open(data->menu)) return;   /* still the panel's */
	data->menu_ui_outside = 0;
	menu_cursor_free(data, 1);
}
static int menu_read_keys(struct nr_panel *data, const unsigned char *keys)
{
	if (!data->menu) return 0;

	/* Before anything else: a closed panel must never leave the pointer captured. */
	menu_cursor_release(data);
	/* The pointer is the only thing left to clear here: the keyboard was swept once at the top
	 * of `nr_panel_input` and already handed to ImGui, so nothing is read again. */
	int foreground = window_is_foreground(data->window);
	if (data->menu_ui && !foreground)
		nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);

	/* L cycles the language. The combo in the panel's footer sets it too, and both go through
	 * `nr_menu_inject_language`, so the panel and the settings file cannot disagree - the
	 * injector raises the same flag the bridge already watches, which re-translates every row
	 * and rewrites the file. There is no second path that could translate differently.
	 *
	 * Edge-triggered here because this is a level poll: without the latch a held L would step the
	 * language once per presented frame and blur through all four in a fraction of a second.
	 *
	 * **Gated on the game's window being in front, and on the panel being open** (PR #20 review,
	 * item 2). An `L` typed in a chat window while the game sat behind it rewrote the settings
	 * file from another window - and a language write goes around "Save is the only thing that
	 * writes", which makes an unfocused L worse than an unfocused arrow. A language change is a
	 * settings action: it belongs behind the same two gates as S and R. The panel's own combo in
	 * the footer still cycles the language, so nothing is lost by gating the key. */
	if (data->menu && data->menu_ui && foreground && nr_menu_is_open(data->menu)) {
		int down = keys['L'];
		if (down && !data->menu_ui_l_was_down)
			nr_menu_inject_language(data->menu,
						(nr_menu_language(data->menu) + 1) % NR_MENU_LANG_COUNT);
		data->menu_ui_l_was_down = down;
	} else if (data->menu) {
		data->menu_ui_l_was_down = 0;   /* re-armed while gated, so the first press acts */
	}

	/* INS opens and closes the panel.
	 *
	 * `nr_menu.c`'s own reader does this for the hand-drawn menu, and the ImGui build switches that
	 * reader OFF so the two do not both act on one keypress - which left INS bound to nothing at
	 * all and the panel unreachable without `NR_MENU_OPEN`. The key is the panel's only way in, so
	 * the ImGui path has to own it here.
	 *
	 * Edge-triggered for the same reason as L: this is a level poll, and a held INS would toggle the
	 * panel once per presented frame.
	 *
	 * **Gated on the game's window being in front** (PR #20 review, item 2). Opening the panel is
	 * still what the user does when they return to the game: the first INS lands while the game is
	 * already in front, so the gate costs nothing - and it stops INS closing the panel, or opening
	 * and re-closing it, while the user is typing in another window. */
	if (data->menu_ui && foreground) {
		int down = keys[0x2D];
		if (down && !data->menu_ui_ins_was_down)
			nr_menu_inject_open(data->menu, !nr_menu_is_open(data->menu));
		data->menu_ui_ins_was_down = down;
	} else if (data->menu) {
		data->menu_ui_ins_was_down = 0;
	}

	/* Home, S and R, which the ImGui build has to read itself.
	 *
	 * INS and L are read at this level, so these join them rather than inventing a second
	 * mechanism - and they go through the menu's own flags, not straight to the effect:
	 * `nr_menu_take_*` is what the bridge polls, and routing the keys there keeps the key and the
	 * panel's button on one path. A button that saves and a key that does something else is the
	 * bug this project has already paid for twice.
	 *
	 * **What this comment used to claim, and why it was wrong:** it said all three lived inside
	 * `nr_menu_update`'s `m->open && m->read_keys` block. S and R had been moved OUT of that block
	 * on purpose (they were dead in the ImGui build while inside it), and Home was never in it. So
	 * in the ImGui build `nr_menu.c` was a SECOND reader of Home with no gate at all - which is why
	 * an unfocused Home toggled the effect even though the read right here was gated. The gate on
	 * `nr_menu.c`'s side is now `read_keys` itself, held off while the game is not in front; see
	 * the block above `nr_menu_bridge_update`.
	 *
	 * Home is the effect toggle, not END. END never arrived in the games that were tried:
	 * the key is bound by enough of them that the layer saw nothing. Home was the picture
	 * comparison, which is what this replaces, so the key is free and already means "change
	 * what I am looking at" - which is what turning the effect on and off does.
	 *
	 * Read only while the panel is open, for S and R, so a game with its own binding keeps
	 * it: the menu is a settings surface, and stealing a key from the game when the player is
	 * not looking at settings is the worse trade.
	 *
	 * **Gated on the game's window being in front** (PR #20 review, item 2): Home in an editor
	 * turned NR off in the game behind it. Home keeps its panel-closed exemption, because
	 * turning the effect off to get the frames back is something a player does mid-game - but
	 * only when the game is what they are looking at. S and R keep their panel-open rule, now
	 * with the focus gate on top of it. */
	if (data->menu_ui && foreground) {
		int toggle = keys[0x24];
		if (toggle && !data->menu_ui_toggle_was_down)
			nr_menu_inject_toggle(data->menu);
		data->menu_ui_toggle_was_down = toggle;
	} else if (data->menu) {
		data->menu_ui_toggle_was_down = 0;
	}

	if (data->menu_ui && foreground && nr_menu_is_open(data->menu)) {
		int save = keys['S'];
		if (save && !data->menu_ui_save_was_down)
			nr_menu_inject_save(data->menu);
		data->menu_ui_save_was_down = save;

		int reset = keys['R'];
		if (reset && !data->menu_ui_reset_was_down)
			nr_menu_inject_reset(data->menu);
		data->menu_ui_reset_was_down = reset;
	} else if (data->menu) {
		data->menu_ui_save_was_down = 0;
		data->menu_ui_reset_was_down = 0;
	}

	/* `nr_menu.c`'s OWN key reading, which the ImGui build does not replace.
	 *
	 * **What it read before this change**: `nr_menu_update` read Home with no gate at all and S and
	 * R whenever the panel was open - neither checked focus, and neither was inside the
	 * `read_keys` block the ImGui build turns off. So the layer's own gated Home at the top of
	 * this function did NOT protect it: in the ImGui build an unfocused Home was consumed by
	 * `nr_menu.c` and still acted. That is the "Home turned NR off while I was in another window"
	 * report. (The line used to be cited as `nr_menu.c:907`; it has moved, so it is no longer
	 * cited - a line number that points at the wrong code is worse than none.)
	 *
	 * **What it reads now**: all three are gated on `read_keys` in `nr_menu.c`, and this is what
	 * holds that switch. The fix is not a focus check inside `nr_menu.c` - that file has no window
	 * and no way to know which window is the game's, which is the whole reason `nr_focus.c`
	 * exists. It is the same switch the ImGui build already uses, held in the right position:
	 *
	 *   - the game is NOT in front  -> read_keys off, so `nr_menu.c` reads nothing at all
	 *   - the game IS in front, no ImGui UI -> read_keys on, which is the quad build's normal state
	 *
	 * Home therefore keeps working with the panel open AND closed in both builds - it only stops
	 * working when the user is not looking at the game, which is the defect. */
	if (data->menu) {
		if (!foreground) {
			nr_menu_set_key_reading(data->menu, 0);
			/* And the latch, or the first press after coming back is swallowed: `key_pressed`
			 * records "was down" even when the answer is discarded, so a Home held in another
			 * window would leave the menu believing it is still held. */
			nr_menu_release_keys(data->menu);
		} else if (!data->menu_ui) {
			nr_menu_set_key_reading(data->menu, 1);
		}
	}

	if (data->menu_bridge) {
		int state = nr_menu_bridge_update(data->menu_bridge, 1.0f / 60.0f);
		(void)state;
		if (state == 2)
			fprintf(stderr, "[nr_layer] settings menu %s\n",
				nr_menu_is_open(data->menu) ? "open" : "closed");
	} else {
		nr_menu_update(data->menu, 1.0f / 60.0f);
	}

	/* The toggle moves the trigger the host already reads every frame. The menu reports the press
	 * and this is what acts on it, because the trigger is the host's. */
	int live = trigger_state(data);
	if (nr_menu_take_toggle_request(data->menu) && live >= 0) {
		int on = !live;
		trigger_write(data, on);
		/* The saved choice follows the key, so a Save after pressing Home writes what the
		 * player just chose rather than what the panel last happened to show. */
		nr_menu_bridge_set_nr_saved(data->menu_bridge, on);
		live = on;
	}

	/* Tell the menu whether NR is on, so its status line is the truth rather than a guess. An
	 * installation with no trigger reads as off here, and as "unknown" in the panel's checkbox. */
	nr_menu_set_nr_enabled(data->menu, live > 0);

	/* And keep Save's memory of it honest.
	 *
	 * The trigger is the only truth about whether the effect is on - the Home key, the panel's
	 * checkbox and the setup window's button all move it - but `nr_enabled_saved` is what a later
	 * Save writes, and only the first two told it about their change. The setup window's Enable NR
	 * went through the trigger alone, so a Save afterwards wrote "nr_enabled": 0 with NR running:
	 * the file recording the opposite of the screen, and the next launch starting with the effect
	 * off for no reason the user could see.
	 *
	 * Comparing against the trigger every frame is what makes it right whichever way in was used,
	 * and it costs one look at a file the line above has already asked about.
	 *
	 * **It was unreachable from round seven to round ten:** a `return` above it, left when this
	 * moved out of nr_layer.c, so a Save with NR on wrote "nr_enabled": 0 again. */
	if (live >= 0 && data->menu_bridge) {
		if ((nr_menu_bridge_nr_saved(data->menu_bridge) ? 1 : 0) != live)
			nr_menu_bridge_set_nr_saved(data->menu_bridge, live);
	}

	return nr_menu_needs_composite(data->menu);
}

int nr_panel_input(nr_panel *data, void *window)
{
	if (!data) return 0;
	data->window_hint = (nr_ui_window)window;
	data->window = window_for_thread_for_ui(data);

	/* ---- the ONE place the keyboard is read ----
	 *
	 * Three files used to call `GetAsyncKeyState` on their own: the layer, `nr_menu.c` (which had
	 * the state machine) and `nr_menu_ui.cpp`. Each had to remember the focus gate separately, and
	 * two of them forgot it at least once - "an L typed in a chat window changed the panel's
	 * language", "Home in an editor turned NR off in the game behind it". That is the defect the
	 * maintainer's addition to item 1 was about, and the fix is not a gate in three places: it is
	 * one reader.
	 *
	 * So: sweep the 256 keys ONCE here, gate the sweep ONCE, and hand the same array to both
	 * consumers - the state machine through `nr_menu_set_key_levels`, the ImGui backend through
	 * `nrex_key`. Nothing below asks the OS for a key.
	 *
	 * A game that is not in front gets all-zeroes, which is what both consumers want: the state
	 * machine sees no edge, ImGui sees everything released. The hotkeys that must still work while
	 * the game is behind another window read this same array - they do not ask again. */
	unsigned char *keys = data->keys;
	{
		int foreground = window_is_foreground(data->window);
		if (foreground) {
			for (int vk = 0; vk < 256; ++vk)
				keys[vk] = nr_key_down(vk) ? 1 : 0;
		} else {
			memset(keys, 0, sizeof data->keys);
		}
		nr_menu_set_key_levels(data->menu, keys);
		if (data->menu_ui) {
			/* Events for ImGui only while the panel is open: it queues them until it draws. */
			nrex_listen(data->menu_ui, data->menu && nr_menu_is_open(data->menu));
			for (int vk = 0; vk < 256; ++vk)
				nrex_key(data->menu_ui, vk, keys[vk]);
		}
	}

	int composite = menu_read_keys(data, keys);

	/* While the panel is open, what it takes is the panel's alone (nr_input_guard.h): the game's
	 * window is kept from the keys, the clicks and the raw input it would otherwise act on too.
	 * After the keys, so the frame INS opens the panel on is already held. */
	nr_input_guard((void *)data->window, data->menu_ui && nr_menu_is_open(data->menu));
	return composite;
}

void nr_panel_frame(nr_panel *data, uint32_t width, uint32_t height)
{
	if (!data || !data->menu_ui) return;

	/* The panel's checkbox writes the same trigger the Home key does; the trigger is the host's. */
	int live = trigger_state(data);

	/* The ImGui path: build the frame, convert the bridge's rows, and let the host render it -
	 * into the UI's own image and onto the game's in Vulkan, over the back buffer in DirectX 12. */
	nrex_row rows[MENU_UI_MAX_ROWS];
	int count = menu_ui_rows(data, rows, MENU_UI_MAX_ROWS);

	/* The pointer, before NewFrame: ImGui consumes queued input when the frame opens. */
	menu_ui_mouse(data, width, height, data->keys);
	/* Seconds since THIS panel was last drawn, not a fixed step.
	 *
	 * ImGui's OWN keyboard repeat is timed on `io.DeltaTime`, and a fixed 1/60 here ties that
	 * repeat to the present rate: measured on the panel's own frames, a held Down moved the
	 * selection once per second at 240 frames and about ten times per second at 60 - so the
	 * selection's speed depends on how many frames a second the game draws, which is not what a
	 * keypress means. The panel's value repeat (`nrex_repeat`) is unaffected: it is already on a
	 * wall clock, so this changes only the half that was wrong.
	 *
	 * Clamped at a quarter second, so a load hitch or an alt-tab does not arrive as a multi-second
	 * delta that re-targets focus half the panel away. */
	float dt = 1.0f / 60.0f;
	{
		/* Held across frames, and left alone when the clock is unavailable so a machine
		 * without one falls back to the fixed step above rather than moving nothing. */
		double now = nr_now_seconds();
		if (data->menu_ui_last_draw != 0.0 && now > data->menu_ui_last_draw) {
			double elapsed = now - data->menu_ui_last_draw;
			if (elapsed < 0.000001) elapsed = 0.000001;
			if (elapsed > 0.25) elapsed = 0.25;
			dt = (float)elapsed;
		}
		data->menu_ui_last_draw = now;
	}
	nrex_new_frame(data->menu_ui, (float)width, (float)height, dt);
	/* The menu's CURRENT language, not the one this device started with.
	 *
	 * The language the bridge was created with is read once and never updated - so passing it
	 * here reset the panel's selector to the startup value on every frame, and the combo showed
	 * "English" forever however many times L was pressed or the list was used. The rows translated
	 * correctly the whole time, because they come from the bridge, which does track the change;
	 * only the selector was pinned.
	 *
	 * `nr_menu_language` is the same accessor the L key reads to step the value, so the two
	 * cannot disagree about which language is showing. */
	/* The panel-level hint - its own keys. It was not passed at all, so the ImGui panel (the one
	 * every game loads) showed no key list while the hand-drawn one did. It also said the game
	 * saw the keys too, which was true until `nr_input_guard.c` kept them from the game's window
	 * (2026-10-31). `NR_STR_HINT` is the same string the hand-drawn path sets, so the two cannot
	 * drift. */
	nrex_begin_panel(data->menu_ui, "DLSS-NR on Intel", nr_menu_language(data->menu),
			 nr_menu_text(nr_menu_language(data->menu), NR_STR_HINT));
	/* The effect, as the host sees it. -1 when this install has no trigger, so the checkbox can
	 * show "unknown" instead of claiming the effect is off. */
	nrex_set_nr_enabled(data->menu_ui, live);
	nrex_rows(data->menu_ui, rows, count, nr_menu_selected(data->menu));
	int acted = nrex_end_panel(data->menu_ui);

	/* Apply what the user moved. The UI reports intent; the bridge owns the knob's range, its
	 * step, the settings write and the daemon's notification - so an edit goes through the same
	 * path an arrow key takes, and the two cannot disagree about what a knob accepts. */
	for (int i = 0; i < count; ++i) {
		if (!rows[i].changed) continue;
		float v = (rows[i].kind == NREX_KIND_FLOAT) ? rows[i].value_f
							    : (float)rows[i].value_i;
		/* The pacing rows and the network come after the knobs and are not knobs: their values
		 * live in the proxy's environment file, not the settings file, so they go to the bridge's
		 * own entry point. Each such row said what it is when it was built (`row_what`). */
		if (data->row_what[i] >= 0) {
			int what = data->row_what[i];
			int value = (int)v;
			if (what == NR_PACING_EVERY) value += 1;
			nr_menu_bridge_set_pacing(data->menu_bridge, what, value);
			continue;
		}
		nr_menu_bridge_set(data->menu_bridge, i, v);
	}
	/* The panel's language selector, reported the same way L is: through the menu's own flag,
	 * which the bridge polls on its next update. One path, so the two cannot translate
	 * differently. */
	if (acted & NREX_ACT_LANGUAGE)
		nr_menu_inject_language(data->menu, nrex_panel_language(data->menu_ui));
	if (acted & NREX_ACT_SAVE)  nr_menu_bridge_save(data->menu_bridge);
	/* Reset is the menu's own flag: nr_menu_take_reset is what the bridge polls, and setting
	 * it here puts the button through the same path the R key takes. */
	if (acted & NREX_ACT_RESET) nr_menu_inject_reset(data->menu);
	/* Revert: back to the SAVED settings. Through the menu's flag, like Reset, so the
	 * button and any future key share one path. */
	if (acted & NREX_ACT_REVERT) nr_menu_inject_revert(data->menu);
	/* The effect checkbox. It moves the same trigger the Home key and the setup window
	 * move - one place, so the three cannot disagree - and records the choice for Save. */
	if (acted & NREX_ACT_NR_TOGGLE) {
		int want = nrex_nr_toggle_request(data->menu_ui) > 0;
		nr_menu_bridge_set_nr_saved(data->menu_bridge, want);
		if (live >= 0) trigger_write(data, want);
	}
	/* Close, through the same path INS uses.
	 *
	 * The button reported this action and nothing consumed it, so pressing Close did nothing at
	 * all - the flag was set, `acted` carried it, and no branch looked at it. Wiring it to the
	 * menu's own open flag rather than to a path of its own is what makes the button and the
	 * key agree about what closing means.
	 *
	 * Closing KEEPS the unsaved edits; see the close handling in `nr_menu_bridge_update`. */
	if (acted & NREX_ACT_CLOSE) nr_menu_inject_open(data->menu, 0);
}

void nr_panel_destroy(nr_panel *data)
{
	if (!data) return;
	/* A panel open when its host goes must not leave the cursor held, nor the guard's thread
	 * running in a library about to be unloaded, nor the game's input held. */
	menu_cursor_free(data, 0);
	nr_input_guard((void *)data->window, 0);
	/* The bridge goes FIRST: it is what the panel reads its rows through, and it is also what
	 * removes the live preview - the daemon's view of what the user is trying RIGHT NOW. Leaving
	 * that behind made a Save look like it had not happened. */
	if (data->menu_bridge) {
		nr_menu_bridge_destroy(data->menu_bridge);
		data->menu_bridge = NULL;
	}
	free(data);
}
