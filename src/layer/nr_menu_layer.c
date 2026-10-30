/* The in-game menu's glue: the composite pass, the key reading, the rows, the mouse.
 *
 * **Windows only, and built only on Windows.** See `nr_menu_layer.h` for why the split exists.
 * Everything here used to live in `nr_layer.c`; it moved so that a Linux build compiles none of
 * the menu, which is what the maintainer asked for - Linux's layer should be `newnr`'s layer, with
 * no ImGui, no `libstdc++` and no STORAGE on a swapchain.
 *
 * What stayed in `nr_layer.c` is the layer: capture, the transport, the trigger file, and the
 * three hooks below. What is here is everything that needs a device, a swapchain and a window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

/* ccess's mode argument. Windows spells it in <io.h> as _access and has no F_OK; 
r_layer.c
 * defines the same three for the same reason, and this file needs F_OK because it asks whether the
 * trigger file exists. */
#ifndef F_OK
#define F_OK 0
#endif
#ifdef _WIN32
#  include <io.h>
/* `access` and `_access` are the same function on MSVC, but only the underscore one is declared by
 * <io.h>; without this the call is an implicit declaration and the link finds a CRT symbol of that
 * name by luck. The layer's own translation unit spells this out for the same reason. */
#  define access(p, m) _access(p, m)
#endif

/* First, because it sets the Vulkan platform define that 
r_gpu_bridge.h needs and includes
 * Vulkan itself. Anything that includes Vulkan before this is a compile error on Win32. */
#include "nr_layer_state.h"

#include "nr_transport.h"
#include "nr_layer_stats.h"
#include "nr_surface_policy.h"
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"
#include "nr_menu_strings.h"
#include "nr_menu_font.h"
#include "nr_menu_layer.h"
#include "nr_raw_mouse.h"
#include "nr_focus.h"
#include "nr_layer_cadence.h"

/* ------------------------------------------------------------------ shared with nr_layer.c
 *
 * These are the four things the glue needed from the file it used to live in. Two are macros the
 * state header now carries; two are accessors, because exporting a `VkInstance` as a global is how
 * two translation units end up disagreeing about which instance is current.
 */
void nr_layer_menu_globals(VkInstance *instance, PFN_vkGetInstanceProcAddr *next_proc);
struct swapchain_data *nr_layer_find_swapchain(VkSwapchainKHR swapchain);
void nr_layer_set_trigger_file(const char *path, int on);

/* Where the settings file is. The same resolution the daemon spawn uses, in one place so the menu
 * and the spawn cannot disagree about which file they mean - a menu writing a file the daemon is
 * not reading would be silent, and it is the kind of split this project has already paid for.
 *
 * `NR_SETTINGS` first, then the platform default directory, which is what `nr_paths.SETTINGS`
 * itself defaults to: `tempfile.gettempdir()` on Windows. */
static void menu_settings_path(char *out, size_t size)
{
	const char *explicit = getenv("NR_SETTINGS");
	if (explicit && *explicit) {
		snprintf(out, size, "%s", explicit);
		return;
	}
	char dir[1024];
	nr_default_dir(dir, sizeof dir);
	snprintf(out, size, "%s/nr_settings.json", dir);
}
/* The menu's resolver for instance-level functions.
 *
 * `next_instance_proc` is `vkGetInstanceProcAddr`, whose first argument is the INSTANCE. The menu
 * has no instance - it is handed a device and a physical device and nothing else - so passing that
 * function straight through meant the menu called it with a `VkPhysicalDevice` where a `VkInstance`
 * belongs. Both are opaque 64-bit handles, so it compiled and then faulted inside the loader.
 *
 * This wrapper ignores whatever handle the menu passes and uses the layer's own instance, which is
 * the one `next_instance_proc` was resolved against in the first place. The menu's contract stays
 * "call the resolver with the handle you were given" and the layer decides what that means. */
static void *NR_MENU_CALL menu_instance_resolver(void *handle, const char *name)
{
	(void)handle;
	VkInstance instance = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr next_proc = NULL;
	nr_layer_menu_globals(&instance, &next_proc);
	if (!next_proc) return NULL;
	return (void *)next_proc(instance, name);
}
/* The menu is opt-out: NR_MENU=0 for a game where even one GetAsyncKeyState a frame is unwelcome,
 * and for anyone bisecting a problem they suspect is ours. Read once. */
