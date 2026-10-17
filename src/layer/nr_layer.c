#define _GNU_SOURCE
/*
 * nr_layer — a Vulkan layer that hands the presented frame to DLSS-NR.
 *
 * Why a Vulkan layer, and not the route everyone else takes.
 *
 * Every published way of getting DLSS-NR into a game — OptiScaler's fork, the
 * ReShade bridges, the dual-GPU MGPU Bridge — loads NVIDIA's own nvngx_dlssnr.dll
 * and therefore needs an NVIDIA GPU (or, for the AMD lab, a PTX translation of it).
 * None of them can run here. What we have instead is a reimplementation that is
 * already Vulkan compute.
 *
 * And under Proton a DX12 game goes DX12 -> VKD3D-Proton -> Vulkan on ANV, which is
 * the same driver our shaders run on. So the frame we want is already a VkImage on
 * the device we already use: no D3D interop, no Windows DLL, no PCIe transfer. A
 * layer at vkQueuePresentKHR sees it. vkBasalt has done exactly this on Linux for
 * years, so the shape is proven.
 *
 * This file is the capture half: chain into the loader, force TRANSFER_SRC onto the
 * swapchain images, and copy the presented frame out on demand.
 *
 * Build: cc -O2 -shared -fPIC -o libnr_layer.so nr_layer.c -lvulkan
 */
/* The layer only needs a platform header so vulkan.h stops complaining; it never calls
 * into the windowing system. Windows has no Xlib, so pick per platform. */
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#else
#  define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "nr_transport.h"
#include "nr_layer_stats.h"
#include "nr_surface_policy.h"
#include "nr_gpu_bridge.h"
#include "nr_gpu_rpc.h"
/* The in-game settings menu. Windows-only by construction: on Linux the same settings are driven
 * by `nr-panel`, which already exists, so these headers bring their own no-op arm and the layer
 * compiles unchanged on both. */
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"
#include "nr_focus.h"
/* The string tables and the codepoint collector: the UI's font atlas is built from the menu's own
 * text, so this file needs to reach them. */
#include "nr_menu_strings.h"
#include "nr_menu_font.h"

/* ------------------------------------------------------------------ diagnostics
 *
 * Two kinds of message, on the same rule nr_menu_ui.cpp follows.
 *
 * LAYER_ERROR is a real failure worth reporting to whoever is running the game. It always prints.
 *
 * LAYER_TRACE is development chatter - which of the menu's guards shut, a window handle, a swapchain
 * pointer, how many rows were built. It compiles to nothing unless LAYER_TRACE_LOGGING is defined.
 * These were written while the pointer conversion and the composite were being worked out, and they
 * were `fprintf`s into the game's stderr: a released layer should not narrate itself into somebody
 * else's process, and a handle plus a PID is not something to hand out for no reason.
 *
 * The per-frame ones matter most. `[mouse] x=... y=...` fired on every pointer move, unthrottled,
 * while the user was playing. */
#ifndef LAYER_TRACE_LOGGING
#define LAYER_TRACE(...) ((void)0)
#else
#define LAYER_TRACE(...) fprintf(stderr, __VA_ARGS__)
#endif
#define LAYER_ERROR(...) fprintf(stderr, __VA_ARGS__)

#ifdef _WIN32
#  include <io.h>
#  ifndef R_OK
#    define R_OK 4
#  endif
#  ifndef W_OK
#    define W_OK 2
#  endif
#  ifndef F_OK
#    define F_OK 0
#  endif
#  ifndef access#    define access(p, m) _access(p, m)
#  endif
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#endif

/* Locking uses the platform's own primitive: an SRWLOCK needs no destroy step and is
 * what this file used on Windows before, a pthread_mutex is what Linux uses. */
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
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif
#else
#include <dlfcn.h>   /* dladdr: the layer's own path */
#include <spawn.h>
#include <fcntl.h>
#endif


#define MAX_SWAPCHAINS 8
#define MAX_IMAGES 8

/* Capture and writeback consume the game's waits once, complete on private
 * fences, then present with no remaining waits. Legacy NR_LAYER_SYNC values are
 * accepted for launcher compatibility; both use this single verified path. */

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

static struct device_data devices[8];
static struct swapchain_data swapchains[MAX_SWAPCHAINS];

static nr_mutex_t lock = NR_MUTEX_INIT;
static PFN_vkGetInstanceProcAddr next_instance_proc;
static VkInstance layer_instance;
static uint32_t layer_api_version;
static const char *capture_path;
static long capture_every;
static long live_every;
static int async_live;
static const char *socket_path;
static const char *trigger_path;
static int ui_mask;

static int gpu_only_mode(void)
{
    const char *value = getenv("NR_GPU_ONLY");
    return value && !strcmp(value, "1");
}

/* Strict mode can retain the game's image, but must never turn an unavailable
 * shared path into CPU pixel capture, processing or cached-image writeback. */
static int host_pixel_path_allowed(struct device_data *data, const char *operation)
{
    if (!gpu_only_mode()) return 1;
    if (!data || !data->gpu_only_notice) {
        fprintf(stderr, "[nr_layer] GPU-only: CPU pixel path disabled (%s); original GPU image retained\n", operation);
        if (data) data->gpu_only_notice = 1;
    }
    return 0;
}

/* The frame goes to a daemon over a Unix socket rather than being processed in
 * process: the implementation is Python and this is a shared object living inside the
 * game. For a photo mode the game is meant to stall anyway, so the round trip is free;
 * a per-frame pass would need the graph ported to C. */
/* `reply_size` is deliberately separate from `payload_size`: the interface mask makes
 * the request larger than the answer, and reusing one size meant asking for bytes the
 * daemon never sends — the read hit EOF and every masked frame came back unchanged. */
/* Sends the request and leaves the connection open: in asynchronous live mode the
 * answer is read on the next present, so the endpoint has to outlive this call.
 * Returns the open link, or -1. */
static int exchange_send(const void *header, size_t header_size, const void *payload,
			 size_t payload_size)
{
	nr_link link;
	if (nr_link_connect(&link, socket_path) != 0) {
		fprintf(stderr, "[nr_layer] no daemon at %s\n", socket_path);
		return -1;
	}
	if (nr_link_write(&link, header, header_size) != 0
	    || nr_link_write(&link, payload, payload_size) != 0) {
		nr_link_close(&link);
		return -1;
	}
	return nr_link_handle(link);
}

/* Reads the whole answer and closes the connection, whichever way it ends. */
static int exchange_receive(int fd, void *reply, size_t reply_size)
{
	nr_link link = nr_link_from_handle(fd);
	if (nr_link_read(&link, reply, reply_size) != 0) {
		nr_link_close(&link);
		return -1;
	}
	nr_link_close(&link);
	return 0;
}


static int exchange(const void *header, size_t header_size, const void *payload,
		    size_t payload_size, void *reply, size_t reply_size)
{
	int fd = exchange_send(header, header_size, payload, payload_size);
	return fd < 0 ? -1 : exchange_receive(fd, reply, reply_size);
}

/* An answer still on its way is read to the end and thrown away rather than cut off: the
 * daemon is single-file and would log a broken pipe for it, and the panel reads that log.
 * It costs at most one frame of the daemon's, and only when the effect is switched off,
 * the swapchain changes or the device goes. */
static void drop_inflight(struct device_data *data)
{
	data->inflight_timing_started = 0;
	if (!data->inflight) return;
	data->inflight = 0;
	nr_link link = nr_link_from_handle(data->inflight_fd);
	if (gpu_only_mode()) {
		/* A stale legacy reply is closed, not downloaded into a discard buffer. */
		nr_link_close(&link);
		return;
	}
	unsigned char sink[65536];
	for (VkDeviceSize got = 0; got < data->inflight_size; ) {
		size_t want = data->inflight_size - got < sizeof sink
			      ? (size_t)(data->inflight_size - got) : sizeof sink;
		if (nr_link_read(&link, sink, want) != 0) break;
		got += want;
	}
	nr_link_close(&link);
}

static struct device_data *find_device(VkDevice device)
{
	for (int i = 0; i < 8; i++)
		if (devices[i].device == device) return &devices[i];
	return NULL;
}

/* Which device and queue family a VkQueue belongs to.
 *
 * `vkQueuePresentKHR` hands over a queue and nothing else. Taking the first live device
 * and the family of `pQueueCreateInfos[0]` is right for the single-queue case and wrong
 * the moment a game asks for a dedicated present or transfer family, which VKD3D-Proton
 * does: the command buffer would then be allocated from a pool of the wrong family and
 * submitted anyway, which is invalid. So the queues are recorded as they are handed out. */
#define MAX_QUEUES 16
static struct queue_data {
	VkQueue queue;
	VkDevice device;
	uint32_t family;
	int capture_ok;      /* the family can hold a copy, and the queue is not protected */
} queues[MAX_QUEUES];

static struct queue_data *find_queue(VkQueue queue)
{
	for (int i = 0; i < MAX_QUEUES; i++)
		if (queues[i].queue == queue) return &queues[i];
	return NULL;
}

/* Whether a copy can be recorded for this family at all. A present queue is not
 * required to support graphics, compute or transfer — some drivers expose a
 * present-only family — and recording `vkCmdCopyImageToBuffer` on one is invalid.
 * Ported from the parallel ProjectsCodex tree, which had this guard and we did not. */
static int family_can_capture(struct device_data *data, uint32_t family)
{
	if (!data || !next_instance_proc || !layer_instance) return 0;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties properties =
		(PFN_vkGetPhysicalDeviceQueueFamilyProperties)
		next_instance_proc(layer_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
	if (!properties) return 0;
	uint32_t count = 0;
	properties(data->physical, &count, NULL);
	if (!count || family >= count) return 0;
	VkQueueFamilyProperties *families = calloc(count, sizeof *families);
	if (!families) return 0;
	properties(data->physical, &count, families);
	int ok = (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
						 | VK_QUEUE_TRANSFER_BIT)) != 0;
	free(families);
	return ok;
}

static void announce_queues(VkDevice device, VkQueue presenting, uint32_t present_family)
{
	uint32_t families[MAX_QUEUES];
	int count = 0, others = 0;
	nr_mutex_lock(&lock);
	for (int i = 0; i < MAX_QUEUES; i++)
		if (queues[i].queue && queues[i].device == device) {
			families[count++] = queues[i].family;
			if (queues[i].queue != presenting && queues[i].family != present_family)
				others++;
		}
	nr_mutex_unlock(&lock);
	char list[128] = { 0 };
	size_t used = 0;
	for (int i = 0; i < count && used + 8 < sizeof list; i++)
		used += (size_t)snprintf(list + used, sizeof list - used, "%s%u",
					 used ? "," : "", families[i]);
	fprintf(stderr, "[nr_layer] queues: %d handed out, families {%s}; presenting on "
		"family %u. %s\n", count, list, present_family,
		others ? "Multiple families: capture waits on the present's semaphores."
		       : "Capture waits on the present's semaphores; copies finish on private fences.");
}

static void remember_queue(VkDevice device, uint32_t family, VkQueue queue, int capture_ok)
{
	if (!queue) return;
	nr_mutex_lock(&lock);
	if (!find_queue(queue))
		for (int i = 0; i < MAX_QUEUES; i++)
			if (!queues[i].queue) {
				queues[i] = (struct queue_data){ queue, device, family, capture_ok };
				break;
			}
	nr_mutex_unlock(&lock);
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue(VkDevice device, uint32_t family,
					     uint32_t index, VkQueue *queue)
{
	struct device_data *data = find_device(device);
	if (!data || !data->get_device_queue) return;
	data->get_device_queue(device, family, index, queue);
	remember_queue(device, family, *queue, family_can_capture(data, family));
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue2(VkDevice device,
					      const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
	struct device_data *data = find_device(device);
	if (!data || !data->get_device_queue2) return;
	data->get_device_queue2(device, info, queue);
	/* Protected memory cannot be read back into a host-visible buffer, so a protected
	 * queue is never a capture source however capable its family is. */
	remember_queue(device, info->queueFamilyIndex, *queue,
		       (info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)
		       ? 0 : family_can_capture(data, info->queueFamilyIndex));
}

/* Four bytes a pixel is assumed everywhere downstream: the staging buffer is sized
 * `width * height * 4`, `vkCmdCopyImageToBuffer` derives its extent from the image, and
 * the daemon decodes exactly these five formats. An HDR swapchain — R16G16B16A16_SFLOAT
 * is eight — would have the driver copy twice what the buffer holds. So a format that
 * is not on this list is not tracked at all, and the layer stays out of the way. */
static int format_is_four_bytes(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return 1;
	default:
		return 0;
	}
}

static struct swapchain_data *find_swapchain(VkSwapchainKHR swapchain)
{
	for (int i = 0; i < MAX_SWAPCHAINS; i++)
		if (swapchains[i].swapchain == swapchain) return &swapchains[i];
	return NULL;
}

/* The loader hands each layer a chain of get-proc-address functions; these walk it. */
static VkLayerInstanceCreateInfo *instance_chain(const VkInstanceCreateInfo *info)
{
	VkLayerInstanceCreateInfo *item = (VkLayerInstanceCreateInfo *)info->pNext;
	while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
			 && item->function == VK_LAYER_LINK_INFO))
		item = (VkLayerInstanceCreateInfo *)item->pNext;
	return item;
}

