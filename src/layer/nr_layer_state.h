/* The layer's per-device and per-swapchain state, in one place.
 *
 * **Why this is a header and not a corner of `nr_layer.c`.** The in-game menu's glue - the
 * composite pass, the key reading, the row building, the mouse - needs 38 fields of
 * `struct device_data` and 6 of `struct swapchain_data`. That glue lives in its own file now, built
 * only on Windows, because on Linux there is no menu: no font, no keys read off Windows, no STORAGE
 * on a swapchain. Moving the glue out is what lets `nr_layer.c` come back to the size it has on
 * `newnr`, and it needs these two structs to be visible to both files.
 *
 * The structs are unchanged from where they were defined; this is a relocation, not a redesign.
 * Everything here is private to the layer's own translation units - nothing outside `src/layer`
 * includes this.
 */
#ifndef NR_LAYER_STATE_H
#define NR_LAYER_STATE_H

#include <stdint.h>

/* The GPU bridge's header names Win32 external-memory extensions, so the platform define has to
 * come before Vulkan is included. `nr_layer.c` used to set it at the top of its own file, which
 * worked while the structs lived there too; now that they are here, this is where it goes - and it
 * is the same pair of choices that file made. */
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#else
#  define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.h>

#include "nr_gpu_bridge.h"
#include "nr_layer_stats.h"
#include "nr_surface_policy.h"
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"

#ifdef _WIN32
typedef SRWLOCK nr_mutex_t;
#define NR_MUTEX_INIT SRWLOCK_INIT
#define nr_mutex_init(m, ...) InitializeSRWLock(m)
#define nr_mutex_lock(m)      AcquireSRWLockExclusive(m)
#define nr_mutex_unlock(m)    ReleaseSRWLockExclusive(m)
#define nr_mutex_destroy(m)   ((void)(m))
#else
typedef pthread_mutex_t nr_mutex_t;
#define NR_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define nr_mutex_init(m, ...) pthread_mutex_init(m, NULL)
#define nr_mutex_lock(m)      pthread_mutex_lock(m)
#define nr_mutex_unlock(m)    pthread_mutex_unlock(m)
#define nr_mutex_destroy(m)   pthread_mutex_destroy(m)
#endif

/* ------------------------------------------------------------------ shared by both files
 *
 * These four came out of `nr_layer.c` when the menu's glue moved. A macro and a `static` function
 * are not linkable from another translation unit, so leaving them behind showed up as five
 * unresolved symbols at link time - which is the honest way for that mistake to surface, but it is
 * still a mistake. Both files use them and neither owns them, so they live here.
 */

#ifndef LAYER_TRACE_LOGGING
#define LAYER_TRACE(...) ((void)0)
#else
#define LAYER_TRACE(...) fprintf(stderr, __VA_ARGS__)
#endif
#define LAYER_ERROR(...) fprintf(stderr, __VA_ARGS__)

#ifdef _WIN32
#define nr_key_down(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
#else
#define nr_key_down(vk) (0)
#endif

static inline double nr_now_seconds(void)
{
#ifdef _WIN32
	static int have_frequency;
	static LARGE_INTEGER cached;
	LARGE_INTEGER counter;
	if (!have_frequency) {
		QueryPerformanceFrequency(&cached);
		have_frequency = 1;
	}
	if (QueryPerformanceCounter(&counter) && cached.QuadPart > 0)
		return (double)counter.QuadPart / (double)cached.QuadPart;
	return 0.0;
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	return 0.0;
#endif
}

#define MAX_SWAPCHAINS 8
#define MAX_IMAGES 8

/* The panel's own array bounds. `MENU_UI_MAX_ROWS` is the widest knob list the bridge can hand over,
 * with room to spare; `MENU_UI_MAX_CHOICES` the widest option list any knob has. They lived beside
 * the code that filled them, which moved to `nr_menu_layer.c`, so they are here where both the
 * struct (which holds the name store) and the filler can see them. */
#define MENU_UI_MAX_ROWS 24
#define MENU_UI_MAX_CHOICES 8