static int menu_enabled(void)
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
static char *menu_ui_font_bytes(size_t *size_out)
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
static int menu_ui_collect_codepoints(int *out, int max)
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
static void menu_build(struct device_data *data, struct swapchain_data *chain)
{
	if (data->menu_state) return;
	data->menu_state = -1;

	/* The surface's own answer, from when the swapchain was created. Without STORAGE the images
	 * cannot be written by a compute pass, and asking anyway is a device loss. */
	if (!chain->storage_ok) {
		data->menu_reason = "this surface does not support storage images";
		fprintf(stderr, "[nr_layer] no settings menu: %s\n", data->menu_reason);
		return;
	}
	/* **And a device that cannot carry the panel gets none.** The panel's shader takes both of its
	 * buffers by device address and is SPIR-V 1.6, so the device must have bufferDeviceAddress
	 * enabled and run Vulkan 1.3. A Vulkan 1.0 title - DOOM (2016) among them - has neither, and
	 * building the panel anyway is invalid usage: validation reported it on vkcube before a frame
	 * was drawn. What the GPU supports is not the question, so vkCreateDevice recorded what the
	 * game enabled (`menu_device_reason` in nr_layer.c). */
	if (data->menu_device_reason) {
		data->menu_reason = data->menu_device_reason;
		fprintf(stderr, "[nr_layer] no settings menu: %s\n", data->menu_reason);
		return;
	}

	/* Two resolvers, because Vulkan splits them: the physical-device queries come through the
	 * instance, everything else through the device. The layer has both already, and passing them
	 * keeps the menu from linking a loader of its own. */
	data->menu = nr_menu_create(data->device, data->physical, data->queue_family,
				    (uint32_t)chain->format,
				    (nr_menu_resolve_fn)data->get_device_proc,
				    (nr_menu_resolve_fn)menu_instance_resolver,
				    &data->menu_reason);
	if (!data->menu) {
		fprintf(stderr, "[nr_layer] no settings menu: %s\n",
			data->menu_reason ? data->menu_reason : "unknown");
		return;
	}

	data->menu_state = 1;

	/* The ImGui UI. Created here because this is the first moment the device, the queue family and
	 * the swapchain's image counts are all in hand; it borrows the layer's device and owns its own
	 * images. A failure leaves `menu_ui` NULL and the quad path serves the frame instead, which is
	 * the same "the menu is a feature, not a dependency" rule the rest of this file follows. */
	{
		const char *ui_why = NULL;
		size_t font_size = 0;
		char *font = menu_ui_font_bytes(&font_size);
		static int codepoints[NR_FONT_MAX_CODEPOINTS];
		int codepoint_count = menu_ui_collect_codepoints(codepoints, NR_FONT_MAX_CODEPOINTS);
		uint32_t ui_family_out = data->queue_family;
		/* One queue of the presenting family, for the backend's own submissions. Leaving this
		 * NULL makes the backend resolve a queue itself and wait forever on the font upload:
		 * the game hung inside its present exactly there. */
		VkQueue ui_queue = VK_NULL_HANDLE;
		/* A queue from a family the device was created with, OTHER than the presenting family when
		 * there is one. The presenting family is the one this layer binds its semaphore contract
		 * to - a submission that lands there can sit forever behind a wait the layer has already
		 * consumed - and the device hands out one queue per family, which is what 'families {0,1}'
		 * in the log means. The copy family has no such contract, so the backend's font upload goes
		 * there, and the backend is told that family too, so its command pool matches the queue it
		 * submits to. When the device has only one family there is no such choice, and the
		 * presenting one is used rather than a number that was never created. */
		uint32_t ui_family = data->ui_queue_family;
		data->get_device_queue(data->device, ui_family, 0, &ui_queue);
		ui_family_out = ui_family;
		VkInstance ui_instance = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr ui_next = NULL;
	nr_layer_menu_globals(&ui_instance, &ui_next);
	data->menu_ui = nrex_create(data->device, data->physical, (void *)ui_instance,
					    (void *)ui_queue, ui_family_out,
					    (uint32_t)chain->format, 420u << 12,
					    chain_image_count(chain), chain_image_count(chain),
					    (void *)(uintptr_t)data->get_device_proc,
					    (void *)(uintptr_t)ui_next,
					    font, font_size, codepoints, codepoint_count, &ui_why);
		/* NOT freed here. AddFontFromMemoryTTF adopts the buffer (FontData with
		 * FontDataOwnedByAtlas left true) and frees it when the atlas is destroyed - freeing it
		 * on this side too was a double free, which is the access violation this menu hit the
		 * moment a font was supplied. Ownership transferred, so the pointer is not touched again;
		 * if the UI never asked for it, the buffer is freed here instead. */
		if (!data->menu_ui) free(font);
		/* next_instance_proc needs the instance; `nrex_create` was given it beside the resolver
		 * above, so the two together reach the instance-level commands. */
		if (data->menu_ui) {
			/* ImGui owns the panel now, so this file stops reading the panel's keys. Leaving both
			 * readers on is what made one keypress act twice - Enter opening a combo while the
			 * bridge stepped the value, INS toggling the panel twice per press. */
			nr_menu_set_key_reading(data->menu, 0);
		} else {
			fprintf(stderr, "[nr_layer] no ImGui UI (%s); the quad menu serves instead\n",
				ui_why ? ui_why : "unknown");
		}
	}

	/* A documented test hook: open the panel as soon as it exists, so the draw path can be
	 * exercised without a keyboard. Off unless asked for. */
	{
		const char *forced = getenv("NR_MENU_OPEN");
		if (forced && *forced && strcmp(forced, "0") != 0)
			nr_menu_inject_open(data->menu, 1);
	}

	char settings[1088];
	menu_settings_path(settings, sizeof settings);
	data->menu_bridge = nr_menu_bridge_create(data->menu, settings, data->menu_language);
	if (data->menu_bridge)
		fprintf(stderr, "[nr_layer] settings menu ready; INS opens it (%s)\n", settings);
	else
		fprintf(stderr, "[nr_layer] settings menu ready; INS opens it\n");
}
static int menu_ui_rows(struct device_data *data, nrex_row *out, int capacity)
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
	 * The GPU image path is DISABLED, not hidden, while the network runs ahead of the game or the
	 * cadence is not every frame: those two are exactly what the path requires, so leaving it live
	 * would let the user set something the layer then silently ignores. Pressing the control is
	 * also what clears the conflict - the bridge applies it - so the row stays clickable when it
	 * is enabled and explains itself when it is not. */
	if (nr_menu_bridge_pacing_available(data->menu_bridge)) {
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
				int n;
				if (c == 0 && !strstr(nr_menu_text(language, NR_STR_EVERY_FRAME), "{}")) {
					n = snprintf(every_names_store[c], sizeof every_names_store[c], "%s",
					             nr_menu_text(language, NR_STR_EVERY_FRAME));
				} else if (strstr(shape, "{}")) {
					const char *brace = strstr(shape, "{}");
					int head = (int)(brace - shape);
					n = snprintf(every_names_store[c], sizeof every_names_store[c],
					             "%.*s%d%s", head, shape, c + 1, brace + 2);
				} else {
					n = snprintf(every_names_store[c], sizeof every_names_store[c],
					             "%s %d", shape, c + 1);
				}
				if (n < 0 || n >= (int)sizeof every_names_store[c])
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
			n++;
		}
	}

	return n;
}
#define window_is_foreground(hwnd) nr_window_is_foreground((void *)(hwnd))