static VkLayerDeviceCreateInfo *device_chain(const VkDeviceCreateInfo *info)
{
	VkLayerDeviceCreateInfo *item = (VkLayerDeviceCreateInfo *)info->pNext;
	while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
			 && item->function == VK_LAYER_LINK_INFO))
		item = (VkLayerDeviceCreateInfo *)item->pNext;
	return item;
}

/* The directory the knob files and the daemon's log live in, when the environment does
 * not name one. nr_paths.py picks /tmp on Linux and the user's temp folder on Windows;
 * this matches that, so a default on one side is a default on the other.
 *
 * GetTempPathA returns the length of what it wrote, including the trailing backslash, and
 * 0 on failure. The trailing separator is trimmed, but only after checking that n indexes
 * something we actually wrote: on failure n is 0, and when the buffer is too small n is
 * the length it *would* have needed - either one makes out[n - 1] read and write outside
 * the array. */
static void nr_default_dir(char *out, size_t size)
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

/* The interpreter to start the daemon with, when NR_PYTHON does not name one.
 *
 * It cannot be "python3" on both: on Windows that name belongs to the Microsoft Store's
 * App Execution Alias, a zero-byte stub that opens the Store and exits 9009 with no
 * output. `CreateProcessA` starts it happily, so the spawn looks like it worked and the
 * daemon simply never appears - measured here, and the reason the log stayed empty.
 *
 * The launcher Windows installs for python.org is `py`, which is not a Store alias and
 * is on PATH whenever any Python 3 is; `python` is the installer's other name. The
 * launcher is preferred because it cannot be the alias, and a path that exists on disk
 * is preferred over a bare name so a stray `py` earlier on PATH cannot win.
 *
 * On Linux "python3" is right and stays. */
static const char *nr_default_python(void)
{
#ifdef _WIN32
	static char found[1024];
	const char *cands[] = { "py", "python" };
	for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
		char buf[1024];
		DWORD n = SearchPathA(NULL, cands[i], ".exe", (DWORD)sizeof buf, buf, NULL);
		if (n == 0 || n >= sizeof buf) continue;
		/* The Store alias is a reparse point with no bytes behind it; its real
		 * file is zero length. Anything with content is an interpreter. */
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(buf, GetFileExInfoStandard, &fad)
		    && fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0)
			continue;
		snprintf(found, sizeof found, "%s", buf);
		return found;
	}
	return "py";                    /* let CreateProcess say so if there is none */
#else
	return "python3";
#endif
}

static void ensure_daemon(void);

static void ensure_daemon(void)
{
	/* Opt-in, and silent when off: most processes that load the layer (vulkaninfo,
	 * a compositor) must not start a daemon and must not print anything either. */
	if (!getenv("NR_LAYER_SPAWN")) return;
	/* Asked to start one, so from here every way out says why: a tester's game loaded the
	 * layer and started no daemon, with nothing said (issue #12). Its stderr is where the
	 * loader's own log goes under VK_LOADER_DEBUG, so one capture holds both. */
	/* Nothing to talk to without an endpoint: spawning a daemon nobody uses costs
	 * ~3 GiB. */
	if (!socket_path) {
		fprintf(stderr, "[nr_layer] no daemon started: NR_LAYER_SPAWN is set and "
			"NR_LAYER_SOCKET is not\n");
		return;
	}
	/* Do not re-spawn from inside a daemon we started: it inherits VK_INSTANCE_LAYERS
	 * and would load this layer again, recursing until the OOM killer arrives. The
	 * child is marked NR_LAYER_SPAWNED, so its own layer sees this and steps back. */
	const char *spawned = getenv("NR_LAYER_SPAWNED");
	if (spawned) {
		fprintf(stderr, "[nr_layer] no daemon started: NR_LAYER_SPAWNED=%s marks this "
			"process as one NR started\n", spawned);
		return;
	}

	/* Already up? Then leave it. A one-shot probe, not a blocking connect: waiting
	 * would stall every game load. */
	{
		int alive = 0;
#ifdef _WIN32
		HANDLE h = CreateFileA(socket_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				       OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); alive = 1; }
		else if (GetLastError() == ERROR_PIPE_BUSY) alive = 1;   /* exists, busy */
#else
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			struct sockaddr_un a;
			memset(&a, 0, sizeof a);
			a.sun_family = AF_UNIX;
			snprintf(a.sun_path, sizeof a.sun_path, "%s", socket_path);
			if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) alive = 1;
			close(fd);
		}
#endif
		if (alive) {
			fprintf(stderr, "[nr_layer] no daemon started: one already answers on %s\n",
				socket_path);
			return;
		}
	}

	const char *py = getenv("NR_PYTHON");
	if (!py || !*py) py = nr_default_python();
	/* The daemon script lives next to *this library*. /proc/self/exe is the game
	 * (a Wine binary under Proton), so ask the loader for our own path instead. */
	char self[1024];
	self[0] = '\0';
#ifdef _WIN32
	{
		HMODULE hmod;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				       (LPCWSTR)(const void *)&ensure_daemon, &hmod)) {
			WCHAR w[1024];
			if (GetModuleFileNameW(hmod, w, 1024))
				WideCharToMultiByte(CP_UTF8, 0, w, -1, self, sizeof self, NULL, NULL);
		}
	}
#else
	{
		Dl_info info;
		if (dladdr((const void *)&ensure_daemon, &info) && info.dli_fname)
			snprintf(self, sizeof self, "%s", info.dli_fname);
	}
#endif
	if (!self[0]) {
		fprintf(stderr, "[nr_layer] could not locate this layer's path; set NR_DAEMON\n");
		return;
	}

	char daemon_buf[1024];
	const char *daemon = getenv("NR_DAEMON");
	if (!daemon || !*daemon) {
		/* Where the daemon sits relative to the library, in the layouts this tree and
		 * its Makefile produce:
		 *   <dir>/nr_daemon.py              flat deployment
		 *   <dir>/src/layer/nr_daemon.py    a checkout, library beside the sources
		 *   ../src/layer/nr_daemon.py       `make` builds the library into work/ and
		 *                                   prepare_layer.py points the manifest there,
		 *                                   so work/ is the directory beside it
		 * The first that exists wins. */
		char *slash = self;
		for (char *p = self; *p; p++) if (*p == '/' || *p == '\\') slash = p;
		*slash = '\0';
		const char *cands[] = { "/nr_daemon.py", "/src/layer/nr_daemon.py",
					"/../src/layer/nr_daemon.py" };
		daemon = NULL;
		for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
			snprintf(daemon_buf, sizeof daemon_buf, "%s%s", self, cands[i]);
			if (access(daemon_buf, R_OK) == 0) { daemon = daemon_buf; break; }
		}
		if (daemon) {
			/* The root is derived by stripping two levels off this path, so it must
			 * not still contain a ".." when that happens. */
			char resolved[1024];
#ifdef _WIN32
			void *ok = _fullpath(resolved, daemon, sizeof resolved);
#else
			void *ok = realpath(daemon, resolved);
#endif
			if (ok) {
				snprintf(daemon_buf, sizeof daemon_buf, "%s", resolved);
				daemon = daemon_buf;
			}
		}
		if (!daemon) {
			fprintf(stderr, "[nr_layer] could not find nr_daemon.py next to the "
				"layer; set NR_DAEMON to its path\n");
			return;
		}
	}

	/* The daemon's module root is two levels above src/layer/nr_daemon.py; for a flat
	 * deployment it is the directory holding the script. NR_ROOT overrides both. */
	char root_buf[1024];
	const char *root = getenv("NR_ROOT");
	if (!root || !*root) {
		snprintf(root_buf, sizeof root_buf, "%s", daemon);
		char *slash = root_buf;
		for (char *p = root_buf; *p; p++) if (*p == '/' || *p == '\\') slash = p;
		*slash = '\0';                                  /* .../src/layer */
		if (strstr(root_buf, "/src/layer") || strstr(root_buf, "\\src\\layer")) {
			slash = root_buf;
			for (char *p = root_buf; *p; p++) if (*p == '/' || *p == '\\') slash = p;
			*slash = '\0';                              /* .../src */
			slash = root_buf;
			for (char *p = root_buf; *p; p++) if (*p == '/' || *p == '\\') slash = p;
			*slash = '\0';                              /* ... (the root) */
		}
		root = root_buf;
	}

	/* Same defaults as src/layer/nr_paths.py, or the knobs the tools write never
	 * reach a daemon started from here. */
	const char *settings = getenv("NR_SETTINGS");
	char settings_dir[1024];
	nr_default_dir(settings_dir, sizeof settings_dir);
	/* room for the whole directory and the name: a 1024-byte default cannot be cut */
	char settings_def[1024 + 32];
	snprintf(settings_def, sizeof settings_def, "%s/nr_settings.json", settings_dir);
	if (!settings || !*settings) settings = settings_def;

	char cmd[4096];
	int written = snprintf(cmd, sizeof cmd,
			       "\"%s\" \"%s\" --socket \"%s\" --settings \"%s\" --root \"%s\"",
			       py, daemon, socket_path, settings, root);
	if (written < 0 || (size_t)written >= sizeof cmd) {
		/* a cut command line would start some other program than the daemon */
		fprintf(stderr, "[nr_layer] the daemon's command line is longer than %zu "
			"bytes; start the daemon by hand\n", sizeof cmd);
		return;
	}

#ifdef _WIN32
	{
		STARTUPINFOA si; PROCESS_INFORMATION pi;
		memset(&si, 0, sizeof si); si.cb = sizeof si;
		memset(&pi, 0, sizeof pi);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = INVALID_HANDLE_VALUE;
		char log[1024 + 32];
		char log_dir[1024];
		nr_default_dir(log_dir, sizeof log_dir);
		const char *log_env = getenv("NR_LAYER_LOG");
		if (log_env && *log_env)
			snprintf(log, sizeof log, "%s", log_env);
		else
			snprintf(log, sizeof log, "%s/nr_daemon.log", log_dir);
		/* Both handles inheritable, or the child's standard streams are invalid: the
		 * spawn passes STARTF_USESTDHANDLES with bInheritHandles=TRUE, and a handle
		 * created without this SECURITY_ATTRIBUTES is not inherited by anyone. Both
		 * failures are silent - the daemon starts and runs, and its log stays empty.
		 *
		 * stdin is NUL rather than INVALID_HANDLE_VALUE. An invalid handle is inherited
		 * as invalid, and anything the daemon starts that asks for its stdin - Python's
		 * own subprocess with capture_output is the one that bit us - fails with
		 * "The handle is invalid" and takes the daemon down with it. NUL is a real,
		 * readable handle that says there is nothing to read. */
		SECURITY_ATTRIBUTES sa = { .nLength = sizeof sa, .lpSecurityDescriptor = NULL,
					   .bInheritHandle = TRUE };
		si.hStdInput = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
					   &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		si.hStdOutput = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
					    &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		si.hStdError = si.hStdOutput;
		/* The marker goes in the child's environment block, not ours: setting it in
		 * the game would stop a second instance in the same process from ever
		 * spawning, and setenv is not safe while other threads read the environment.
		 * VK_INSTANCE_LAYERS goes too: the daemon builds its own Vulkan instance, and
		 * with the layer still named there its status lines land in the daemon's log
		 * where a reader takes them for the layer talking about the game. */
		/* The child's environment, built from ours with two entries dropped and one
		 * added. Sized from what is actually there rather than a fixed 8 KB: an
		 * environment larger than the guess would have been truncated silently, and
		 * the daemon would start with a partial environment and no sign of why.
		 *
		 * The marker carries the game's process id, and the daemon ends when that
		 * process does (nr_daemon.end_with_game). Steam counts every process a game
		 * starts as the game: a daemon that outlived it kept the game running for Steam,
		 * so Restore Steam refused, and kept the model in memory until it was killed. */
		char marker[48];
		snprintf(marker, sizeof marker, "NR_LAYER_SPAWNED=%lu",
			 (unsigned long)GetCurrentProcessId());
		size_t marker_size = strlen(marker) + 1;
		char *env = GetEnvironmentStringsA();
		size_t need = marker_size + 1;
		if (env)
			for (char *e = env; *e; e += strlen(e) + 1)
				need += strlen(e) + 1;
		char *env_block = malloc(need);
		if (!env_block) {
			if (env) FreeEnvironmentStringsA(env);
			fprintf(stderr, "[nr_layer] out of memory building the daemon's "
				"environment; frame stays as the game drew it\n");
			return;
		}
		size_t used = 0;
		if (env) {
			for (char *e = env; *e; e += strlen(e) + 1) {
				if (strncmp(e, "NR_LAYER_SPAWNED=", 17) == 0) continue;
				if (strncmp(e, "VK_INSTANCE_LAYERS=", 19) == 0) continue;
				size_t len = strlen(e) + 1;
				memcpy(env_block + used, e, len);
				used += len;
			}
			FreeEnvironmentStringsA(env);
		}
		memcpy(env_block + used, marker, marker_size);
		used += marker_size;
		env_block[used] = '\0';

		/* A console without a window, not DETACHED_PROCESS: a virtual environment's
		 * python.exe is a launcher that starts the real interpreter as its own child, and a
		 * console child of a process with no console gets a new, visible one - over the game,
		 * which loses the focus and, full screen, stops presenting. Children of a
		 * CREATE_NO_WINDOW process share its hidden console instead. */
		BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
					 env_block, NULL, &si, &pi);
		free(env_block);
		if (si.hStdInput != INVALID_HANDLE_VALUE &&
		    si.hStdInput != NULL) CloseHandle(si.hStdInput);
		if (si.hStdOutput != INVALID_HANDLE_VALUE) CloseHandle(si.hStdOutput);
		if (!ok) {
			fprintf(stderr, "[nr_layer] daemon spawn failed (%lu); frame stays "
				"as the game drew it\n", GetLastError());
			return;
		}
		CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	}