struct device_data {
	VkDevice device;
	VkPhysicalDevice physical;
	VkQueue queue;
	uint32_t queue_family;
	/* A queue family the APPLICATION created, for the UI's own submissions. It is not necessarily
	 * the presenting one - a submission there can sit behind a semaphore wait this layer has
	 * already consumed - and it is not "the other number": a device may have been created with one
	 * family only, which is what the present fixture does. Equal to `queue_family` when there is no
	 * other family to use. See where it is filled in `nr_CreateDevice`. */
	uint32_t ui_queue_family;
	nr_mutex_t present_lock;
	unsigned long frame_counter;
	VkResult transfer_error, device_error;
	VkFence stalled_fence;
	VkSwapchainKHR result_chain;
	int queues_announced;           /* the one-time queue inventory has been logged */
	int gpu_only_notice;
	VkCommandPool pool;
	VkCommandBuffer transfer_commands;
	VkFence transfer_fence;
	VkPipelineStageFlags *transfer_stages;
	uint32_t transfer_stage_capacity;
	struct nr_layer_timing timing;
	struct nr_surface_selection primary;
#ifdef _WIN32
	struct nr_gpu_bridge *gpu;
	HANDLE gpu_peer;
	int gpu_enabled, gpu_blocked, gpu_attached, gpu_exposed, gpu_announced;
	unsigned long gpu_retry_frame;
	/* Why the GPU path stands aside for good on this device, written into the daemon's
	 * log once frames take the usual path, after the daemon's own first lines. */
	char gpu_note[200];
	int gpu_note_logged, gpu_lines;
#endif
	VkDeviceMemory staging_memory;
	VkBuffer staging;
	VkDeviceSize staging_size;
	void *mapped;
	unsigned char *result;          /* the processed frame, held while the trigger is up */
	VkDeviceSize result_size;
	int holding;
	/* One frame captured before the one being processed, so the daemon can be told
	 * which pixels did not move. Filled on the present after the trigger goes up and
	 * released when it goes down; nothing is copied while the trigger is down, so a
	 * game that never triggers pays nothing for this. */
	unsigned char *earlier;
	VkDeviceSize earlier_size;
	const VkSemaphore *present_wait;
	uint32_t present_wait_count;
	int waits_consumed;
	int have_earlier;
	unsigned char *outgoing;        /* colour followed by the mask, for one send */
	VkDeviceSize outgoing_size;
	/* NR_LAYER_ASYNC: the frame sent on the last processed present, whose answer is read
	 * on the next one. Zero-initialised devices must not read fd 0 as a request. */
	int inflight;
	int inflight_fd;
	VkDeviceSize inflight_size;
	uint64_t inflight_timing_started;
	unsigned inflight_timing_slot;
	unsigned inflight_timing_group;
	PFN_vkGetDeviceProcAddr get_device_proc;
	PFN_vkSetDeviceLoaderData set_loader_data;
	PFN_vkQueuePresentKHR present;
	PFN_vkCreateSwapchainKHR create_swapchain;
	PFN_vkDestroySwapchainKHR destroy_swapchain;
	PFN_vkGetDeviceQueue get_device_queue;
	PFN_vkGetDeviceQueue2 get_device_queue2;
	PFN_vkGetSwapchainImagesKHR get_swapchain_images;
	PFN_vkDestroyDevice destroy_device;

