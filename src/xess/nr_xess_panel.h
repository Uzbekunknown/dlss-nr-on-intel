/* nr_xess_panel.h - NR's in-game panel in a DirectX 12 game, for the proxy (nr_xess.c), which is C.
 *
 * The panel is the Vulkan layer's (src/layer/nr_panel.c and nr_menu_ui.cpp): INS opens it, Home
 * turns NR on and off, and while it is open the game is kept from the keys and clicks given to it.
 * Here it is drawn in DXGI's Present (nr_xess_panel.cpp), by a Direct3D 12 renderer
 * (src/layer/nr_menu_ui_d3d12.cpp), over the back buffer of whichever swap chain presents - under
 * XeSS frame generation that is the generator's own, so the panel is on every frame shown.
 */
#ifndef NR_XESS_PANEL_H
#define NR_XESS_PANEL_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_xess_panel_host {
	void (*say)(const char *line);          /* the proxy's log, a line at a time */
	/* nr_present_probe.exe, and where its answer is kept between runs of the game */
	wchar_t probe[1024];
	wchar_t answer[1024];
	char settings[1088];                     /* the daemon's settings file (NR_SETTINGS), UTF-8 */
	char env_file[1088];                     /* nr-env.txt, UTF-8, for the network row; "" for none */
	/* The effect's trigger, the proxy's: 1 on, 0 off, -1 when this installation has none. */
	int (*trigger_get)(void *user);
	void (*trigger_set)(void *user, int on);
	void *user;
	/* For the panel's frame counter: the frames the daemon answered so far, and the network's time
	 * it gave with them, in microseconds summed over the `reported` answers that carried any.
	 * Totals, asked once a Present; the presents themselves the panel counts. NULL: no NR rate. */
	void (*nr_counts)(void *user, uint64_t *frames, uint64_t *network_us, uint64_t *reported);
} nr_xess_panel_host;

/* From DllMain: keeps the host and starts the thread that finds DXGI's Present (the probe, or its
 * answer from an earlier run) and hooks it. Nothing is drawn until a swap chain presents. */
void nr_xess_panel_start(const nr_xess_panel_host *host);

/* From xessD3D12Execute, on every call: the first one hooks the device's queues, so the queue a swap
 * chain presents on can be told before the panel is first drawn. */
void nr_xess_panel_list(ID3D12GraphicsCommandList *list);

/* 1 once DXGI's Present is hooked, for a test that has to wait for the install thread. */
int nr_xess_panel_hooked(void);

/* The presents the frame counter has counted so far, and the counter's line as the panel last built
 * it ("" before its first reading), for a test. */
uint64_t nr_xess_panel_presents(void);
const char *nr_xess_panel_rates(void);

#ifdef __cplusplus
}
#endif
#endif