#else
	{
		/* Copy the environment with the marker added, so the game's own environment is
		 * untouched (setenv() in a multithreaded process is not safe).
		 * VK_INSTANCE_LAYERS is dropped for the same reason as on Windows: the daemon
		 * builds its own Vulkan instance and would otherwise load this layer too.
		 * The marker carries the game's process id, as on Windows, and the daemon ends
		 * when the game does (nr_daemon.end_with_game): Steam's reaper waits for every
		 * process a game started, so a daemon that outlived it kept the game running
		 * for Steam, and the model in memory. */
		extern char **environ;
		char marker[48];
		snprintf(marker, sizeof marker, "NR_LAYER_SPAWNED=%ld", (long)getpid());
		size_t count = 0;
		while (environ[count]) count++;
		char **child_env = malloc((count + 2) * sizeof *child_env);
		if (!child_env) return;
		size_t w = 0;
		for (size_t i = 0; i < count; i++)
			if (strncmp(environ[i], "NR_LAYER_SPAWNED=", 17) != 0
			    && strncmp(environ[i], "VK_INSTANCE_LAYERS=", 19) != 0)
				child_env[w++] = environ[i];
		child_env[w++] = marker;
		child_env[w] = NULL;

		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
		const char *log = getenv("NR_LAYER_LOG");
		char log_dir2[1024];
		nr_default_dir(log_dir2, sizeof log_dir2);
		char log_def[1024 + 32];
		snprintf(log_def, sizeof log_def, "%s/nr_daemon.log", log_dir2);
		if (!log || !*log) log = log_def;
		posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		posix_spawn_file_actions_adddup2(&fa, 1, 2);

		char *argv[10];
		int i = 0;
		argv[i++] = (char *)py;
		argv[i++] = (char *)daemon;
		argv[i++] = "--socket";   argv[i++] = (char *)socket_path;
		argv[i++] = "--settings"; argv[i++] = (char *)settings;
		argv[i++] = "--root";     argv[i++] = (char *)root;
		argv[i] = NULL;

		pid_t pid;
		if (posix_spawnp(&pid, py, &fa, NULL, argv, child_env) != 0)
			fprintf(stderr, "[nr_layer] daemon spawn failed; frame stays as the "
				"game drew it\n");
		free(child_env);
		posix_spawn_file_actions_destroy(&fa);
	}
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateInstance(const VkInstanceCreateInfo *info,
						 const VkAllocationCallbacks *allocator,
						 VkInstance *instance)
{
	VkLayerInstanceCreateInfo *link = instance_chain(info);
	if (!link) return VK_ERROR_INITIALIZATION_FAILED;
	next_instance_proc = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	PFN_vkCreateInstance create =
		(PFN_vkCreateInstance)next_instance_proc(NULL, "vkCreateInstance");
	VkResult r = create(info, allocator, instance);
	if (r == VK_SUCCESS) {
		/* Physical-device entry points cannot be resolved against a NULL
		 * instance — only the handful of global ones can — so the instance has
		 * to be kept. */
		layer_instance = *instance;
		layer_api_version = info->pApplicationInfo && info->pApplicationInfo->apiVersion
			? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
		capture_path = getenv("NR_LAYER_CAPTURE");
		socket_path = getenv("NR_LAYER_SOCKET");
		ensure_daemon();
		trigger_path = getenv("NR_LAYER_TRIGGER");
		const char *mask = getenv("NR_LAYER_UI_MASK");
		ui_mask = mask && strcmp(mask, "0") != 0;
		const char *every = getenv("NR_LAYER_EVERY");
		capture_every = every ? strtol(every, NULL, 10) : 0;
		const char *sync = getenv("NR_LAYER_SYNC");
		(void)sync;
		const char *live = getenv("NR_LAYER_LIVE");
		live_every = live ? strtol(live, NULL, 10) : 0;
		if (live_every < 0) live_every = 0;
		const char *pipelined = getenv("NR_LAYER_ASYNC");
		async_live = live_every > 0 && pipelined && strcmp(pipelined, "0") != 0;
		if (async_live)
			fprintf(stderr, "[nr_layer] async: each processed present shows the answer "
				"for the one before it, and the daemon works while the game draws\n");
		if (live_every > 0)
			fprintf(stderr, "[nr_layer] live: every %ld%s present goes through the "
				"network, the frames between hold the last result\n",
				live_every, live_every == 1 ? "st" : "th");
		if (live_every > 0 && getenv("NR_LAYER_TRIGGER"))
			fprintf(stderr, "[nr_layer] live runs while the trigger exists; "
				"remove it to hand the game back\n");
		fprintf(stderr, "[nr_layer] sync: present waits + private fences (readback and writeback)\n");
		fprintf(stderr, "[nr_layer] active; socket=%s trigger=%s capture=%s every=%ld\n",
			socket_path ? socket_path : "(none)",
			trigger_path ? trigger_path : "(none)",
			capture_path ? capture_path : "(off)", capture_every);
		if (ui_mask)
			fprintf(stderr, "[nr_layer] ui mask on: the first present after the "
				"trigger is kept to find what held still\n");
	}
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateDevice(VkPhysicalDevice physical,
					       const VkDeviceCreateInfo *info,
					       const VkAllocationCallbacks *allocator,
					       VkDevice *device)
{
	VkLayerDeviceCreateInfo *link = device_chain(info);
	if (!link) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkSetDeviceLoaderData set_loader_data = NULL;
	for (VkLayerDeviceCreateInfo *item = (VkLayerDeviceCreateInfo *)info->pNext;
	     item; item = (VkLayerDeviceCreateInfo *)item->pNext)
		if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
		    item->function == VK_LOADER_DATA_CALLBACK) {
			set_loader_data = item->u.pfnSetDeviceLoaderData;
			break;
		}
	PFN_vkGetInstanceProcAddr next_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_device = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;

	PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_instance(layer_instance, "vkCreateDevice");
	if (!create) return VK_ERROR_INITIALIZATION_FAILED;
	const VkDeviceCreateInfo *chosen = info;
#ifdef _WIN32
	struct nr_gpu_device_patch patch;
	memset(&patch, 0, sizeof patch);
	if (layer_api_version >= VK_API_VERSION_1_1 && nr_gpu_patch_device(&patch, physical, info,
		(PFN_vkEnumerateDeviceExtensionProperties)next_instance(layer_instance, "vkEnumerateDeviceExtensionProperties"),
		(PFN_vkGetPhysicalDeviceFeatures2)next_instance(layer_instance, "vkGetPhysicalDeviceFeatures2"),
		(PFN_vkGetPhysicalDeviceProperties)next_instance(layer_instance, "vkGetPhysicalDeviceProperties")))
		chosen = &patch.info;
	else if (getenv("NR_GPU_TRANSPORT") && !strcmp(getenv("NR_GPU_TRANSPORT"), "1"))
		fprintf(stderr, "[nr_layer] GPU transport fallback: %s\n",
			patch.reason ? patch.reason : "Vulkan 1.1 instance required");
	VkLayerDeviceLink *saved_link = link->u.pLayerInfo;
#endif
	VkResult r = create(physical, chosen, allocator, device);
#ifdef _WIN32
	if (r != VK_SUCCESS && patch.enabled) {
		fprintf(stderr, "[nr_layer] GPU transport fallback: patched device creation failed (%d)\n", r);
		link->u.pLayerInfo = saved_link;
		r = create(physical, info, allocator, device);
		patch.enabled = 0;
	}
	int gpu_enabled = patch.enabled && r == VK_SUCCESS;
	nr_gpu_patch_free(&patch);
#endif
	if (r != VK_SUCCESS) return r;

	nr_mutex_lock(&lock);
	struct device_data *data = NULL;
	for (int i = 0; i < 8; i++) if (!devices[i].device) { data = &devices[i]; break; }
	if (data) {
		memset(data, 0, sizeof *data);
		nr_mutex_init(&data->present_lock, NULL);
		data->device = *device;
		data->physical = physical;
		data->get_device_proc = next_device;
		data->set_loader_data = set_loader_data;
#ifdef _WIN32
		data->gpu_enabled = gpu_enabled;
#endif
		data->present = (PFN_vkQueuePresentKHR)next_device(*device, "vkQueuePresentKHR");
		data->get_device_queue =
			(PFN_vkGetDeviceQueue)next_device(*device, "vkGetDeviceQueue");
		data->get_device_queue2 =
			(PFN_vkGetDeviceQueue2)next_device(*device, "vkGetDeviceQueue2");
		data->destroy_swapchain =
			(PFN_vkDestroySwapchainKHR)next_device(*device, "vkDestroySwapchainKHR");
		data->create_swapchain =
			(PFN_vkCreateSwapchainKHR)next_device(*device, "vkCreateSwapchainKHR");
		data->get_swapchain_images =
			(PFN_vkGetSwapchainImagesKHR)next_device(*device, "vkGetSwapchainImagesKHR");
		data->destroy_device = (PFN_vkDestroyDevice)next_device(*device, "vkDestroyDevice");
		data->queue_family = info->queueCreateInfoCount
			? info->pQueueCreateInfos[0].queueFamilyIndex : 0;
		/* The UI's queue comes from a family the application actually created. It used to be
		 * `queue_family == 0 ? 1 : 0` - a guess that family 1 exists, which held for the game
		 * (both families are created there) and not for anything that creates one. Validation
		 * named it twice: vkGetDeviceQueue(1) and the UI's command pool, on a device created with
		 * family 0 alone. Picking from the create infos costs nothing and is the thing the spec
		 * asks for. */
		data->ui_queue_family = data->queue_family;
		for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
			if (info->pQueueCreateInfos[i].queueFamilyIndex != data->queue_family) {
				data->ui_queue_family = info->pQueueCreateInfos[i].queueFamilyIndex;
				break;
			}
	}
	nr_mutex_unlock(&lock);
	return r;
}

/* The swapchain images are the frame we want, so they have to be copyable. The loader
 * lets a layer edit the create info on the way down; without TRANSFER_SRC the copy
 * below is invalid, and this is the one place it can be added.
 *
 * STORAGE goes on with it, for the in-game menu. The menu writes into the swapchain image
 * with a compute pass, which needs the image to be a storage image - and a surface that
 * refuses STORAGE must leave the menu switched off rather than fail on the first
 * dispatch. So the two flags are asked for together but tracked apart: a surface that
 * takes TRANSFER and not STORAGE keeps capture and loses the menu, which is the right way
 * round because capture is what the daemon needs.
 *
 * What must NOT happen is writing a storage image the surface never agreed to: on the
 * Intel driver that is a device loss, and on any driver a validation error. The menu
 * refuses to build when the format cannot be one - see `format_supports_storage` in
 * nr_menu.c, which asks the physical device the same question from the other side. */