	/* The in-game settings menu, built on the first present of a swapchain that took STORAGE.
	 * `menu_state` is 0 until it has been tried, 1 when it exists, -1 when it cannot be built
	 * here - so a surface that cannot carry it is decided once rather than every frame.
	 *
	 * `menu_rows` and the strings behind them are the bridge's, refreshed once a frame while the
	 * menu is open: the knob list lives in Python and this file must not keep a second copy. */
	nr_menu *menu;
	nr_menu_bridge *menu_bridge;
	int menu_state;
	const char *menu_reason;
	/* Why this device cannot carry the panel at all, decided at vkCreateDevice from what the game
	 * enabled (see `menu_device_reason` in nr_layer.c); NULL when it can. */
	const char *menu_device_reason;
	int menu_language;
	/* The menu's own recording, reused like `transfer_commands`. The fence is what makes reuse
	 * safe: the wait completes the previous frame's menu before the next resets the buffer. */
	VkCommandPool menu_pool;
	uint32_t menu_pool_family;      /* the family menu_pool was created for */
	VkCommandBuffer menu_commands;
	VkFence menu_fence;
	nrex_ui *menu_ui;               /* the ImGui UI, when it started; NULL means the quad path */
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
	 * wrote the settings file. `nr_menu.c` now gates all three on `read_keys`, and the layer holds
	 * that off while the game is not in front (see `menu_read_keys`), so the two readers agree
	 * about when the keyboard is the game's.
	 *
	 * The report that produced the move, for the record: "S does not save, R does not reset, END
	 * does nothing", with the panel drawing and its buttons working fine. INS and L already lived
	 * here for the same reason and kept working, which is what made the three the odd ones out
	 * rather than the pattern.
	 *
	 * Home is the effect toggle. It was END, and END is bound by enough games that the key
	 * never arrived; Home was the comparison, and the comparison is what this replaces - so
	 * the key is free and already means "change the picture", which is what the toggle does. */
	int menu_ui_toggle_was_down;
	int menu_ui_save_was_down;
	int menu_ui_reset_was_down;
	/* The pointer the panel uses, in swapchain pixels, read from the system cursor while the
	 * panel owns it (see `menu_ui_mouse`: the layer confines the cursor to the window and writes
	 * its position every frame, so this is one source of truth rather than an accumulated guess).
	 *
	 * Only meaningful while `menu_ui_have_pointer` is set. */
	/* Whether the OS pointer is currently hidden for the panel. `ShowCursor` is a counter,
	 * so it is pushed down once on open and restored once on close rather than every frame. */
	int menu_ui_cursor_hidden;
	float menu_ui_pointer_x;
	float menu_ui_pointer_y;
	int menu_ui_have_pointer;
	/* The OS position the last delta was measured from.
	 *
	 * Two `long`s, not a `POINT`: this struct is declared in every translation unit that includes
	 * the layer's header, `POINT` exists only under `windows.h`, and a Windows type here made the
	 * Linux build fail at a place nothing to do with the window code - the field is used only by
	 * `menu_ui_mouse`, which is itself Windows-only now. Plain integers are portable and the
	 * conversion happens in the one place that reads them. */
	long menu_ui_last_cursor_x;
	long menu_ui_last_cursor_y;
	/* When this device's panel was last drawn, in seconds from the same clock `nrex_repeat`
	 * uses, so ImGui's keyboard repeat is timed on the real interval between presents.
	 *
	 * Per device, not a file static: the layer runs several devices in one process (the
	 * Vulkan loader hands a game's devices to the same layer), and two devices presenting
	 * alternately would otherwise make each one's delta the time since the OTHER one drew
	 * a panel - a value with nothing to do with its own present rate. Zero means "not drawn
	 * yet", which is what makes the first frame fall back to the fixed step. */
	double menu_ui_last_draw;
};

struct swapchain_data {
	VkSwapchainKHR swapchain;
	VkDevice device;
	VkImage images[MAX_IMAGES];
	uint32_t image_count;
	VkFormat format;
	VkExtent2D extent;
	/* The surface accepted VK_IMAGE_USAGE_STORAGE_BIT, so the in-game menu may write into
	 * these images with a compute pass. Zero when it did not: the menu is then not built,
	 * which is what keeps an unsupported surface usable without one. */
	int storage_ok;
	struct nr_surface_record surface;
};

/* Two small helpers the menu's glue needed from the file it came out of.
 *
 * `nr_default_dir` is where a fallback log goes; `chain_image_count` is the swapchain's image count
 * with the minimum the backend wants as a floor. Both are four lines and neither depends on
 * anything above, so they are `static inline` here rather than exported - a function table for two
 * four-line helpers would be more machinery than the problem. */
static inline void nr_default_dir(char *out, size_t size)
{
#ifdef _WIN32
	DWORD n = GetTempPathA((DWORD)size, out);
	if (n == 0 || n >= (DWORD)size) {
		snprintf(out, size, ".");
		return;
	}
	if (n >= 1 && out[n - 1] == '\\') out[n - 1] = '\0';
#else
	snprintf(out, size, "/tmp");
#endif
}

static inline uint32_t chain_image_count(struct swapchain_data *c)
{
	return c && c->image_count ? c->image_count : 2;
}

#endif /* NR_LAYER_STATE_H */