/* The game's window, for the pointer conversion.
 *
 * The layer has no window and no message pump - it lives inside the game's process - so the handle
 * comes from the GUI thread's active window, which is the one the user is looking at. */
/* Our own process's window: the process, not the thread.
 *
 * `GetWindowThreadProcessId(h, NULL)` returns a THREAD id and it was being compared against
 * `GetCurrentThreadId()`. Those agree only when the window was created by the calling thread - and
 * this layer's present runs on the game's rendering thread while the window belongs to whichever
 * thread the game created it on. Almost every game has those be different, so every window was
 * rejected and the panel never received a pointer position. The question the check means to ask is
 * "is this our process's window", which is a process comparison. */
#define window_is_ours(h) nr_window_is_ours((void *)(h))

#ifdef _WIN32
/* The window handle this file's Win32 calls want. A typedef rather than `HWND` spelled out, so
 * the declaration below reads the same on both platforms and no call site casts. */
typedef HWND nr_ui_window;
#else
typedef void *nr_ui_window;
#endif

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

static nr_ui_window window_for_thread_for_ui(void)
{
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
/* No window of ours to find, so no pointer to convert.
 *
 * This is not "the panel loses its mouse" so much as "there is no panel on this platform": the
 * ImGui backend (`nr_menu_ui.cpp`) compiles to a stub off Windows, `data->menu_ui` is NULL, and
 * `menu_ui_mouse` returns at its first guard before it ever asks for a window. The two callers
 * that survive are the quad menu's own cursor, which has no window either.
 *
 * Returning NULL rather than a fake window matters: the caller's `if (!hwnd)` reports "no window
 * for this process" once and returns, which is the honest thing to say. A fabricated handle would
 * make it convert screen coordinates against a window that does not exist. */
static nr_ui_window window_for_thread_for_ui(void)
{
	return NULL;
}
#endif

static void menu_ui_mouse(struct device_data *data, struct swapchain_data *chain,
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
	 * only runs on the frames the panel is open (`menu_composite` is what calls it), so a release
	 * here would never run at the one moment it is needed. It lives in `menu_read_keys`, which
	 * runs every frame whether the panel is open or shut. */
	int panel_open = data->menu && nr_menu_is_open(data->menu);

	/* `nr_ui_window`, so the declaration serves both platforms and no call site casts. */
	nr_ui_window hwnd = window_for_thread_for_ui();
	if (!hwnd) UI_MOUSE_STOP(2, "no window for this process");
	if (!window_is_foreground(hwnd)) {
		/* "No position" rather than the last one: leaving a stale position would let a click that
		 * arrives on the very frame focus is regained land on whatever was under the pointer when
		 * the user left. The modifiers are cleared for the same reason - the user may have held
		 * Ctrl+Tab in the other window and released it there. */
		nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
		nrex_set_software_cursor(data->menu_ui, 0);
		/* The panel is still open but another window has the focus, so the pointer belongs to that
		 * window: give it back. The layer re-takes it on the first frame the game is in front
		 * again, and a cursor confined to a game nobody is looking at is the one thing that would
		 * be worse than the bug this is fixing. */
		if (data->menu_ui_cursor_hidden) {
			ShowCursor(TRUE);
			data->menu_ui_cursor_hidden = 0;
			ClipCursor(NULL);
			data->menu_ui_have_pointer = 0;
			nr_raw_mouse_stop();
		}
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

	/* Client pixels -> swapchain pixels. A game rendering at a lower resolution than its window is
	 * the common case, and this is what keeps the pointer aligned when it does. */
	float sx = (float)chain->extent.width / cw;
	float sy = (float)chain->extent.height / ch;

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
	 * "the panel has the mouse". */
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
		 * With the panel closed none of this runs - the clip is released and the game gets its
		 * cursor back (see the release at the top of this function).
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

		/* Clamp to the picture: outside it there is no panel to point at, and the pin would be
		 * somewhere the player cannot see. */
		if (data->menu_ui_pointer_x < 0.0f) data->menu_ui_pointer_x = 0.0f;
		if (data->menu_ui_pointer_y < 0.0f) data->menu_ui_pointer_y = 0.0f;
		if (data->menu_ui_pointer_x > (float)chain->extent.width - 1.0f)
			data->menu_ui_pointer_x = (float)chain->extent.width - 1.0f;
		if (data->menu_ui_pointer_y > (float)chain->extent.height - 1.0f)
			data->menu_ui_pointer_y = (float)chain->extent.height - 1.0f;

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
			if (!GetClipCursor(&held) ||
			    held.left != box.left || held.top != box.top ||
			    held.right != box.right || held.bottom != box.bottom) {
				ClipCursor(&box);
			}
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
 * It used to live in `menu_ui_mouse`, which only runs while `menu_composite` does - that is,
 * while the panel is open - so the release never executed at the one moment it was needed. The
 * cursor stayed pinned in its one-pixel box after the panel closed and the player could not move
 * the mouse at all; the raw-input queue meanwhile kept accumulating, so reopening the panel
 * replayed every movement made while it was shut.
 *
 * `ClipCursor` and `ShowCursor` are PROCESS state, not per-window: leaking either one leaves a
 * cursor that cannot leave the game or cannot be seen, and the only way back is to end the
 * process. So this is driven by "is the panel open" alone, not by whether anything else
 * succeeded.
 */
static void menu_cursor_release(struct device_data *data)
{
	if (!data) return;
	if (data->menu && nr_menu_is_open(data->menu)) return;   /* still the panel's */
	if (!data->menu_ui_cursor_hidden) return;
#ifdef _WIN32
	ShowCursor(TRUE);
	ClipCursor(NULL);
#endif
	data->menu_ui_cursor_hidden = 0;
	data->menu_ui_have_pointer = 0;
	nr_raw_mouse_stop();
}
static int menu_read_keys(struct device_data *data, const unsigned char *keys)
{
	if (!data->menu) return 0;

	/* The trigger file is the layer's own, resolved once at start-up. Fetched here rather than
	 * reached for at each use: it moved out of this file when the menu's glue did, and a pointer
	 * to a static in another translation unit is exactly the sort of thing that reads fine and
	 * links wrong. */
	const char *trigger_path = nr_layer_trigger_path();

	/* Before anything else: a closed panel must never leave the pointer captured. */
	menu_cursor_release(data);
	/* The pointer is the only thing left to clear here: the keyboard was swept once at the top
	 * of `nr_menu_layer_present` and already handed to ImGui, so nothing is read again. */
	int foreground = window_is_foreground(window_for_thread_for_ui());
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

	/* The toggle moves the trigger the layer already reads every frame. The menu reports the
	 * press and this is what acts on it, because the trigger is the layer's. */
	if (nr_menu_take_toggle_request(data->menu) && trigger_path && *trigger_path) {
		int on = !(access(trigger_path, F_OK) == 0);
		nr_layer_set_trigger_file(trigger_path, on);
		/* The saved choice follows the key, so a Save after pressing Home writes what the
		 * player just chose rather than what the panel last happened to show. */
		nr_menu_bridge_set_nr_saved(data->menu_bridge, on);
	}

	/* Tell the menu whether NR is on, so its status line is the truth rather than a guess.
	 * **`*trigger_path` as well as `trigger_path`.** NR_LAYER_TRIGGER can be the empty string,
	 * which is not NULL, and _access("") on Windows is not a defined failure. */
	nr_menu_set_nr_enabled(data->menu,
			       trigger_path && *trigger_path && access(trigger_path, F_OK) == 0);

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
	 * and it costs one `access` on a file the line above has already asked about.
	 *
	 * **It was unreachable from round seven to round ten:** a `return` above it, left when this
	 * moved out of nr_layer.c, so a Save with NR on wrote "nr_enabled": 0 again. */
	if (trigger_path && *trigger_path && data->menu_bridge) {
		int live = access(trigger_path, F_OK) == 0;
		if ((nr_menu_bridge_nr_saved(data->menu_bridge) ? 1 : 0) != live)
			nr_menu_bridge_set_nr_saved(data->menu_bridge, live);
	}

	return nr_menu_needs_composite(data->menu);
}
/* The menu's own command buffer, its fence, and one submit. See the note above on why it owns no
 * semaphores.
 *
 * Returns 1 when the frame was drawn, 0 when there was nothing to record or the recording failed -
 * in which case the frame presents without a menu, which is a missing menu rather than a lost
 * game. */
static int menu_composite(struct device_data *data, struct swapchain_data *chain, VkQueue queue,
                          uint32_t index, const unsigned char *keys)
{
	if (!data->menu || data->device_error || data->transfer_error) return 0;
	if (index >= chain->image_count) return 0;
	if (!chain->storage_ok) return 0;

	/* The panel's checkbox writes the same trigger the Home key does; the path is the layer's. */
	const char *trigger_path = nr_layer_trigger_path();

/* "may be set and not read" - GCC and Clang take the attribute, MSVC has no such thing and treats
 * an initialised local as fine by default, so the macro is empty there. */
#if defined(__GNUC__) || defined(__clang__)
#  define NR_MAYBE_UNUSED __attribute__((unused))
#else
#  define NR_MAYBE_UNUSED
#endif

/* Resolve one device entry point for this function's use.
 *
 * `NR_MAYBE_UNUSED` is not decoration: `FreeCommandBuffers` is in the list because the menu's own
 * pool is freed at teardown, which is not in this function - so the variable is set and never read
 * here. GCC's `-Wunused-but-set-variable` calls that a warning and the Linux build had 20 of them
 * where `newnr` had none. Marking the whole family is what keeps the list honest: adding an entry
 * for a call that turns out to live elsewhere does not cost a warning, and removing one does not
 * cost a build. */
#define FN(type, name) type name NR_MAYBE_UNUSED = \
	(type)data->get_device_proc(data->device, "vk" #name)
	FN(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers);
	FN(PFN_vkResetCommandBuffer, ResetCommandBuffer);
	FN(PFN_vkBeginCommandBuffer, BeginCommandBuffer);
	FN(PFN_vkEndCommandBuffer, EndCommandBuffer);
	FN(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier);
	FN(PFN_vkQueueSubmit, QueueSubmit);
	FN(PFN_vkCreateFence, CreateFence);
	FN(PFN_vkResetFences, ResetFences);
	FN(PFN_vkWaitForFences, WaitForFences);
	FN(PFN_vkFreeCommandBuffers, FreeCommandBuffers);
#undef FN

	/* Allocated once and reused, like `transfer_commands`: an allocation a frame is an allocation
	 * on the present path. The fence is what makes reuse safe - the wait below completes the
	 * previous frame's menu before this one resets its buffer. */
	/* A pool of the menu's own, not `data->pool`.
	 *
	 * `data->pool` belongs to the capture path: `transfer()` creates it lazily, and `transfer()`
	 * only runs while the layer is capturing - which needs the trigger to exist. The menu has
	 * nothing to do with capture, so on a game where NR is idle the pool is still VK_NULL_HANDLE
	 * when the menu first draws, and `vkAllocateCommandBuffers` with a null pool faults inside the
	 * loader. That was this menu's crash: an access violation reading 0x70, which is a field of
	 * the dispatch table behind a NULL handle.
	 *
	 * One pool for one command buffer, created on first use and destroyed with the rest. */
#define FN2(type, name) type name = (type)data->get_device_proc(data->device, "vk" #name)
	FN2(PFN_vkCreateCommandPool, CreateCommandPool);
	FN2(PFN_vkDestroyCommandPool, DestroyCommandPool);
#undef FN2
	if (!CreateCommandPool) return 0;

	/* One pool per queue family, rebuilt when the family changes.
	 *
	 * A command pool belongs to a family for its whole life and its command buffers may only be
	 * submitted to a queue of that family. The layer reassigns `data->queue_family` at present time
	 * when the presenting queue is a different one, and frees the CAPTURE pool there
	 * (`release_transfer_resources`) for exactly this reason - the menu's pool was not included, so
	 * it went stale. A submit from a stale pool is accepted by the loader and does nothing, which is
	 * what the readback found: no pixel of the panel anywhere in the image, no error, no crash. */
	if (data->menu_pool && data->menu_pool_family != data->queue_family) {
		/* The command buffer came from this pool; the fence has already been waited, so both are
		 * finished with. */
		if (data->menu_commands && DestroyCommandPool) {
			data->menu_commands = VK_NULL_HANDLE;
		}
		if (DestroyCommandPool) DestroyCommandPool(data->device, data->menu_pool, NULL);
		data->menu_pool = VK_NULL_HANDLE;
	}
	if (!data->menu_pool) {
		/* **`RESET_COMMAND_BUFFER` as well as `TRANSIENT`, and the first is not optional.**
		 * Every frame the menu draws begins with `vkResetCommandBuffer` on this pool's one buffer,
		 * and resetting a buffer from a pool without this flag is invalid usage - the maintainer's
		 * validation run on vkcube reported it, and under validation vkcube then exited
		 * 0xC0000409. `TRANSIENT` stays because the buffer is genuinely short-lived; the two flags
		 * answer different questions. */
		VkCommandPoolCreateInfo pci = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
				 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = data->queue_family };
		if (CreateCommandPool(data->device, &pci, NULL, &data->menu_pool) != VK_SUCCESS)
			return 0;
		data->menu_pool_family = data->queue_family;
	}
	if (!data->menu_commands) {
		VkCommandBufferAllocateInfo alloc = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = data->menu_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1 };
		if (AllocateCommandBuffers(data->device, &alloc, &data->menu_commands) != VK_SUCCESS)
			return 0;
		if (data->set_loader_data &&
		    data->set_loader_data(data->device, data->menu_commands) != VK_SUCCESS) {
			data->menu_commands = VK_NULL_HANDLE;
			return 0;
		}
	}
	if (!data->menu_fence) {
		VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if (CreateFence(data->device, &fi, NULL, &data->menu_fence) != VK_SUCCESS) return 0;
	} else if (ResetFences(data->device, 1, &data->menu_fence) != VK_SUCCESS) {
		data->device_error = VK_ERROR_DEVICE_LOST;
		return 0;
	}
	if (ResetCommandBuffer(data->menu_commands, 0) != VK_SUCCESS) return 0;

	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	if (BeginCommandBuffer(data->menu_commands, &bi) != VK_SUCCESS) return 0;

	/* The image is the game's, and the presentation engine may still be reading the previous
	 * frame from it. Up to GENERAL for the stores, and back to PRESENT_SRC_KHR at the end - the
	 * layout the game left it in, so a frame with a menu on it presents like any other. */
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = chain->images[index],
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CmdPipelineBarrier(data->menu_commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (data->menu_ui) {
		/* The ImGui path: build the frame, convert the bridge's rows, render into the UI's own
		 * image, then blend that image onto the game's - which is the step that was missing when
		 * the backend reported itself started and the screen stayed clean. */
		nrex_row rows[MENU_UI_MAX_ROWS];
		int count = menu_ui_rows(data, rows, MENU_UI_MAX_ROWS);

		/* The UI image follows the swapchain.
		 *
		 * It used to be made once at 640x360 and never touched again, while `nrex_new_frame` below
		 * was told the swapchain's CURRENT extent every frame. After a resolution change the two
		 * disagreed: ImGui laid the panel out for the new size and drew it into an image of the old
		 * one, so the panel was clipped to the old rectangle - the part past 640x360 simply was not
		 * there, and the blend sampled an image the shader was told was a different size.
		 *
		 * Checked every frame because it is one comparison, and because there is no resize EVENT
		 * here to hang it on: a Vulkan layer learns the extent from the swapchain it is presenting,
		 * and `chain->extent` is updated by the layer's own `vkCreateSwapchainKHR` hook. A game can
		 * also change the extent without recreating anything (a borderless window drag), which is
		 * the case a "recreate on swapchain creation" hook would miss. */
		if (nrex_image_width(data->menu_ui) != chain->extent.width ||
		    nrex_image_height(data->menu_ui) != chain->extent.height) {
			/* The previous frame has to be finished before the image is destroyed.
			 *
			 * `nrex_resize` frees the UI image and makes a new one, and the frame before this one
			 * was still reading it: ImGui draws into it and the blend pass samples it, and both are
			 * queued behind `menu_fence` until the wait at the end of this function. Destroying an
			 * image the GPU is still reading is undefined - it shows up as a corrupt panel, a
			 * device loss, or nothing at all, depending on the driver.
			 *
			 * `WaitForFences` on the menu's own fence is the completion point for exactly that work,
			 * and it is what the end of this function uses for the same reason. Waiting twice is
			 * harmless: the second wait sees an already-signalled fence and returns at once.
			 *
			 * Skipped when there is no fence yet - the first frame, where there is nothing in flight
			 * to wait for. */
			if (data->menu_fence)
				WaitForFences(data->device, 1, &data->menu_fence, VK_TRUE,
					      1000000000ull);
			if (!nrex_resize(data->menu_ui, chain->extent.width, chain->extent.height)) {
				fprintf(stderr, "[nr_layer] the UI image would not resize to %ux%u; "
						"keeping %ux%u\n", chain->extent.width, chain->extent.height,
					nrex_image_width(data->menu_ui), nrex_image_height(data->menu_ui));
			}
		}

		/* The pointer, before NewFrame: ImGui consumes queued input when the frame opens. */
		menu_ui_mouse(data, chain, keys);
		/* Seconds since THIS device's panel was last drawn, not a fixed step.
		 *
		 * ImGui's OWN keyboard repeat is timed on `io.DeltaTime`, and a fixed 1/60 here ties that
		 * repeat to the present rate: measured on the panel's own frames, a held Down moved the
		 * selection once per second at 240 frames and about ten times per second at 60 - so the
		 * selection's speed depends on how many frames a second the game draws, which is not what a
		 * keypress means. The panel's value repeat (`nrex_repeat`) is unaffected: it is already on a
		 * wall clock, so this changes only the half that was wrong.
		 *
		 * The stamp is `data->menu_ui_last_draw`, per device: two devices presenting alternately
		 * would otherwise feed each one the other's interval. Clamped at a quarter second, so a load
		 * hitch or an alt-tab does not arrive as a multi-second delta that re-targets focus half the
		 * panel away. */
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
		nrex_new_frame(data->menu_ui, (float)chain->extent.width,
			       (float)chain->extent.height, dt);
		/* The menu's CURRENT language, not the one this device started with.
		 *
		 * `data->menu_language` is read once, when the bridge is created, and never updated - so
		 * passing it here reset the panel's selector to the startup value on every frame, and the
		 * combo showed "English" forever however many times L was pressed or the list was used.
		 * The rows translated correctly the whole time, because they come from the bridge, which
		 * does track the change; only the selector was pinned.
		 *
		 * `nr_menu_language` is the same accessor the L key reads to step the value, so the two
		 * cannot disagree about which language is showing. */
		/* The panel-level hint - its own keys, and the fact that the game still receives them.
		 * It was not passed at all, so the ImGui panel (the one every game loads) showed no key
		 * list while the hand-drawn one did, and the answer to "the keys also reach the game"
		 * never reached a player. `NR_STR_HINT` is the same string the hand-drawn path sets, so
		 * the two cannot drift. */
		nrex_begin_panel(data->menu_ui, "DLSS-NR on Intel", nr_menu_language(data->menu),
				 nr_menu_text(nr_menu_language(data->menu), NR_STR_HINT));
		/* The effect, as the layer sees it. -1 when this install has no trigger path, so the
		 * checkbox can show "unknown" instead of claiming the effect is off. */
		nrex_set_nr_enabled(data->menu_ui,
					(trigger_path && *trigger_path) ? (access(trigger_path, F_OK) == 0) : -1);
		nrex_rows(data->menu_ui, rows, count, nr_menu_selected(data->menu));
		int acted = nrex_end_panel(data->menu_ui);

		/* Apply what the user moved. The UI reports intent; the bridge owns the knob's range, its
		 * step, the settings write and the daemon's notification - so an edit goes through the same
		 * path an arrow key takes, and the two cannot disagree about what a knob accepts. */
		for (int i = 0; i < count; ++i) {
			if (!rows[i].changed) continue;
			float v = (rows[i].kind == NREX_KIND_FLOAT) ? rows[i].value_f
								    : (float)rows[i].value_i;
			/* The pacing rows come after the knobs and are not knobs: their values live in the
			 * proxy's environment file, not the settings file, so they go to the bridge's own
			 * entry point. `knobs` is the boundary, read from the bridge rather than assumed, so
			 * a knob added later cannot silently be treated as pacing. */
			int knobs = nr_menu_bridge_row_count(data->menu_bridge);
			if (i >= knobs) {
				int what = NR_PACING_ASYNC;
				int value = (int)v;
				if (i == knobs + 1) what = NR_PACING_GPU_IMAGE;
				else if (i == knobs + 2) { what = NR_PACING_EVERY; value += 1; }
				/* The Network row: a flag, so `value` is 0 or 1 and means A or B directly. */
				else if (i == knobs + 3) what = NR_PACING_PRESET;
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
			if (trigger_path && *trigger_path) {
				nr_layer_set_trigger_file(trigger_path, want);
			}
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

		nrex_render(data->menu_ui, (uint64_t)(uintptr_t)data->menu_commands);

		/* The render pass has just written the UI image and the blend is about to read it as a
		 * storage image. Without this the dispatch is free to run while those writes are still in
		 * flight, and what it reads is a mixture of this frame and the last - the banding the
		 * first working build showed, with pieces of the game's own frame inside the panel.
		 *
		 * Both layouts are GENERAL (a storage image has no other), so this orders access rather
		 * than migrating layout. */
		{
			VkImageMemoryBarrier ui_barrier = {0};
			ui_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			ui_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			ui_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			ui_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			ui_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			ui_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			ui_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			ui_barrier.image = (VkImage)(uintptr_t)nrex_image(data->menu_ui);
			ui_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			ui_barrier.subresourceRange.levelCount = 1;
			ui_barrier.subresourceRange.layerCount = 1;
			CmdPipelineBarrier(data->menu_commands,
					   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					   0, 0, NULL, 0, NULL, 1, &ui_barrier);
		}
		{
			static int shown = 0;
			if (!shown) {
				shown = 1;
				LAYER_TRACE("[ov] menu=%p ui=%p view=%p img=%p cmd=%p target=%p %ux%u rows=%d\n",
					(void *)data->menu, (void *)data->menu_ui,
					nrex_image_view(data->menu_ui), nrex_image(data->menu_ui),
					(void *)data->menu_commands, (void *)chain->images[index],
					chain->extent.width, chain->extent.height, count);
			}
		}
		nr_menu_draw_overlay(data->menu, (uint64_t)(uintptr_t)data->menu_commands,
				     (uint64_t)(uintptr_t)chain->images[index],
				     (uint64_t)(uintptr_t)nrex_image_view(data->menu_ui),
				     chain->extent.width, chain->extent.height);
	}

	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	CmdPipelineBarrier(data->menu_commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (EndCommandBuffer(data->menu_commands) != VK_SUCCESS) return 0;

	/* The game's present waits, so this store is ordered after the frame that is being presented.
	 *
	 * An empty wait list was the first attempt and it does not work: the panel was drawn, the
	 * submit completed on its fence, and nothing appeared - because a submit with nothing to wait
	 * for is unordered against the queue's other work, and on this driver the store landed before
	 * the frame's own rendering finished. `transfer()` waits on `data->present_wait` for the same
	 * reason and its pixels do reach the screen.
	 *
	 * Waiting does not consume a semaphore - it orders against it. What must not be signalled
	 * twice is the signal, and the menu signals nothing.
	 *
	 * Zero waits is a real case, not a fallback: the layer captures the waits only while it is
	 * capturing frames, so a game with NR off has none, and the menu still has to draw. */
	uint32_t waits = data->present_wait_count;
	if (waits > data->transfer_stage_capacity) {
		VkPipelineStageFlags *grown = realloc(data->transfer_stages,
						      (size_t)waits * sizeof *grown);
		if (!grown) return 0;
		data->transfer_stages = grown;
		data->transfer_stage_capacity = waits;
		for (uint32_t i = 0; i < waits; ++i)
			grown[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = waits, .pWaitSemaphores = waits ? data->present_wait : NULL,
		.pWaitDstStageMask = waits ? data->transfer_stages : NULL,
		.commandBufferCount = 1, .pCommandBuffers = &data->menu_commands,
		.signalSemaphoreCount = 0, .pSignalSemaphores = NULL };
	if (QueueSubmit(queue, 1, &si, data->menu_fence) != VK_SUCCESS) return 0;

	/* Waited rather than deferred: the buffer is reused next frame, and the wait is the menu's own
	 * cost - a compute dispatch over the panel - not the game's. Deferring would need a command
	 * buffer and a fence per frame in flight, which is more state than the feature is worth. */
	if (WaitForFences(data->device, 1, &data->menu_fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
		return 0;

return 1;
}





/* Read the keyboard and act on it. No GPU work, no allocation, no submit: a handful of
 * `GetAsyncKeyState` calls.
 *
 * Separate from drawing because the two have different costs and different frequencies. The keys
 * have to be read every frame - the NR toggle and the compare key act with the menu closed, and a
 * toggle that only worked while the menu was open would be useless. Drawing only has to happen
 * when something is on screen.
 *
 * Returns 1 when the frame needs a composite. */
/* The rows the bridge already produces, in the shape the ImGui UI wants.
 *
 * A conversion, not a second source of truth: `nr_menu_bridge_row_label/value` are the same strings
 * the hand-drawn menu drew, built from `nr_menu_text` and the live knob values. The bridge keeps
 * ownership; this fills a stack array of views into it for the duration of the frame. */



/* Whether the game's window is the one the user is actually working in.
 *
 * This is the gate OptiScaler puts in front of its whole input feed, and it is the thing this layer
 * was missing. Without it the panel read the SYSTEM-WIDE cursor position and button state every
 * frame, so using any other window while the game sat behind it drove the menu: dragging a
 * progress bar elsewhere moved a slider, selecting text elsewhere selected a row, clicking a title
 * bar elsewhere moved the panel. The user's report was exactly that, and it is not a subtle bug -
 * it makes the panel unusable whenever the game is not in front, which is most of the time on a
 * desktop.
 *
 * OptiScaler learns this from `WM_SETFOCUS`/`WM_KILLFOCUS` because it subclasses the game's
 * `WndProc` (menu/input/input_system_messages.cpp). A Vulkan layer has no window procedure to
 * subclass, so the same fact is asked for directly: the foreground window is compared against the
 * one this process owns. `GetForegroundWindow` is system-wide and needs no message pump, which is
 * the same reason the key reading uses `GetAsyncKeyState`.
 *
 * The comparison is by process, not by handle, because a game can present from a child or a
 * sibling window - the same reason `window_is_ours` exists.
 *
 * **No wrapper, and no `HWND` in the signature.** It was a `static int window_is_foreground(HWND)`
 * that forwarded to `nr_window_is_ours`, which put an unguarded Windows type in this file and
 * broke the Linux build: `nr_layer.c` compiled on Linux before this branch because `windows.h`
 * sat inside `#ifdef _WIN32` with a pthread arm beside it, and 27 lines of bare Win32 undid that.
 * `nr_focus.c` already answers the question with a `void *` and its own no-op arm, so it is called
 * directly - one less indirection, and one less type that only exists on one platform. */

/* ------------------------------------------------------------------ the hooks
 *
 * `nr_layer.c` calls these from three places and knows nothing else about the menu. Off Windows
 * the same three names are no-ops in the header, so the layer's code is identical on both.
 */

void nr_menu_layer_note_storage(struct device_data *data, struct swapchain_data *chain)
{
	(void)data;
	if (chain) chain->storage_ok = 1;
}

int nr_menu_layer_present(struct device_data *data, VkQueue queue,
                          const VkPresentInfoKHR *info)
{
	if (!data || !info) return 0;

	/* Build on the first present of a swapchain that can carry it. It cannot be built earlier:
	 * this is the first moment the layer has the device, the queue family and the swapchain format
	 * together. A failure sets `menu_state` to -1 with a reason, so a surface that cannot carry a
	 * menu is decided once rather than retried every frame.
	 *
	 * This call is what makes the menu exist. Without it `data->menu` is provably NULL at every
	 * read below, and the compiler - correctly - deletes the whole overlay: the first build of this
	 * had `menu_build` defined and never called, and the resulting DLL contained none of the menu's
	 * strings. A build that succeeds is not a menu that runs. */
	/* ---- the ONE place the keyboard is read ----
	 *
	 * Three files used to call `GetAsyncKeyState` on their own: this one, `nr_menu.c` (which had
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
	 * the game is behind another window read this same array - they do not ask again.
	 *
	 * Before the build, not after: the frame the menu is created is the frame its first keypress
	 * has to be seen on. */
	unsigned char keys[256];
	{
		int foreground = window_is_foreground(window_for_thread_for_ui());
		if (foreground) {
			for (int vk = 0; vk < 256; ++vk)
				keys[vk] = nr_key_down(vk) ? 1 : 0;
		} else {
			memset(keys, 0, sizeof keys);
		}
		nr_menu_set_key_levels(data->menu, keys);
		if (data->menu_ui) {
			for (int vk = 0; vk < 256; ++vk)
				nrex_key(data->menu_ui, vk, keys[vk]);
		}
	}

	if (!data->menu && !data->menu_state && menu_enabled()
	    && info->swapchainCount > 0 && info->pSwapchains) {
		struct swapchain_data *first = nr_layer_find_swapchain(info->pSwapchains[0]);
		if (first && first->device == data->device)
			menu_build(data, first);
	}

	if (data->menu && info->swapchainCount > 0 && info->pSwapchains && info->pImageIndices) {
		struct swapchain_data *chain = nr_layer_find_swapchain(info->pSwapchains[0]);
		if (chain && chain->device == data->device && menu_read_keys(data, keys))
			return menu_composite(data, chain, queue, info->pImageIndices[0], keys);
	} else if (data->menu) {
		/* No swapchain to draw into - a present-only queue, or a surface the layer never
		 * tracked. The keys still work, so the toggle and the compare keep responding. */
		menu_read_keys(data, keys);
	}
	return 0;
}

void nr_menu_layer_destroy(struct device_data *data)
{
	if (!data) return;
	/* The bridge goes FIRST: it is what the panel reads its rows through, and it is also what
	 * removes the live preview - the daemon's view of what the user is trying RIGHT NOW. Leaving
	 * that behind made a Save look like it had not happened. */
	if (data->menu_bridge) {
		nr_menu_bridge_destroy(data->menu_bridge);
		data->menu_bridge = NULL;
	}
	if (data->menu_ui) {
		nrex_destroy(data->menu_ui);
		data->menu_ui = NULL;
	}
	if (data->menu) {
		nr_menu_destroy(data->menu);
		data->menu = NULL;
	}
	if (data->menu_fence) {
		PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)
			data->get_device_proc(data->device, "vkDestroyFence");
		if (destroy_fence) destroy_fence(data->device, data->menu_fence, NULL);
		data->menu_fence = VK_NULL_HANDLE;
	}
	if (data->menu_pool) {
		PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)
			data->get_device_proc(data->device, "vkDestroyCommandPool");
		if (destroy_pool) destroy_pool(data->device, data->menu_pool, NULL);
		data->menu_pool = VK_NULL_HANDLE;
	}
}