VKAPI_ATTR VkResult VKAPI_CALL nr_CreateSwapchainKHR(VkDevice device,
						     const VkSwapchainCreateInfoKHR *info,
						     const VkAllocationCallbacks *allocator,
						     VkSwapchainKHR *swapchain)
{
	struct device_data *data = find_device(device);
	if (!data) return VK_ERROR_INITIALIZATION_FAILED;
	const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
				       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkSwapchainCreateInfoKHR patched = *info;
	patched.imageUsage |= copies;
	/* The caps say whether the surface supports STORAGE; asking for a flag the surface does not
	 * advertise makes vkCreateSwapchainKHR fail, and the fallback below would then lose the
	 * transfer flags too and turn capture off. So STORAGE is only requested when the caps offer
	 * it, and the menu is only built when it was. */
	int storage_ok = 0;
	{
		/* Resolved here rather than cached: a swapchain is created a handful of times in a
		 * session, and a pointer stored on the device would need invalidating when the instance
		 * goes away - which this file has been bitten by before. */
		PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn =
			(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
			next_instance_proc(layer_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		if (caps_fn && data->physical) {
			VkSurfaceCapabilitiesKHR caps;
			memset(&caps, 0, sizeof caps);
			if (caps_fn(data->physical, info->surface, &caps) == VK_SUCCESS
			    && (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
				patched.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
				storage_ok = 1;
			}
		}
	}
	int copyable = 1;
	VkResult r = data->create_swapchain(device, &patched, allocator, swapchain);
	if (r != VK_SUCCESS) {
		/* The surface may refuse transfer usage. Falling back keeps the game alive,
		 * but the images then lack TRANSFER_SRC and copying from them is invalid
		 * usage — so the swapchain is created and deliberately not tracked. Before
		 * this the fallback silently left us issuing an illegal copy every frame;
		 * the parallel ProjectsCodex tree checks the flags and we did not. */
		copyable = (info->imageUsage & copies) == copies;
		r = data->create_swapchain(device, info, allocator, swapchain);
		storage_ok = 0;
	}
	if (r != VK_SUCCESS) return r;
	if (!copyable) {
		fprintf(stderr, "[nr_layer] swapchain refused transfer usage; capture off\n");
		return r;
	}
	/* One layer, and not protected: the copy reads a single 2-D image, and protected
	 * memory cannot reach a host-visible buffer at all. */
	if (info->imageArrayLayers != 1
	    || (info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) {
		fprintf(stderr, "[nr_layer] swapchain is multi-layer or protected; capture off\n");
		return r;
	}

	if (!format_is_four_bytes(info->imageFormat)) {
		fprintf(stderr, "[nr_layer] swapchain format %d is not four bytes a pixel; "
			"leaving it alone\n", info->imageFormat);
		return r;
	}
	nr_mutex_lock(&lock);
	struct swapchain_data *entry = NULL;
	for (int i = 0; i < MAX_SWAPCHAINS; i++)
		if (!swapchains[i].swapchain) { entry = &swapchains[i]; break; }
	if (!entry)
		fprintf(stderr, "[nr_layer] all %d swapchain slots are in use; this one is "
			"not tracked\n", MAX_SWAPCHAINS);
	if (entry) {
		memset(entry, 0, sizeof *entry);
		entry->swapchain = *swapchain;
		entry->device = device;
		entry->format = info->imageFormat;
		entry->extent = info->imageExtent;
		/* Whether this surface took VK_IMAGE_USAGE_STORAGE_BIT, so the menu knows whether it
		 * may write into these images. A surface that took transfer and not storage keeps
		 * capture and loses the menu - see the comment above nr_CreateSwapchainKHR. */
		entry->storage_ok = storage_ok;
		entry->surface.handle = (uint64_t)*swapchain;
		entry->surface.width = info->imageExtent.width;
		entry->surface.height = info->imageExtent.height;
		entry->image_count = MAX_IMAGES;
		VkResult got = data->get_swapchain_images(device, *swapchain,
							  &entry->image_count, entry->images);
		/* VK_INCOMPLETE means there are more images than the array holds, and
		 * `pImageIndices` may then name one past the end. Refusing is the only safe
		 * answer; the alternative is an out-of-bounds read every present. */
		if (got != VK_SUCCESS || !entry->image_count) {
			memset(entry, 0, sizeof *entry);
			nr_mutex_unlock(&lock);
			fprintf(stderr, "[nr_layer] swapchain has more than %d images or none; "
				"capture off\n", MAX_IMAGES);
			return r;
		}
		fprintf(stderr, "[nr_layer] swapchain %ux%u format %d, %u images\n",
			entry->extent.width, entry->extent.height, entry->format,
			entry->image_count);
	}
	nr_mutex_unlock(&lock);
	return r;
}

/* A family change is legal only after our last submission completed. A failed
 * fence wait makes the whole device terminal and pins these resources until
 * teardown. Destroying the pool also releases its cached command buffer. */
static void release_transfer_resources(struct device_data *data)
{
    if (data->transfer_fence) {
        PFN_vkDestroyFence destroy = (PFN_vkDestroyFence)
            data->get_device_proc(data->device, "vkDestroyFence");
        if (destroy) destroy(data->device, data->transfer_fence, NULL);
        data->transfer_fence = VK_NULL_HANDLE;
    }
    if (data->pool) {
        PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)
            data->get_device_proc(data->device, "vkDestroyCommandPool");
        if (destroy) destroy(data->device, data->pool, NULL);
        data->pool = VK_NULL_HANDLE;
    }
    data->transfer_commands = VK_NULL_HANDLE;
    free(data->transfer_stages);
    data->transfer_stages = NULL;
    data->transfer_stage_capacity = 0;
}

#ifdef _WIN32
/* The importer acknowledges RELEASE only after its private GPU fences and
 * imported objects are finished. A lost reply never proves that no import
 * happened: retain the export until acknowledgement or authenticated peer exit. */
static int release_gpu_bridge(struct device_data *data)
{
    if (!data->gpu) return 1;
    int safe = !data->gpu_exposed;
    if (!safe && socket_path) {
        struct nr_gpu_packet packet;
        struct nr_gpu_reply reply;
        nr_gpu_bridge_packet(data->gpu, NR_GPU_RELEASE, &packet);
        safe = nr_gpu_rpc(socket_path, &packet, &reply) && reply.status == NR_GPU_SUCCESS;
    }
    if (!safe && data->gpu_peer)
        safe = WaitForSingleObject(data->gpu_peer, 0) == WAIT_OBJECT_0;
    if (!safe) {
        data->gpu_blocked = 1;
        fprintf(stderr, "[nr_layer] GPU transport fallback: export retained until daemon releases it or exits\n");
        return 0;
    }
    VkResult result = nr_gpu_bridge_close(data->gpu, NULL);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        data->device_error = VK_ERROR_DEVICE_LOST;
        return 0;
    }
    free(data->gpu);
    data->gpu = NULL;
    if (data->gpu_peer) CloseHandle(data->gpu_peer);
    data->gpu_peer = NULL;
    data->gpu_attached = data->gpu_exposed = 0;
    return 1;
}

/* 2: image already returned on GPU; -1: captured but no output, keep original;
 * 0: negotiation declined before taking waits, use the established host path. */
static int process_frame_gpu(struct device_data *data, struct swapchain_data *chain,
                             VkQueue queue, uint32_t index)
{
    if (!data->gpu_enabled || data->gpu_blocked || !socket_path ||
        data->frame_counter < data->gpu_retry_frame) return 0;
    if (async_live || live_every != 1 || ui_mask || capture_path) {
        if (!data->gpu_announced++)
            fprintf(stderr, "[nr_layer] GPU transport fallback: requires synchronous live every frame without mask/capture\n");
        return 0;
    }
    if (data->gpu && !nr_gpu_bridge_compatible(data->gpu, data->queue_family, queue,
        chain->extent.width, chain->extent.height, chain->format) && !release_gpu_bridge(data)) return 0;
    if (!data->gpu) {
        struct nr_gpu_bridge *bridge = calloc(1, sizeof *bridge);
        if (!bridge) return 0;
#define NR_GPU_INSTANCE(name) (PFN_vk##name)next_instance_proc(layer_instance, "vk" #name)
        VkResult result = nr_gpu_bridge_init(bridge, data->device, data->physical, data->queue_family,
            data->get_device_proc, NR_GPU_INSTANCE(GetPhysicalDeviceProperties2),
            NR_GPU_INSTANCE(GetPhysicalDeviceExternalBufferProperties),
            NR_GPU_INSTANCE(GetPhysicalDeviceExternalSemaphoreProperties),
            NR_GPU_INSTANCE(GetPhysicalDeviceMemoryProperties), NR_GPU_INSTANCE(GetPhysicalDeviceQueueFamilyProperties));
#undef NR_GPU_INSTANCE
        bridge->set_loader_data = data->set_loader_data;
        struct nr_gpu_packet hello;
        static volatile LONG64 sessions;
        LARGE_INTEGER ticks;
        QueryPerformanceCounter(&ticks);
        uint64_t session = (uint64_t)ticks.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32)
                           ^ (uint64_t)InterlockedIncrement64(&sessions);
        if (!session) session = 1;
        if (result == VK_SUCCESS)
            result = nr_gpu_bridge_configure(bridge, chain->extent.width, chain->extent.height,
                                             chain->format, session, &hello);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "[nr_layer] GPU transport fallback: %s (%d)\n",
                bridge->reason ? bridge->reason : "export creation failed", result);
            nr_gpu_bridge_close(bridge, NULL);
            free(bridge);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) data->gpu_retry_frame = data->frame_counter + 300;
            else data->gpu_blocked = 1;
            return 0;
        }
        data->gpu = bridge;
        data->gpu_exposed = 1;
        struct nr_gpu_reply reply;
        DWORD peer_pid = 0;
        int answered = nr_gpu_rpc_peer(socket_path, &hello, &reply, &peer_pid);
        if (peer_pid) data->gpu_peer = OpenProcess(SYNCHRONIZE, FALSE, peer_pid);
        if (!answered || reply.status != NR_GPU_SUCCESS) {
            if (answered && reply.status == NR_GPU_UNSUPPORTED) data->gpu_exposed = 0;
            fprintf(stderr, "[nr_layer] GPU transport fallback: daemon negotiation %s (%u)\n",
                answered ? "declined" : "failed", answered ? reply.detail : 0u);
            release_gpu_bridge(data);
            if (answered && reply.status == NR_GPU_UNSUPPORTED)
                data->gpu_retry_frame = data->frame_counter + 300;
            else data->gpu_blocked = 1;
            return 0;
        }
        data->gpu_attached = 1;
        fprintf(stderr, "[nr_layer] GPU transport ready: %ux%u format %d family %u daemon %lu sync=%s; pixels remain on GPU\n",
            bridge->width, bridge->height, bridge->format, bridge->family, (unsigned long)peer_pid,
            bridge->fenced ? "fence" : "semaphore");
    }
    struct nr_gpu_bridge *bridge = data->gpu;
    nr_timing_begin(&data->timing, (uint64_t)(uintptr_t)data->device,
        (uint64_t)chain->swapchain, chain->extent.width, chain->extent.height,
        data->queue_family, data->frame_counter, 0, 1);
    uint64_t began = data->timing.active ? nr_time_us() : 0;
    VkResult result = nr_gpu_bridge_capture(bridge, chain->images[index], queue,
        data->waits_consumed ? 0 : data->present_wait_count,
        data->waits_consumed ? NULL : data->present_wait);
    if (bridge->waits_consumed) data->waits_consumed = 1;
    if (data->timing.active) nr_timing_add(&data->timing, NR_TIME_CAPTURE, nr_time_us() - began);
    if (result != VK_SUCCESS) {
        data->gpu_blocked = 1;
        if (result == VK_ERROR_DEVICE_LOST || bridge->capture_pending)
            data->device_error = VK_ERROR_DEVICE_LOST;
        return -1;
    }
    struct nr_gpu_packet packet;
    struct nr_gpu_reply reply;
    nr_gpu_bridge_packet(bridge, NR_GPU_FRAME, &packet);
    began = data->timing.active ? nr_time_us() : 0;
    int answered = nr_gpu_rpc(socket_path, &packet, &reply);
    if (data->timing.active) nr_timing_add(&data->timing, NR_TIME_EXCHANGE, nr_time_us() - began);
    if (answered && reply.status == NR_GPU_SUCCESS) {
        began = data->timing.active ? nr_time_us() : 0;
        result = nr_gpu_bridge_copy_output(bridge, chain->images[index], queue);
        if (data->timing.active) nr_timing_add(&data->timing, NR_TIME_RETURN, nr_time_us() - began);
        if (result == VK_SUCCESS) return 2;
        /* An output submit may still own the swapchain image. Never forward an
         * unsynchronized present after that private fence failed. */
        data->device_error = VK_ERROR_DEVICE_LOST;
    } else {
        result = nr_gpu_bridge_wait(bridge, bridge->capture_fence, &bridge->capture_pending);
        if (result != VK_SUCCESS) data->device_error = VK_ERROR_DEVICE_LOST;
    }
    int retry_unsupported = answered && reply.status == NR_GPU_UNSUPPORTED;
    fprintf(stderr, "[nr_layer] GPU transport fallback: %s; original frame retained\n",
        retry_unsupported ? "frame settings unsupported" : "frame exchange failed");
    data->gpu_blocked = 1;
    if (release_gpu_bridge(data) && retry_unsupported && !data->device_error) {
        /* Dynamic settings can temporarily require the host path. A confirmed
         * capture completion and peer release let a future HELLO try again. */
        data->gpu_blocked = 0;
        data->gpu_retry_frame = data->frame_counter + 300;
    }
    return -1;
}
#endif

