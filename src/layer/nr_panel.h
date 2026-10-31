/* The in-game panel's logic, shared by the Vulkan layer and the DirectX 12 proxy.
 *
 * The panel came with PR #20 as part of the Vulkan layer's glue (`nr_menu_layer.c`): the keyboard
 * swept once behind the focus gate, the hotkeys (INS, L, Home, S, R), the rows built from the
 * bridge, the pointer held in its one-pixel box, the ImGui frame and what it asked for. None of that
 * is Vulkan, and DirectX 12 games need all of it, so it lives here and both hosts call it; what stays
 * with each host is the drawing - a compute blend into a Vulkan swapchain image there, a render pass
 * over a DXGI back buffer in `src/xess/nr_xess_panel.cpp`.
 *
 * Each frame the host calls `nr_panel_input`, every present whether the panel is open or not, and
 * when it answers 1, `nr_panel_frame`, then renders ImGui's draw data with its own backend. The
 * functions keep the names and the shapes they had in the layer's file, so the history of each
 * decision in the comments still reads true.
 *
 * Windows only, like the rest of the menu.
 */
#ifndef NR_PANEL_H
#define NR_PANEL_H

#include <stddef.h>
#include <stdint.h>

#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the host lends the panel.
 *
 * The effect's trigger is the host's: the Vulkan layer names it with `NR_LAYER_TRIGGER` in its
 * environment, the DirectX 12 proxy reads it out of `nr-env.txt` as a wide path. The Home key, the
 * panel's checkbox and Save all go through these two, so the panel never opens the file itself. */
typedef struct nr_panel_host {
	/* 1 while NR runs, 0 while it does not, -1 when this installation has no trigger. */
	int (*trigger_get)(void *user);
	void (*trigger_set)(void *user, int on);
	void *user;
	/* Whether the panel offers the Vulkan layer's pacing rows: asynchronous mode, the GPU image path
	 * and the cadence. They are the layer's own, read from `nr-env.txt` when the game starts, and a
	 * DirectX 12 game has none of them - its frames are on the GPU, each on its own frame. The
	 * network row is offered either way, whenever `nr-env.txt` could be read. */
	int pacing;
} nr_panel_host;

typedef struct nr_panel nr_panel;

/* The panel's own array bounds. `MENU_UI_MAX_ROWS` is the widest knob list the bridge can hand over,
 * with room to spare; `MENU_UI_MAX_CHOICES` the widest option list any knob has. */
#define MENU_UI_MAX_ROWS 24
#define MENU_UI_MAX_CHOICES 8

/* NR_MENU=0 turns the panel off, for a game where even one GetAsyncKeyState sweep a frame is
 * unwelcome, and for anyone bisecting a problem they suspect is ours. */
int nr_panel_enabled(void);

/* The font the panel draws with, read from the system's font directory (malloc'd; ImGui adopts it),
 * and every codepoint its four languages draw, sorted, for the atlas. */
char *nr_panel_font(size_t *size_out);
int nr_panel_codepoints(int *out, int max);

/* The panel over a menu and a UI the host made. `ui` may be NULL - the quad path, Vulkan's only -
 * and then this file reads no keys of the panel's own. The bridge is the panel's; the menu and the
 * UI stay the host's, and outlive `nr_panel_destroy`. `settings` is the daemon's settings file. */
nr_panel *nr_panel_create(nr_menu *menu, nrex_ui *ui, const char *settings, int language,
                          const nr_panel_host *host);

/* Gives the cursor back, ends the guard's hold and the bridge (which removes the live preview). */
void nr_panel_destroy(nr_panel *panel);

/* The frame's keys, every present, open or shut: the sweep, the hotkeys, the bridge's update, the
 * trigger, and the hold on the game's input. `window` is the window the frames go to when the host
 * knows it (a DXGI swap chain does), or NULL to find this process's own. Returns 1 when there is
 * something to draw. */
int nr_panel_input(nr_panel *panel, void *window);

/* This frame's panel, after `nr_panel_input` said there is one: the rows, the pointer, ImGui's frame
 * and what the user did in it. `width` x `height` is the image it is drawn over. The host renders
 * ImGui's draw data after this, with its own backend. */
void nr_panel_frame(nr_panel *panel, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* NR_PANEL_H */