/* Everything this layer allocated per device, given back. The pointer to
 * `vkDestroyDevice` was being stored and never used: a staging buffer, its device memory
 * and its host mapping leaked on every device teardown, which a game that recreates its
 * device — a resolution change under some wrappers — does more than once. Ported from the
 * parallel ProjectsCodex tree. */
static int drain_private_submissions(struct device_data *data, const char *teardown)
{
	/* A failed private wait can make present report DEVICE_LOST without the
	 * driver having actually stopped. Drain those submissions once before
	 * freeing any child object or forwarding vkDestroyDevice. Keep the device
	 * and its occupied tracking slot if the real work still has not finished. */
	VkFence pending_fences[4];
	uint32_t pending_count = 0;
	if (data->transfer_fence && data->device_error)
		pending_fences[pending_count++] = data->transfer_fence;
	if (data->stalled_fence && data->stalled_fence != data->transfer_fence)
		pending_fences[pending_count++] = data->stalled_fence;
#ifdef _WIN32
	if (data->gpu) {
		if (data->gpu->capture_pending) pending_fences[pending_count++] = data->gpu->capture_fence;
		if (data->gpu->output_pending) pending_fences[pending_count++] = data->gpu->output_fence;
	}
#endif
	if (pending_count) {
		PFN_vkWaitForFences wait = (PFN_vkWaitForFences)
			data->get_device_proc(data->device, "vkWaitForFences");
		VkResult drained = wait ? wait(data->device, pending_count, pending_fences,
			VK_TRUE, UINT64_C(10000000000)) : VK_ERROR_INITIALIZATION_FAILED;
		if (drained != VK_SUCCESS && drained != VK_ERROR_DEVICE_LOST) {
			data->device_error = VK_ERROR_DEVICE_LOST;
#ifdef _WIN32
			data->gpu_blocked = 1;
#endif
			fprintf(stderr, "[nr_layer] %s teardown retained: %u private submissions remain unresolved (%d)\n",
				teardown, pending_count, drained);
			return 0;
		}
		if (drained == VK_ERROR_DEVICE_LOST) data->device_error = drained;
#ifdef _WIN32
		if (data->gpu) {
			data->gpu->capture_pending = data->gpu->output_pending = 0;
			if (drained == VK_ERROR_DEVICE_LOST) data->gpu->error = drained;
		}
#endif
	}
	return 1;
}

static int release_device(struct device_data *data)
{
	if (!drain_private_submissions(data, "device")) return 0;
#ifdef _WIN32
	if (!release_gpu_bridge(data) && data->gpu) {
		/* At device teardown all local submissions are now finished (or the
		 * driver confirmed loss). Peer imports own independent backing refs;
		 * their missing RELEASE reply alone cannot keep this local device alive. */
		fprintf(stderr, "[nr_layer] device teardown: closing drained local exports; daemon owns its imported references\n");
		nr_gpu_bridge_close(data->gpu, NULL);
		free(data->gpu);
		data->gpu = NULL;
		if (data->gpu_peer) CloseHandle(data->gpu_peer);
		data->gpu_peer = NULL;
		data->gpu_attached = data->gpu_exposed = 0;
	}
	nr_gpu_rpc_reap();
#endif
	drop_inflight(data);
	nr_timing_summary(&data->timing);
	if (data->mapped) {
		PFN_vkUnmapMemory unmap =
			(PFN_vkUnmapMemory)data->get_device_proc(data->device, "vkUnmapMemory");
		if (unmap) unmap(data->device, data->staging_memory);
	}
	if (data->staging) {
		PFN_vkDestroyBuffer destroy =
			(PFN_vkDestroyBuffer)data->get_device_proc(data->device, "vkDestroyBuffer");
		if (destroy) destroy(data->device, data->staging, NULL);
	}
	if (data->staging_memory) {
		PFN_vkFreeMemory release =
			(PFN_vkFreeMemory)data->get_device_proc(data->device, "vkFreeMemory");
		if (release) release(data->device, data->staging_memory, NULL);
	}
	release_transfer_resources(data);
	if (data->stalled_fence) {
        PFN_vkDestroyFence destroy = (PFN_vkDestroyFence)data->get_device_proc(data->device, "vkDestroyFence");
        destroy(data->device, data->stalled_fence, NULL);
    }
	nr_mutex_destroy(&data->present_lock);
	free(data->result);
	free(data->earlier);
	free(data->outgoing);
	return 1;
}

VKAPI_ATTR void VKAPI_CALL nr_DestroyDevice(VkDevice device,
					    const VkAllocationCallbacks *allocator)
{
	struct device_data *data = find_device(device);
	PFN_vkDestroyDevice next = data ? data->destroy_device : NULL;
	if (data) {
		if (!release_device(data)) return;
		nr_mutex_lock(&lock);
		for (int i = 0; i < MAX_SWAPCHAINS; i++)
			if (swapchains[i].device == device)
				memset(&swapchains[i], 0, sizeof swapchains[i]);
		for (int i = 0; i < MAX_QUEUES; i++)
			if (queues[i].device == device)
				memset(&queues[i], 0, sizeof queues[i]);
		nr_mutex_unlock(&lock);
		/* Before the memset, which would otherwise drop the pointer and leak the menu.
		 *
		 * `nr_menu_bridge_destroy` is what removes the live preview, and the preview is the
		 * daemon's view of what the user is trying RIGHT NOW - it has no meaning once this process
		 * is gone. Leaving it behind is what made a Save look like it had not happened: a Reset
		 * wrote the defaults there, the game exited with the panel open, and the next launch
		 * overrode the saved settings with those stale defaults. The destroy call was reachable
		 * only from the tests, so none of that cleanup ran in a real game - and the zeroing below
		 * threw the pointer away, so nothing could have run it afterwards either. */
		if (data->menu_bridge) {
			nr_menu_bridge_destroy(data->menu_bridge);
			data->menu_bridge = NULL;
		}
		/* The panel's Vulkan objects belong to this device and outlive every swapchain: its image,
		 * view and memory, the atlas and quad buffers with theirs, the backend's command buffer,
		 * and the two shader modules. NOTHING destroyed them - `nr_menu_create` and `nrex_create`
		 * had no matching call in the tree, and the `memset` below dropped both pointers - so
		 * validation's Object Tracking listed all fifteen at vkDestroyDevice. The bridge goes
		 * first because the panel reads its rows through it. */
		if (data->menu_ui) {
			nrex_destroy(data->menu_ui);
			data->menu_ui = NULL;
		}
		if (data->menu) {
			nr_menu_destroy(data->menu);
			data->menu = NULL;
		}
		memset(data, 0, sizeof *data);
	}
	if (next) next(device, allocator);
}

/* Without this the eight slots are consumed one per window resize, exclusive-fullscreen
 * toggle or HDR renegotiation, after which nothing is tracked and the photo mode goes
 * quiet. Worse, a driver may reuse a handle, and then `find_swapchain` would match a
 * stale entry whose VkImages are long destroyed. */
VKAPI_ATTR void VKAPI_CALL nr_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
						  const VkAllocationCallbacks *allocator)
{
	struct device_data *data = find_device(device);
    if (data) nr_mutex_lock(&data->present_lock);
    if (data && !drain_private_submissions(data, "swapchain")) {
        /* Pending capture/writeback commands may still name these VkImages.
         * Keep both the swapchain and its tracking entry until a later safe
         * teardown; a synthetic DEVICE_LOST does not cancel driver work. */
        nr_mutex_unlock(&data->present_lock);
        return;
    }
    if (data && data->result_chain == swapchain) {
#ifdef _WIN32
        release_gpu_bridge(data);
#endif
        data->result_chain = VK_NULL_HANDLE;
        data->holding = data->have_earlier = 0;
    }
	nr_mutex_lock(&lock);
	for (int i = 0; i < MAX_SWAPCHAINS; i++)
		if (swapchains[i].swapchain == swapchain)
			memset(&swapchains[i], 0, sizeof swapchains[i]);
	nr_mutex_unlock(&lock);
	if (data && data->destroy_swapchain)
		data->destroy_swapchain(device, swapchain, allocator);
    if (data) nr_mutex_unlock(&data->present_lock);
}

static uint32_t memory_type(struct device_data *data, uint32_t bits,
			    VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties properties;
	PFN_vkGetPhysicalDeviceMemoryProperties get =
		(PFN_vkGetPhysicalDeviceMemoryProperties)next_instance_proc(
			layer_instance, "vkGetPhysicalDeviceMemoryProperties");
	if (!get) return UINT32_MAX;
	get(data->physical, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
		if ((bits & (1u << i))
		    && (properties.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

/* Grow host-visible storage only after the previous private copy fence completed. */
static void release_staging(struct device_data *data)
{
    if (data->mapped) ((PFN_vkUnmapMemory)data->get_device_proc(data->device, "vkUnmapMemory"))(data->device, data->staging_memory);
    if (data->staging) ((PFN_vkDestroyBuffer)data->get_device_proc(data->device, "vkDestroyBuffer"))(data->device, data->staging, NULL);
    if (data->staging_memory) ((PFN_vkFreeMemory)data->get_device_proc(data->device, "vkFreeMemory"))(data->device, data->staging_memory, NULL);
    data->mapped = NULL;
    data->staging = VK_NULL_HANDLE;
    data->staging_memory = VK_NULL_HANDLE;
    data->staging_size = 0;
}

static int ensure_resources(struct device_data *data, VkDeviceSize needed)
{
    if (!host_pixel_path_allowed(data, "host capture allocation")) return -1;
	if (!data->pool) {
		VkCommandPoolCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = data->queue_family };
		PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)
			data->get_device_proc(data->device, "vkCreateCommandPool");
		if (create(data->device, &info, NULL, &data->pool) != VK_SUCCESS) return -1;
	}
	if (data->staging_size >= needed && data->result) {
        data->result_size = needed;
        return 0;
    }
    release_staging(data);
    data->holding = data->have_earlier = 0;

	PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)
		data->get_device_proc(data->device, "vkCreateBuffer");
	PFN_vkGetBufferMemoryRequirements requirements = (PFN_vkGetBufferMemoryRequirements)
		data->get_device_proc(data->device, "vkGetBufferMemoryRequirements");
	PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)
		data->get_device_proc(data->device, "vkAllocateMemory");
	PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)
		data->get_device_proc(data->device, "vkBindBufferMemory");
	PFN_vkMapMemory map = (PFN_vkMapMemory)
		data->get_device_proc(data->device, "vkMapMemory");
	VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				    .size = needed,
				    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
					     | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
	if (create_buffer(data->device, &info, NULL, &data->staging) != VK_SUCCESS) return -1;
	VkMemoryRequirements mr;
	requirements(data->device, data->staging, &mr);
	uint32_t type = memory_type(data, mr.memoryTypeBits,
				    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
				    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
				    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (type == UINT32_MAX)
		type = memory_type(data, mr.memoryTypeBits,
				   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
				   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX) { release_staging(data); return -1; }
	VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					    .allocationSize = mr.size, .memoryTypeIndex = type };
	if (allocate(data->device, &allocation, NULL, &data->staging_memory) != VK_SUCCESS) {
        release_staging(data); return -1;
    }
    if (bind(data->device, data->staging, data->staging_memory, 0) != VK_SUCCESS ||
        map(data->device, data->staging_memory, 0, VK_WHOLE_SIZE, 0, &data->mapped) != VK_SUCCESS) {
        release_staging(data); return -1;
    }
	data->staging_size = needed;
	free(data->result);
	data->result = malloc((size_t)needed);
	data->result_size = needed;
	return data->result ? 0 : -1;
}

/* All copy submissions wait only on the current present's semaphores and on their
 * own fence. No foreign queue is touched and no semaphore is recycled behind WSI.
 * The synchronous daemon already requires host readback; completing writeback too
 * keeps the lifetime contract explicit until an asynchronous path is separately proven. */
static int transfer(struct device_data *data, struct swapchain_data *chain, VkQueue queue,
                    uint32_t index, int to_image)
{
    if (!host_pixel_path_allowed(data, "host capture/writeback")) return -1;
    if (data->device_error || data->transfer_error) return -1;
#define FN(type, name) type name = (type)data->get_device_proc(data->device, "vk" #name)
    FN(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers);
    FN(PFN_vkBeginCommandBuffer, BeginCommandBuffer);
    FN(PFN_vkEndCommandBuffer, EndCommandBuffer);
    FN(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier);
    FN(PFN_vkCmdCopyImageToBuffer, CmdCopyImageToBuffer);
    FN(PFN_vkCmdCopyBufferToImage, CmdCopyBufferToImage);
    FN(PFN_vkQueueSubmit, QueueSubmit);
    FN(PFN_vkWaitForFences, WaitForFences);
    FN(PFN_vkCreateFence, CreateFence);
    FN(PFN_vkResetFences, ResetFences);
    FN(PFN_vkResetCommandBuffer, ResetCommandBuffer);
#undef FN
    uint64_t transfer_started = data->timing.active ? nr_time_us() : 0;
    VkCommandBuffer commands = data->transfer_commands;
    VkFence fence = data->transfer_fence;
    VkResult r;
    if (!commands) {
        VkCommandBufferAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = data->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        r = AllocateCommandBuffers(data->device, &alloc, &commands);
        if (r != VK_SUCCESS) goto failed;
        if (data->set_loader_data &&
            (r = data->set_loader_data(data->device, commands)) != VK_SUCCESS) {
            release_transfer_resources(data); /* Reclaim this unsubmitted allocation before retry. */
            goto failed;
        }
        data->transfer_commands = commands;
    } else {
        /* The only way here follows a successful fence wait. A failed wait
         * latches device_error and returns before any future reset or recording. */
        r = ResetCommandBuffer(commands, 0);
        if (r != VK_SUCCESS) goto failed;
    }
    if (!fence) {
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        r = CreateFence(data->device, &fi, NULL, &fence);
        if (r != VK_SUCCESS) goto failed;
        data->transfer_fence = fence;
    } else {
        r = ResetFences(data->device, 1, &fence);
        if (r != VK_SUCCESS) goto failed;
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r = BeginCommandBuffer(commands, &bi);
    if (r != VK_SUCCESS) goto failed;
    VkImageLayout working = to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                    : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier into = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = to_image ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = working,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = chain->images[index],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &into);
    VkBufferMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = data->staging, .offset = 0, .size = VK_WHOLE_SIZE };
    if (to_image)
        CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &host, 0, NULL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { chain->extent.width, chain->extent.height, 1 } };
    if (to_image) CmdCopyBufferToImage(commands, data->staging, chain->images[index], working, 1, &region);
    else CmdCopyImageToBuffer(commands, chain->images[index], working, data->staging, 1, &region);
    VkImageMemoryBarrier back = into;
    back.srcAccessMask = into.dstAccessMask;
    back.dstAccessMask = 0;
    back.oldLayout = working;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    if (!to_image) {
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
    }
    r = EndCommandBuffer(commands);
    if (r != VK_SUCCESS) goto failed;
    uint32_t waits = data->present_wait_count;
    if (waits > data->transfer_stage_capacity) {
        VkPipelineStageFlags *grown = realloc(data->transfer_stages,
            (size_t)waits * sizeof *grown);
        if (!grown) { r = VK_ERROR_OUT_OF_HOST_MEMORY; goto failed; }
        data->transfer_stages = grown;
        data->transfer_stage_capacity = waits;
        for (uint32_t i = 0; i < waits; i++) grown[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = waits, .pWaitSemaphores = waits ? data->present_wait : NULL,
        .pWaitDstStageMask = waits ? data->transfer_stages : NULL,
        .commandBufferCount = 1, .pCommandBuffers = &commands };
    r = QueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) goto failed;
    data->waits_consumed = 1;
    data->present_wait_count = 0;
    uint64_t wait_started = data->timing.active ? nr_time_us() : 0;
    r = WaitForFences(data->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (data->timing.active) {
        nr_timing_add(&data->timing, to_image ? NR_TIME_WRITEBACK_WAIT : NR_TIME_READBACK_WAIT,
            nr_time_us() - wait_started);
        nr_timing_add(&data->timing, to_image ? NR_TIME_RETURN : NR_TIME_CAPTURE,
            nr_time_us() - transfer_started);
    }
    if (r != VK_SUCCESS) {
        /* Keep potentially pending commands/fence until device teardown. Never recycle
         * their memory after an unsuccessful wait, or continue presenting this device. */
        data->device_error = r;
        return -1;
    }
    return 0;
failed:
    data->transfer_error = r;
    if (r == VK_ERROR_DEVICE_LOST) data->device_error = r;
    fprintf(stderr, "[nr_layer] transfer failed (%d)\n", (int)r);
    return -1;
}

/* Which pixels are the interface.
 *
 * The shipped feature never has to ask: it inserts the pass before the interface is
 * drawn ("UI remains downstream"). A layer at `vkQueuePresentKHR` sees the composed
 * frame and has to work it out, and the one signal available is motion — an interface
 * holds still while the scene under it does not.
 *
 * That signal cannot separate an interface over a still scene from a still scene, so
 * when almost nothing moved the mask is refused rather than guessed: `settled` returns
 * 0 and the frame is sent unmasked, which is what a photo mode of a paused scene wants.
 */
static uint32_t settled(const unsigned char *now, const unsigned char *before,
			uint32_t pixels, unsigned char *mask)
{
	uint32_t held = 0;
	for (uint32_t i = 0; i < pixels; i++) {
		const unsigned char *a = now + 4 * i, *b = before + 4 * i;
		int moved = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 6;
		mask[i] = moved ? 0u : 0xFFu;
		held += !moved;
	}
	return held;
}

/* Whether a mask is worth sending at all. Over nine tenths held still means nothing
 * moved, so an interface cannot be told from a still scene and the mask would cover the
 * whole frame. Under a fiftieth means there is no interface worth a second plane on the
 * wire. Between them the signal is real. */
static int mask_worth_sending(uint32_t held, uint32_t pixels)
{
	return held < (uint32_t)((uint64_t)pixels * 9 / 10) && held > pixels / 50;
}

static int process_frame(struct device_data *data, struct swapchain_data *chain,
			 VkQueue queue, uint32_t index)
{
    if (!host_pixel_path_allowed(data, "host frame processing")) return -1;
	nr_timing_begin(&data->timing, (uint64_t)(uintptr_t)data->device,
		(uint64_t)chain->swapchain, chain->extent.width, chain->extent.height,
		data->queue_family, data->frame_counter, 0, 0);
	VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
	if (ensure_resources(data, needed)) return -1;
	if (transfer(data, chain, queue, index, 0)) return -1;

	uint32_t pixels = chain->extent.width * chain->extent.height;
	int masked = 0;
	if (ui_mask && data->have_earlier && data->earlier_size >= needed) {
		if (data->outgoing_size < needed + pixels) {
			unsigned char *grown = realloc(data->outgoing, (size_t)needed + pixels);
			if (!grown) return -1;
			data->outgoing = grown;
			data->outgoing_size = needed + pixels;
		}
		memcpy(data->outgoing, data->mapped, (size_t)needed);
		uint32_t held = settled(data->mapped, data->earlier, pixels,
					data->outgoing + needed);
		masked = mask_worth_sending(held, pixels);
		if (!data->timing.enabled)
			fprintf(stderr, "[nr_layer] %u%% of the frame held still; ui mask %s\n",
				100u * held / pixels, masked ? "sent" : "refused");
	}

	uint32_t header[4] = { masked ? 0x314E524Eu : 0x304E524Eu, chain->extent.width,
			       chain->extent.height, (uint32_t)chain->format };
	if (capture_path) {
		FILE *file = fopen(capture_path, "wb");
		if (file) {
			fwrite(header, sizeof header, 1, file);
			fwrite(data->mapped, 1, (size_t)needed, file);
			fclose(file);
		}
	}
	if (!socket_path) return -1;
	/* The mask travels immediately after the colour, one byte a pixel. */
	if (masked) {
		uint64_t began = data->timing.active ? nr_time_us() : 0;
		int failed = exchange(header, sizeof header, data->outgoing,
			     (size_t)needed + pixels, data->result, (size_t)needed);
		if (data->timing.active) nr_timing_add(&data->timing, NR_TIME_EXCHANGE, nr_time_us() - began);
		if (failed) {
			fprintf(stderr, "[nr_layer] the daemon did not answer; frame unchanged\n");
			return -1;
		}
		memcpy(data->mapped, data->result, (size_t)needed);
		if (!data->timing.enabled)
			fprintf(stderr, "[nr_layer] processed %ux%u with a ui mask\n",
				chain->extent.width, chain->extent.height);
		return 0;
	}
	uint64_t began = data->timing.active ? nr_time_us() : 0;
	int failed = exchange(header, sizeof header, data->mapped, (size_t)needed, data->result,
		     (size_t)needed);
	if (data->timing.active) nr_timing_add(&data->timing, NR_TIME_EXCHANGE, nr_time_us() - began);
	if (failed) {
		fprintf(stderr, "[nr_layer] the daemon did not answer; frame unchanged\n");
		return -1;
	}
	memcpy(data->mapped, data->result, (size_t)needed);
	if (!data->timing.enabled)
		fprintf(stderr, "[nr_layer] processed %ux%u\n", chain->extent.width,
			chain->extent.height);
	return 0;
}

/* NR_LAYER_ASYNC: the frame goes out and the answer for the one before it comes back, so
 * the daemon works while the game draws its next frame instead of the game waiting for the
 * daemon; what is on screen is one processed present behind. The old answer is read before
 * the new frame is sent. The other order deadlocks: an answer is larger than a socket's
 * buffer, and the daemon, which takes one connection at a time, cannot accept the new frame
 * until the old answer has been read. */
static int process_frame_async(struct device_data *data, struct swapchain_data *chain,
			       VkQueue queue, uint32_t index)
{
    if (!host_pixel_path_allowed(data, "host asynchronous processing")) return -1;
	nr_timing_begin(&data->timing, (uint64_t)(uintptr_t)data->device,
		(uint64_t)chain->swapchain, chain->extent.width, chain->extent.height,
		data->queue_family, data->frame_counter, 1, 0);
	VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
	if (ensure_resources(data, needed) || transfer(data, chain, queue, index, 0)) {
		drop_inflight(data);
		return -1;
	}
	int answered = 0;
	uint64_t exchange_started = data->timing.active ? nr_time_us() : 0;
	if (data->inflight && data->inflight_size == needed) {
		data->inflight = 0;
		answered = exchange_receive(data->inflight_fd, data->result, (size_t)needed) == 0;
		if (answered && data->inflight_timing_started &&
		    data->inflight_timing_group == data->timing.group) {
			/* Attach wall latency to the previous sampled request. Current request
			 * timing only describes work blocking this present, not the old frame. */
			struct nr_timing_sample *previous = &data->timing.samples[data->inflight_timing_slot];
			previous->us[NR_TIME_PREVIOUS_RESPONSE] = nr_time_us() - data->inflight_timing_started;
			previous->valid |= 1u << NR_TIME_PREVIOUS_RESPONSE;
			data->timing.since_summary++;
		}
		data->inflight_timing_started = 0;
		if (!answered)
			fprintf(stderr, "[nr_layer] the daemon did not answer; frame unchanged\n");
	} else {
		drop_inflight(data);          /* the extent changed under it: it fits nothing */
	}
	if (socket_path) {
		uint32_t header[4] = { 0x304E524Eu, chain->extent.width, chain->extent.height,
				       (uint32_t)chain->format };
		int fd = exchange_send(header, sizeof header, data->mapped, (size_t)needed);
		if (fd >= 0) {
			data->inflight = 1;
			data->inflight_fd = fd;
			data->inflight_size = needed;
			if (data->timing.active) {
				data->inflight_timing_started = data->timing.started;
				data->inflight_timing_slot = data->timing.cursor;
				data->inflight_timing_group = data->timing.group;
			}
		}
	}
	if (data->timing.active)
		nr_timing_add(&data->timing, NR_TIME_EXCHANGE, nr_time_us() - exchange_started);
	if (!answered) return -1;
	memcpy(data->mapped, data->result, (size_t)needed);
	return 0;
}

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
static void *menu_instance_resolver(void *handle, const char *name)
{
	(void)handle;
	if (!next_instance_proc) return NULL;
	return (void *)next_instance_proc(layer_instance, name);
}

/* The chain this device is presenting on. A device has one or two; the first is the one the
 * game is drawing to, and the menu only ever needs the extent and the image to write. */
static struct swapchain_data *chain_for(struct device_data *data)
{
	for (int i = 0; i < MAX_SWAPCHAINS; ++i)
		if (swapchains[i].swapchain && swapchains[i].device == data->device)
			return &swapchains[i];
	return NULL;
}

/* The menu is opt-out: NR_MENU=0 for a game where even one GetAsyncKeyState a frame is unwelcome,
 * and for anyone bisecting a problem they suspect is ours. Read once. */
static int menu_enabled(void)
{
	const char *off = getenv("NR_MENU");
	return !(off && strcmp(off, "0") == 0);
}

/* Build the menu and its bridge, on the first present that can carry them.
 *
 * Fails soft in every direction: a swapchain that did not take STORAGE, a format that cannot be a
 * storage image, no font, no shader - each sets `menu_state` to -1 with a reason, the message goes
 * to stderr once, and every later call returns on the first branch. A game with no menu is the
 * game it was. */

/* The image count the UI's backend wants. A swapchain's minimum is not tracked separately - the
 * actual count is always >= it - so the one number this file has is the one it hands over. */
static void *g_instance_for_ui(void) { return (void *)layer_instance; }

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

static uint32_t chain_image_count(struct swapchain_data *c)
{
	return c && c->image_count ? c->image_count : 2;
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
		data->menu_ui = nrex_create(data->device, data->physical, (void *)layer_instance,
					    (void *)ui_queue, ui_family_out,
					    (uint32_t)chain->format, 420u << 12,
					    chain_image_count(chain), chain_image_count(chain),
					    (void *)(uintptr_t)data->get_device_proc,
					    (void *)(uintptr_t)next_instance_proc,
					    font, font_size, codepoints, codepoint_count, &ui_why);
		/* NOT freed here. AddFontFromMemoryTTF adopts the buffer (FontData with
		 * FontDataOwnedByAtlas left true) and frees it when the atlas is destroyed - freeing it
		 * on this side too was a double free, which is the access violation this menu hit the
		 * moment a font was supplied. Ownership transferred, so the pointer is not touched again;
		 * if the UI never asked for it, the buffer is freed here instead. */
		if (!data->menu_ui) free(font);
		/* next_instance_proc needs the instance; g_instance_for_ui hands it over inside the
		 * resolver the UI was given, so the two together reach the instance-level commands. */
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
#define MENU_UI_MAX_ROWS 24
/* The widest option list any knob has, with room to spare. */
#define MENU_UI_MAX_CHOICES 8

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
				nr_menu_bridge_knob_choice_name(i, c, name_storage[i][c],
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
	return n;
}

/* The keyboard, swept whole.
 *
 * This walked a hand-written list - the arrows, Enter, Escape and the five letters the old menu
 * bound. Digits were not in it, so `nrex_key` was never called for `0`..`9`, `ToUnicode` never ran
 * for them, and the field the panel offers showed a caret and refused every keystroke. A platform
 * backend receives every key and lets the UI decide; a key ImGui has no use for is dropped inside
 * `nrex_key`, which costs one dispatch per key and nothing else. */
static void menu_ui_keys(struct device_data *data)
{
	if (!data->menu_ui) return;
	for (int vk = 0; vk < 256; ++vk)
		nrex_key(data->menu_ui, vk, (GetAsyncKeyState(vk) & 0x8000) != 0);
}

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
 * sibling window - the same reason `window_is_ours` exists. */
static int window_is_foreground(HWND hwnd)
{
	return nr_window_is_foreground((void *)hwnd);
}

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
static int window_is_ours(HWND h)
{
	return nr_window_is_ours((void *)h);
}

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

static HWND window_for_thread_for_ui(void)
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

/* The mouse, in the menu's own coordinate space.
 *
 * The screen position is converted to the game's client area and scaled to the swapchain extent,
 * because ImGui draws in image pixels: passing raw screen coordinates would put every click
 * somewhere other than the pointer. The layer never captures or hides the cursor - it is a guest in
 * the game's window and the game keeps its input.
 *
 * `io.AddMouse*` rather than assigning the fields: ImGui consumes queued input once a frame, and
 * writing the fields directly is documented as wrong for anything but a fixed test harness. */
static void menu_ui_mouse(struct device_data *data, struct swapchain_data *chain)
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
	HWND hwnd = window_for_thread_for_ui();
	if (!hwnd) UI_MOUSE_STOP(2, "no window for this process");
	if (!window_is_foreground(hwnd)) {
		/* "No position" rather than the last one: leaving a stale position would let a click that
		 * arrives on the very frame focus is regained land on whatever was under the pointer when
		 * the user left. The modifiers are cleared for the same reason - the user may have held
		 * Ctrl+Tab in the other window and released it there. */
		nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
		nrex_set_software_cursor(data->menu_ui, 0);
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
	float x = ((float)screen.x - (float)origin.x) * sx;
	float y = ((float)screen.y - (float)origin.y) * sy;

	int down = (GetAsyncKeyState(0x01) & 0x8000) != 0;
	nrex_mouse(data->menu_ui, x, y, down);

	/* And whether the menu has to draw the pointer itself.
	 *
	 * The game owns the OS cursor: ShowCursor is a per-process counter it can leave off, and a
	 * fullscreen title usually paints its own or confines the hardware one. Neither is something a
	 * layer can undo, so when Windows says the cursor is not showing, the panel draws one. Asked
	 * every frame because the game shows and hides it as it likes.
	 *
	 * The hint goes with it, and it is not the same thing as the position above: that one is
	 * "no position" outside the image, so a widget is never hovered from off-panel. ImGui draws its
	 * cursor only when it HAS a position, so without the hint the fallback arrow appeared only over
	 * the panel - the one place the user is not looking for it. */
	CURSORINFO ci;
	ci.cbSize = sizeof ci;
	int cursor_visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
	nrex_set_software_cursor(data->menu_ui, !cursor_visible);
	if (!cursor_visible)
		nrex_cursor_hint(data->menu_ui, x, y);
}

static int menu_read_keys(struct device_data *data)
{
	if (!data->menu) return 0;

	/* The keyboard is system-wide too, so the ImGui feed gets the same gate as the pointer.
	 *
	 * Without this, typing in any other window while the game sat behind it pressed keys in the
	 * panel: an `L` in a chat box cycled the language, an arrow in a document moved a slider.
	 *
	 * **Only the ImGui feed is gated.** INS, L and the NR toggle are the LAYER's own hotkeys, read
	 * further down, and they have to keep working - opening the panel is what the user does when the
	 * game is NOT in front, and the NR toggle is meant to work over the game while another window
	 * has focus. Returning early here skipped all of them, which made the panel impossible to open;
	 * feeding "all keys up" instead is what the ImGui path needs, and it leaves the hotkeys alone.
	 *
	 * The sweep is skipped rather than fed as releases for the same reason: 256 release events are
	 * not a state change ImGui needs, and `nrex_mouse` already reports the pointer gone. */
	int foreground = window_is_foreground(window_for_thread_for_ui());
	if (data->menu_ui) {
		if (foreground) {
			menu_ui_keys(data);
		} else {
			/* Clear whatever the panel was holding, so nothing stays pressed or dragged while the
			 * user is working in another window. */
			nrex_mouse(data->menu_ui, -1.0f, -1.0f, 0);
		}
	}

	/* L cycles the language. The combo in the panel's footer sets it too, and both go through
	 * `nr_menu_inject_language`, so the panel and the settings file cannot disagree - the
	 * injector raises the same flag the bridge already watches, which re-translates every row
	 * and rewrites the file. There is no second path that could translate differently.
	 *
	 * Edge-triggered here because this is a level poll: without the latch a held L would step the
	 * language once per presented frame and blur through all four in a fraction of a second. */
	if (data->menu && data->menu_ui) {
		int down = (GetAsyncKeyState('L') & 0x8000) != 0;
		if (down && !data->menu_ui_l_was_down)
			nr_menu_inject_language(data->menu,
						(nr_menu_language(data->menu) + 1) % NR_MENU_LANG_COUNT);
		data->menu_ui_l_was_down = down;
	}

	/* INS opens and closes the panel.
	 *
	 * `nr_menu.c`'s own reader does this for the hand-drawn menu, and the ImGui build switches that
	 * reader OFF so the two do not both act on one keypress - which left INS bound to nothing at
	 * all and the panel unreachable without `NR_MENU_OPEN`. The key is the panel's only way in, so
	 * the ImGui path has to own it here.
	 *
	 * Edge-triggered for the same reason as L: this is a level poll, and a held INS would toggle the
	 * panel once per presented frame. */
	if (data->menu_ui) {
		int down = (GetAsyncKeyState(0x2D) & 0x8000) != 0;
		if (down && !data->menu_ui_ins_was_down)
			nr_menu_inject_open(data->menu, !nr_menu_is_open(data->menu));
		data->menu_ui_ins_was_down = down;
	}

	if (data->menu_bridge) {
		int state = nr_menu_bridge_update(data->menu_bridge, 1.0f / 60.0f);
		if (state == 2)
			fprintf(stderr, "[nr_layer] settings menu %s\n",
				nr_menu_is_open(data->menu) ? "open" : "closed");
	} else {
		nr_menu_update(data->menu, 1.0f / 60.0f);
	}

	/* The toggle moves the trigger the layer already reads every frame. The menu reports the
	 * press and this is what acts on it, because the trigger is the layer's. */
	if (nr_menu_take_toggle_request(data->menu) && trigger_path && *trigger_path) {
		if (access(trigger_path, F_OK) == 0) {
			remove(trigger_path);
		} else {
			FILE *f = fopen(trigger_path, "wb");
			if (f) fclose(f);
		}
	}

	/* Tell the menu whether NR is on, so its status line is the truth rather than a guess. */
	nr_menu_set_nr_enabled(data->menu,
			       trigger_path && access(trigger_path, F_OK) == 0);

	{
		int need = nr_menu_needs_composite(data->menu);
		return need;
	}
}

/* The menu's own command buffer, its fence, and one submit. See the note above on why it owns no
 * semaphores.
 *
 * Returns 1 when the frame was drawn, 0 when there was nothing to record or the recording failed -
 * in which case the frame presents without a menu, which is a missing menu rather than a lost
 * game. */
static int menu_composite(struct device_data *data, struct swapchain_data *chain, VkQueue queue,
                          uint32_t index)
{
	if (!data->menu || data->device_error || data->transfer_error) return 0;
	if (index >= chain->image_count) return 0;
	if (!chain->storage_ok) return 0;

#define FN(type, name) type name = (type)data->get_device_proc(data->device, "vk" #name)
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
		VkCommandPoolCreateInfo pci = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
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

	/* The comparison first, so the panel sits over it. The reference is the layer's staging
	 * buffer, which holds the game's own frame from this present - which is what makes the wipe a
	 * comparison of two frames rather than a cut in one. */
	if (nr_menu_compare_mode(data->menu) != NR_MENU_COMPARE_OFF) {
		/* The staging buffer is what the layer copies the game's frame into, so it holds the
		 * picture before NR wrote its result back - which is what makes the wipe a comparison of
		 * two frames rather than a cut in one. Zero when there is no staging buffer, and the menu
		 * then cuts the image against itself. */
		uint64_t reference = (data->staging && data->mapped)
			? (uint64_t)(uintptr_t)data->staging : 0;
		nr_menu_draw_compare(data->menu, (uint64_t)(uintptr_t)data->menu_commands,
				     (uint64_t)(uintptr_t)chain->images[index], reference,
				     chain->extent.width, chain->extent.height);
	}
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
		menu_ui_mouse(data, chain);
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
			static int have_frequency;
			static LARGE_INTEGER cached;
			LARGE_INTEGER counter;
			double now = 0.0;
			if (!have_frequency) {
				QueryPerformanceFrequency(&cached);
				have_frequency = 1;
			}
			if (QueryPerformanceCounter(&counter) && cached.QuadPart > 0)
				now = (double)counter.QuadPart / (double)cached.QuadPart;
			if (data->menu_ui_last_draw != 0.0 && now > data->menu_ui_last_draw) {
				double elapsed = now - data->menu_ui_last_draw;
				if (elapsed < 0.000001) elapsed = 0.000001;
				if (elapsed > 0.25) elapsed = 0.25;
				dt = (float)elapsed;
			}
			/* Held across frames, and left alone when the counter is unavailable so a machine without
			 * one falls back to the fixed step above rather than moving nothing. */
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
		nrex_begin_panel(data->menu_ui, "DLSS-NR on Intel", nr_menu_language(data->menu));
		nrex_rows(data->menu_ui, rows, count, nr_menu_selected(data->menu));
		int acted = nrex_end_panel(data->menu_ui);

		/* Apply what the user moved. The UI reports intent; the bridge owns the knob's range, its
		 * step, the settings write and the daemon's notification - so an edit goes through the same
		 * path an arrow key takes, and the two cannot disagree about what a knob accepts. */
		for (int i = 0; i < count; ++i) {
			if (!rows[i].changed) continue;
			float v = (rows[i].kind == NREX_KIND_FLOAT) ? rows[i].value_f
								    : (float)rows[i].value_i;
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
	} else if (nr_menu_is_open(data->menu))
		nr_menu_draw(data->menu, (uint64_t)(uintptr_t)data->menu_commands,
			     (uint64_t)(uintptr_t)chain->images[index],
			     chain->extent.width, chain->extent.height);

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

/* A completed private fence covers the consumed waits and all copies. Forward the
 * original wait list only when no copy submission has taken ownership of it. */
static VkResult present_now(struct device_data *data, VkQueue queue,
                            const VkPresentInfoKHR *info)
{
    if (data->device_error) return data->device_error;
    if (data->transfer_error) return data->transfer_error;

    /* The in-game menu. Two separate costs, deliberately split:
     *
     *   the keys   every frame, no GPU work - the NR toggle and the compare key act with the menu
     *              closed, and a toggle that only worked while the menu was open would be useless
     *   the draw   only when something is on screen. `nr_menu_needs_composite` is the test, and
     *              for ordinary play it is false, so no command buffer is opened and nothing is
     *              submitted.
     *
     * The menu is built here, lazily, on the first present of a swapchain that can carry it. It
     * cannot be built earlier: this is the first moment the layer has the device, the queue family
     * and the swapchain format together. A failure sets `menu_state` to -1 with a reason, so a
     * surface that cannot carry a menu is decided once rather than retried every frame.
     *
     * This call is what makes the menu exist. Without it `data->menu` is provably NULL at every
     * read below, and the compiler - correctly - deletes the whole overlay: the first build of this
     * had `menu_build` defined and never called, and the resulting DLL contained none of the
     * menu's strings. A build that succeeds is not a menu that runs. */
    if (!data->menu && !data->menu_state && menu_enabled()
        && info->swapchainCount > 0 && info->pSwapchains) {
        struct swapchain_data *first = find_swapchain(info->pSwapchains[0]);
        if (first && first->device == data->device)
            menu_build(data, first);
    }

    if (data->menu && info->swapchainCount > 0 && info->pSwapchains && info->pImageIndices) {
        struct swapchain_data *chain = find_swapchain(info->pSwapchains[0]);
        if (chain && chain->device == data->device && menu_read_keys(data))
            menu_composite(data, chain, queue, info->pImageIndices[0]);
    } else if (data->menu) {
        /* No swapchain to draw into - a present-only queue, or a surface the layer never
         * tracked. The keys still work, so the toggle and the compare keep responding. */
        menu_read_keys(data);
    }

    if (!data->waits_consumed) return data->present(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 0;
    patched.pWaitSemaphores = NULL;
    return data->present(queue, &patched);
}

static VkResult present_locked(VkQueue queue,
						  const VkPresentInfoKHR *info)
{
	struct device_data *data = NULL;
	/* Resolve the queue rather than taking the first live device: with two devices the
	 * first is not necessarily this one, and the family decides which pool is legal. */
	struct queue_data *owner = find_queue(queue);
	if (owner && !owner->capture_ok) {
		/* A present-only or protected queue: nothing can be recorded on it. Find the
		 * device only to reach its `present` pointer. */
		struct device_data *host = find_device(owner->device);
		if (host) return host->present(queue, info);
	}
	if (owner) {
		data = find_device(owner->device);
		if (data && data->queue_family != owner->family) {
#ifdef _WIN32
			release_gpu_bridge(data);
#endif
			data->queue_family = owner->family;
			/* Cached commands belong to this family. All prior transfers completed
			 * their fence; otherwise nr_QueuePresentKHR rejects this device. */
			release_transfer_resources(data);
		}
	}
	if (!data)
		for (int i = 0; i < 8; i++) if (devices[i].device) { data = &devices[i]; break; }
	if (!data) return VK_ERROR_INITIALIZATION_FAILED;
	data->frame_counter++;
	/* Mark all surfaces in this present before choosing. A service window can
	 * present alone without displacing a recently active main game window. */
	struct nr_surface_record records[MAX_SWAPCHAINS] = {{0}};
	nr_mutex_lock(&lock);
	for (uint32_t i = 0; i < info->swapchainCount; ++i) {
		struct swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
		if (chain && chain->device == data->device)
			nr_surface_mark_presented(&chain->surface, data->frame_counter);
	}
	for (size_t i = 0; i < MAX_SWAPCHAINS; ++i)
		if (swapchains[i].device == data->device) records[i] = swapchains[i].surface;
	nr_mutex_unlock(&lock);
	const char *preferred = getenv("NR_LAYER_SWAPCHAIN");
	uint64_t preferred_handle = preferred ? strtoull(preferred, NULL, 0) : 0;
	struct nr_surface_choice primary = nr_surface_choose(records, MAX_SWAPCHAINS,
		data->frame_counter, 120, data->primary, preferred_handle);
	if (primary.changed) {
#ifdef _WIN32
		release_gpu_bridge(data);
		data->gpu_retry_frame = 0;
#endif
		data->holding = data->have_earlier = 0;
		drop_inflight(data);
		data->primary = primary.selection;
		fprintf(stderr, "[nr_layer] primary swapchain %llx %ux%u (%s)\n",
			(unsigned long long)primary.selection.handle, primary.selection.width,
			primary.selection.height, primary.reason == NR_SURFACE_EXPLICIT ? "selected" : "largest recent");
	}
	if (!data->queues_announced) {
		data->queues_announced = 1;
		announce_queues(data->device, queue, owner ? owner->family : data->queue_family);
	}

	/* Offered, not yet taken: a transfer claims them, and if none runs this present goes
	 * through untouched with its own. */
	data->present_wait = info->pWaitSemaphores;
	data->present_wait_count = info->waitSemaphoreCount;
	data->waits_consumed = 0;
	data->transfer_error = VK_SUCCESS;

	/* Live mode is a slideshow rather than a photo: every Nth present goes through the
	 * network and the frames between re-blit the last result, so the picture is steady
	 * instead of alternating with the game's own. The trigger file is not consulted —
	 * an `access()` per present is a syscall the hot path does not need — and neither
	 * is the interface mask, whose detector is built around a frame that was asked for.
	 *
	 * The rate this can hold is set by the daemon's `--render-scale`, not by N: the
	 * network's cost follows the extent it is given (notes/phase37). N only decides how
	 * many game frames each rendered one covers. */
	if (live_every > 0) {
		if (gpu_only_mode()) data->holding = 0;
		/* The trigger keeps its meaning — "do the thing" — so the effect can be
		 * turned on and off mid-game without restarting it. With no trigger
		 * configured, live mode simply always runs. */
		int on = !trigger_path || access(trigger_path, F_OK) == 0;
		if (!on) {
			data->holding = 0;
			drop_inflight(data);
#ifdef _WIN32
			if (!data->gpu_blocked) release_gpu_bridge(data);
#endif
		}
		for (uint32_t i = 0; on && i < info->swapchainCount; i++) {
			if (data->device_error || data->transfer_error) return present_now(data, queue, info);
			struct swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
			if (!chain || chain->device != data->device ||
			    (uint64_t)chain->swapchain != data->primary.handle ||
			    info->pImageIndices[i] >= chain->image_count) continue;
            if (data->result_chain != chain->swapchain) {
                data->holding = data->have_earlier = 0;
                data->result_chain = chain->swapchain;
                drop_inflight(data);
            }
			uint32_t index = info->pImageIndices[i];
			VkDeviceSize want = (VkDeviceSize)chain->extent.width
					  * chain->extent.height * 4;
			if (data->frame_counter % (unsigned long)live_every == 0) {
				int done;
#ifdef _WIN32
				int gpu = process_frame_gpu(data, chain, queue, index);
				if (gpu) {
					data->holding = 0;
					nr_timing_finish(&data->timing, gpu == 2);
					continue;
				}
#endif
				if (!host_pixel_path_allowed(data, "shared processing unavailable")) {
					data->holding = 0;
					continue;
				}
				done = async_live ? process_frame_async(data, chain, queue, index)
						      : process_frame(data, chain, queue, index);
				if (done == 0) {
					data->holding = 1;
					int returned = transfer(data, chain, queue, index, 1) == 0;
					nr_timing_finish(&data->timing, returned);
				} else {
					data->holding = 0; /* never replay a partially received reply */
					nr_timing_finish(&data->timing, 0);
				}
			} else if (!gpu_only_mode() && data->holding && data->result_size == want) {
				memcpy(data->mapped, data->result, (size_t)data->result_size);
				transfer(data, chain, queue, index, 1);
			}
		}
		return present_now(data, queue, info);
	}
	if (!host_pixel_path_allowed(data, "photo/capture mode requires host pixels")) {
		data->holding = data->have_earlier = 0;
		drop_inflight(data);
		return present_now(data, queue, info);
	}

	/* A file is the trigger, not a key: it works the same on X11 and Wayland, needs
	 * no input hooking inside another process's window, and can be set from a script
	 * or a hotkey daemon. While it exists the processed frame is held on screen. */
	int wanted = trigger_path && access(trigger_path, F_OK) == 0;
	if (!wanted && capture_every > 0)
		wanted = data->frame_counter % (unsigned long)capture_every == 0;

	for (uint32_t i = 0; i < info->swapchainCount; i++) {
		if (data->device_error || data->transfer_error) return present_now(data, queue, info);
		struct swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
		if (!chain || chain->device != data->device ||
		    (uint64_t)chain->swapchain != data->primary.handle ||
		    info->pImageIndices[i] >= chain->image_count) continue;
            if (data->result_chain != chain->swapchain) {
                data->holding = data->have_earlier = 0;
                data->result_chain = chain->swapchain;
            }
		uint32_t index = info->pImageIndices[i];
		if (wanted && ui_mask && !data->holding && !data->have_earlier) {
			/* Spend the first present after the trigger keeping the frame, and
			 * process the next one against it. One frame of extra latency on a
			 * pass that already takes a second, and nothing at all while the
			 * trigger is down. */
			VkDeviceSize needed = (VkDeviceSize)chain->extent.width
					      * chain->extent.height * 4;
			if (ensure_resources(data, needed) == 0
			    && transfer(data, chain, queue, index, 0) == 0) {
				if (data->earlier_size < needed) {
					unsigned char *grown = realloc(data->earlier, (size_t)needed);
					if (grown) { data->earlier = grown; data->earlier_size = needed; }
				}
				if (data->earlier_size >= needed) {
					memcpy(data->earlier, data->mapped, (size_t)needed);
					data->have_earlier = 1;
				}
			}
		} else if (wanted && !data->holding) {
			if (process_frame(data, chain, queue, index) == 0) {
				data->holding = 1;
				int returned = transfer(data, chain, queue, index, 1) == 0;
				nr_timing_finish(&data->timing, returned);
			} else nr_timing_finish(&data->timing, 0);
		} else if (wanted && data->holding
			   && data->result_size == (VkDeviceSize)chain->extent.width
						 * chain->extent.height * 4) {
			memcpy(data->mapped, data->result, (size_t)data->result_size);
			transfer(data, chain, queue, index, 1);
		} else if (!wanted) {
			data->holding = 0;
			data->have_earlier = 0;
		}
	}
	return present_now(data, queue, info);
}

VKAPI_ATTR VkResult VKAPI_CALL nr_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    struct queue_data *owner = find_queue(queue);
    struct device_data *data = owner ? find_device(owner->device) : NULL;
    if (!data) for (int i = 0; i < 8; i++) if (devices[i].device) { data = &devices[i]; break; }
    if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    nr_mutex_lock(&data->present_lock);
    VkResult result = data->device_error ? data->device_error : present_locked(queue, info);
    nr_mutex_unlock(&data->present_lock);
    return result;
}

#define INTERCEPT(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)nr_##name

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetDeviceProcAddr(VkDevice device,
							      const char *pName)
{
	INTERCEPT(QueuePresentKHR);
	INTERCEPT(CreateSwapchainKHR);
	INTERCEPT(DestroyDevice);
	INTERCEPT(DestroySwapchainKHR);
	INTERCEPT(GetDeviceQueue);
	INTERCEPT(GetDeviceQueue2);
	struct device_data *data = find_device(device);
	return data ? data->get_device_proc(device, pName) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetInstanceProcAddr(VkInstance instance,
								const char *pName)
{
	INTERCEPT(CreateInstance);
	INTERCEPT(CreateDevice);
	INTERCEPT(GetInstanceProcAddr);
	INTERCEPT(GetDeviceProcAddr);
	INTERCEPT(QueuePresentKHR);
	INTERCEPT(CreateSwapchainKHR);
	INTERCEPT(DestroyDevice);
	INTERCEPT(DestroySwapchainKHR);
	INTERCEPT(GetDeviceQueue);
	INTERCEPT(GetDeviceQueue2);
	return next_instance_proc ? next_instance_proc(instance, pName) : NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
	VkNegotiateLayerInterface *pVersion)
{
	if (pVersion->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;
	pVersion->loaderLayerInterfaceVersion = 2;
	pVersion->pfnGetInstanceProcAddr = nr_GetInstanceProcAddr;
	pVersion->pfnGetDeviceProcAddr = nr_GetDeviceProcAddr;
	pVersion->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
