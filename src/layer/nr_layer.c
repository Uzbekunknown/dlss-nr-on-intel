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
#include <stdarg.h>
#include <errno.h>
#ifndef _WIN32
/* `CLOCK_MONOTONIC`, for the frame timer's non-Windows arm. Windows reaches the same clock
 * through QPC, which `nr_now_seconds` below keeps to its own side of the `#ifdef`. */
#  include <time.h>
#endif
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
#include "nr_raw_mouse.h"
#include "nr_focus.h"
/* The string tables and the codepoint collector: the UI's font atlas is built from the menu's own
 * text, so this file needs to reach them. */
#include "nr_menu_strings.h"
#include "nr_menu_font.h"
#include "nr_layer_cadence.h"   /* CADENCE_NAME_STORE: the one size the store and its test share */
/* `struct device_data` and `struct swapchain_data`, which the menu's own file needs too. They used
 * to be defined below; they live in a header now so the Windows-only glue can see them. */
#include "nr_layer_state.h"
/* The three hooks this file calls into the menu's own translation unit.
 *
 * **Including this is not optional, and leaving it out was a real defect.** `nr_menu_layer_present`
 * was called without a declaration, so C assumed it returned `int` - and MSVC, whose C4013 is a
 * level-3 warning, said nothing at its default level. GCC refuses the file outright, which is what
 * the maintainer's MinGW and Linux runs hit. The same mistake in a different file truncated a
 * 64-bit pointer and crashed a game one round earlier; `build_win.bat` now passes `/we4013`, so
 * MSVC makes it an error rather than relying on anyone noticing. */
#include "nr_menu_layer.h"

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
#  ifndef access
#    define access(p, m) _access(p, m)
#  endif
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#endif

/* Locking uses the platform's own primitive: an SRWLOCK needs no destroy step and is
 * what this file used on Windows before, a pthread_mutex is what Linux uses. */

/* Whether a virtual key is down, on whichever platform has a keyboard.
 *
 * `GetAsyncKeyState` is Windows-only, and six call sites in this file used it directly - the six
 * hotkeys and the panel's left button - with no `#if` around them. `nr_layer.c` compiled on Linux
 * before this branch because `windows.h` sits inside the `#ifdef _WIN32` below with a pthread arm
 * beside it; those six lines (and the window helpers) undid that, so `make` stopped here.
 *
 * Off Windows the answer is "not down" for every key, which is the same thing a platform backend
 * with no input reports: ImGui derives its own edges from the levels it is handed, so all-up
 * leaves every widget idle rather than latched. Nothing on Linux reads the result today - the
 * ImGui backend is a stub there - but a layer that stops compiling is worse than one that stops
 * responding. */

/* A monotonic clock in seconds, for the panel's frame timer.
 *
 * The QPC calls this file used directly were the last of the Win32 leak: `LARGE_INTEGER` is a
 * Windows type, so a Linux compile reached three lines of it in the middle of `menu_composite`.
 * `nr_menu_ui.cpp` already answers the same question off Windows with `clock_gettime` and
 * `CLOCK_MONOTONIC`, and this is the same question.
 *
 * 0.0 means "no clock", which is what the caller falls back on: a machine without one keeps the
 * fixed 1/60 step rather than moving nothing. */

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

/* A line appended whole to the daemon's log, which the daemon has open for appending and
 * shares. On Windows a game's stderr reaches nobody, and the setup window's status and
 * Save report read this log: the GPU path's reasons go there too (issue #17). */
static void append_daemon_log(const char *line)
{
	char fallback[1024 + 32];
	const char *log = getenv("NR_LAYER_LOG");
	if (!log || !*log) {
		char dir[1024];
		nr_default_dir(dir, sizeof dir);
		snprintf(fallback, sizeof fallback, "%s/nr_daemon.log", dir);
		log = fallback;
	}
#ifdef _WIN32
	HANDLE h = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD wrote;
	WriteFile(h, line, (DWORD)strlen(line), &wrote, NULL);
	CloseHandle(h);
#else
	int fd = open(log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) return;
	ssize_t wrote = write(fd, line, strlen(line));
	(void)wrote;
	close(fd);
#endif
}



/* Capture and writeback consume the game's waits once, complete on private
 * fences, then present with no remaining waits. Legacy NR_LAYER_SYNC values are
 * accepted for launcher compatibility; both use this single verified path. */



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


/* Said on stderr, as before, and into the daemon's log when to_log. */
static void say(int to_log, const char *fmt, va_list args)
{
	char line[512];
	vsnprintf(line, sizeof line, fmt, args);
	fputs(line, stderr);
	if (to_log) append_daemon_log(line);
}

static void say_in_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	say(1, fmt, args);
	va_end(args);
}

/* Strict mode can retain the game's image, but must never turn an unavailable
 * shared path into CPU pixel capture, processing or cached-image writeback. */
static int host_pixel_path_allowed(struct device_data *data, const char *operation)
{
    if (!gpu_only_mode()) return 1;
    if (!data || !data->gpu_only_notice) {
        say_in_log("[nr_layer] GPU-only: CPU pixel path disabled (%s); original GPU image retained\n", operation);
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

#ifdef _WIN32
/* Why the in-game panel cannot be built on the device `info` made, or NULL when it can.
 *
 * The panel's blend pass takes its two buffers by device address and is a SPIR-V 1.6 module, so
 * the device must run Vulkan 1.3 and have bufferDeviceAddress ENABLED. What the GPU supports is
 * not the question: the Arc supports the feature, vkcube's device enables nothing, and building
 * the panel there was invalid usage. The feature is on when the GPU path's patch put it there
 * (`gpu_enabled`), or when the game asked for it itself, as DXVK does. Decided here because only
 * vkCreateDevice sees what was enabled. */
static const char *menu_device_reason(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
				      int gpu_enabled)
{
	PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)
		next_instance_proc(layer_instance, "vkGetPhysicalDeviceProperties");
	VkPhysicalDeviceProperties props;
	memset(&props, 0, sizeof props);
	if (properties) properties(physical, &props);
	uint32_t version = layer_api_version < props.apiVersion ? layer_api_version : props.apiVersion;
	if (version < VK_API_VERSION_1_3)
		return "the device runs Vulkan below 1.3, and the panel's shader needs 1.3";
	if (gpu_enabled) return NULL;
	int steps = 0;
	for (const VkBaseInStructure *p = (const VkBaseInStructure *)info->pNext;
	     p && ++steps <= 128; p = p->pNext) {
		if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
		    && ((const VkPhysicalDeviceVulkan12Features *)p)->bufferDeviceAddress)
			return NULL;
		if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
		    && ((const VkPhysicalDeviceBufferDeviceAddressFeatures *)p)->bufferDeviceAddress)
			return NULL;
	}
	return "the game's device does not enable bufferDeviceAddress, which the panel needs";
}
#endif

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
	char gpu_note[200] = "";
	if (layer_api_version >= VK_API_VERSION_1_1 && nr_gpu_patch_device(&patch, physical, info,
		(PFN_vkEnumerateDeviceExtensionProperties)next_instance(layer_instance, "vkEnumerateDeviceExtensionProperties"),
		(PFN_vkGetPhysicalDeviceFeatures2)next_instance(layer_instance, "vkGetPhysicalDeviceFeatures2"),
		(PFN_vkGetPhysicalDeviceProperties)next_instance(layer_instance, "vkGetPhysicalDeviceProperties")))
		chosen = &patch.info;
	else if (getenv("NR_GPU_TRANSPORT") && !strcmp(getenv("NR_GPU_TRANSPORT"), "1")) {
		if (layer_api_version < VK_API_VERSION_1_1)
			/* A game on Vulkan 1.0, DOOM (2016) among them (issue #17). */
			snprintf(gpu_note, sizeof gpu_note,
				 "Vulkan 1.1 instance required, and the game asked for %u.%u",
				 VK_API_VERSION_MAJOR(layer_api_version), VK_API_VERSION_MINOR(layer_api_version));
		else
			snprintf(gpu_note, sizeof gpu_note, "%s",
				 patch.reason ? patch.reason : "the device cannot share frames");
		fprintf(stderr, "[nr_layer] GPU transport fallback: %s\n", gpu_note);
	}
	VkLayerDeviceLink *saved_link = link->u.pLayerInfo;
#endif
	VkResult r = create(physical, chosen, allocator, device);
#ifdef _WIN32
	if (r != VK_SUCCESS && patch.enabled) {
		snprintf(gpu_note, sizeof gpu_note, "patched device creation failed (%d)", r);
		fprintf(stderr, "[nr_layer] GPU transport fallback: %s\n", gpu_note);
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
		memcpy(data->gpu_note, gpu_note, sizeof data->gpu_note);
		data->menu_device_reason = menu_device_reason(physical, info, gpu_enabled);
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
	 * it, and the menu is only built when it was.
	 *
	 * **The caps are not enough: the format must take it too.** A surface's caps are the same
	 * for every format it offers, and Intel's Windows driver offers STORAGE on a surface whose
	 * B8G8R8A8_SRGB images cannot be storage images. A swapchain asked for that is invalid
	 * usage (VUID-VkSwapchainCreateInfoKHR-imageFormat-01778), which a driver may answer by
	 * failing the creation - and then the fallback turns capture off for the whole game. The
	 * menu's own `format_supports_storage` asks the same question, so a format that fails it
	 * never had a menu to lose.
	 *
	 * **And only on Windows, because that is the only place the menu exists.** The ImGui panel is
	 * the whole of what draws it: it needs a font from the Windows font directory, keys read
	 * through `GetAsyncKeyState`, and the STORAGE usage its blend pass writes through. Off Windows
	 * `nrex_create` is a no-op that returns NULL, so the menu is never built - but asking the
	 * surface for STORAGE anyway left every Linux swapchain carrying a usage flag nothing uses,
	 * which is exactly what the maintainer asked to be left as it was before this branch. The
	 * capture path does not need it: it reads through TRANSFER_SRC. */
	int storage_ok = 0;
#ifdef _WIN32
	{
		/* Resolved here rather than cached: a swapchain is created a handful of times in a
		 * session, and a pointer stored on the device would need invalidating when the instance
		 * goes away - which this file has been bitten by before. */
		PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn =
			(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
			next_instance_proc(layer_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		PFN_vkGetPhysicalDeviceFormatProperties format_fn =
			(PFN_vkGetPhysicalDeviceFormatProperties)
			next_instance_proc(layer_instance, "vkGetPhysicalDeviceFormatProperties");
		if (caps_fn && format_fn && data->physical) {
			VkSurfaceCapabilitiesKHR caps;
			memset(&caps, 0, sizeof caps);
			VkFormatProperties props;
			memset(&props, 0, sizeof props);
			format_fn(data->physical, info->imageFormat, &props);
			if (caps_fn(data->physical, info->surface, &caps) == VK_SUCCESS
			    && (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)
			    && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
				patched.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
				storage_ok = 1;
			}
		}
	}
#endif
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
		 * capture and loses the menu - see the comment above nr_CreateSwapchainKHR.
		 *
		 * **Through the hook, and only when `storage_ok` says so.** The hook sets the flag
		 * without looking, as its header says. A surface that refused STORAGE, or a swapchain
		 * made on the fallback path, must keep the 0 the memset gave it, or the panel writes
		 * into images that cannot hold it. Off Windows the hook is a no-op and the flag stays
		 * 0, which is correct: there is no menu to read it. */
		if (storage_ok) nr_menu_layer_note_storage(data, entry);
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
/* The GPU path's lines: on stderr, and the first few of a device in the daemon's log. A
 * format refused is tried again every 300 frames, and the log need not say so each time. */
static void gpu_say(struct device_data *data, const char *fmt, ...)
{
    int to_log = data->gpu_lines < 8;
    data->gpu_lines += to_log;
    va_list args;
    va_start(args, fmt);
    say(to_log, fmt, args);
    va_end(args);
}

/* A reason the path stands aside for good is written once frames take the usual path: by
 * then the daemon has written its own first lines, which begin a session in the window's
 * reading of the log, and the reason lands inside it. */
static void log_gpu_note(struct device_data *data)
{
    if (!data->gpu_note[0] || data->gpu_note_logged) return;
    data->gpu_note_logged = 1;
    char line[300];
    snprintf(line, sizeof line, "[nr_layer] GPU transport fallback: %s; frames take the usual path\n",
             data->gpu_note);
    append_daemon_log(line);
}

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
        gpu_say(data, "[nr_layer] GPU transport fallback: export retained until daemon releases it or exits\n");
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
        if (!data->gpu_announced++) {
            fprintf(stderr, "[nr_layer] GPU transport fallback: requires synchronous live every frame without mask/capture\n");
            if (!data->gpu_note[0])
                snprintf(data->gpu_note, sizeof data->gpu_note,
                         "requires synchronous live every frame without mask/capture "
                         "(NR_LAYER_ASYNC=%d, NR_LAYER_LIVE=%ld%s%s)", async_live, live_every,
                         ui_mask ? ", a mask" : "", capture_path ? ", a capture" : "");
        }
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
            gpu_say(data, "[nr_layer] GPU transport fallback: %s (%d)\n",
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
            gpu_say(data, "[nr_layer] GPU transport fallback: daemon negotiation %s (%u)\n",
                answered ? "declined" : "failed", answered ? reply.detail : 0u);
            release_gpu_bridge(data);
            if (answered && reply.status == NR_GPU_UNSUPPORTED)
                data->gpu_retry_frame = data->frame_counter + 300;
            else data->gpu_blocked = 1;
            return 0;
        }
        data->gpu_attached = 1;
        gpu_say(data, "[nr_layer] GPU transport ready: %ux%u format %d family %u daemon %lu sync=%s; pixels remain on GPU\n",
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
    gpu_say(data, "[nr_layer] GPU transport fallback: %s; original frame retained\n",
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
		/* **One call, and it is a hook.** This block used to call `nr_menu_bridge_destroy`,
		 * `nrex_destroy` and `nr_menu_destroy` itself, which had three consequences:
		 *
		 *   - off Windows those three have no definition at all, so the layer could not link
		 *     there - the no-op arm of `nr_menu_layer.h` was never seen;
		 *   - the menu's command pool and fence, which only the hook knows about, were never
		 *     destroyed. Validation's Object Tracking reports them at vkDestroyDevice;
		 *   - it left `nr_menu_layer_destroy` dead code, so "three hooks" was really one.
		 *
		 * The hook does the same three calls in the same order - bridge first, because the panel
		 * reads its rows through it - plus the pool and the fence. It must run before the memset
		 * below, which would otherwise drop the pointers and leak everything. */
		nr_menu_layer_destroy(data);
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

/* Turn the effect on or off, by creating or removing the trigger file the present path reads.
 *
 * One function because there are now three ways in - the Home key, the panel's checkbox, and
 * whatever comes next - and they must not each spell the file handling out. A zero-byte file is
 * "on" and its absence is "off"; nothing else is written, so nothing can go stale.
 *
 * Windows only, as its one caller below is: off Windows it would be a function with no caller. */
#ifdef _WIN32
static void set_trigger(const char *path, int on)
{
	if (!path || !*path) return;
	if (on) {
		FILE *f = fopen(path, "wb");
		if (f) fclose(f);
	} else {
		if (access(path, F_OK) == 0) remove(path);
	}
}
#endif

/* ------------------------------------------------------------------ what the menu's file needs
 *
 * `nr_menu_layer.c` holds the menu's glue and needs four things that are this file's: the instance
 * and the instance resolver, the swapchain lookup, and the trigger path. They are accessors rather
 * than exported globals for one reason: a `VkInstance` shared through a header is how two
 * translation units come to disagree about which instance is current, and this layer already runs
 * several devices in one process.
 *
 * Off Windows nothing calls them - the menu's file is not compiled - so they are `#ifdef`ed to keep
 * a Linux build from carrying four functions with no caller. */
#ifdef _WIN32
void nr_layer_menu_globals(VkInstance *instance, PFN_vkGetInstanceProcAddr *next_proc)
{
	if (instance) *instance = layer_instance;
	if (next_proc) *next_proc = next_instance_proc;
}

struct swapchain_data *nr_layer_find_swapchain(VkSwapchainKHR swapchain)
{
	return find_swapchain(swapchain);
}

void nr_layer_set_trigger_file(const char *path, int on)
{
	set_trigger(path, on);
}

const char *nr_layer_trigger_path(void)
{
	return trigger_path;
}
#endif




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
    /* The menu's own file owns all of this now - building it, reading its keys, drawing it.
     *
     * Three hooks, and off Windows they are empty: the menu does not exist there, so the whole of
     * `nr_menu_layer.c` is not compiled and nothing below is reached. That is what makes Linux
     * `newnr`'s layer again rather than a layer carrying a menu it can never show.
     *
     * The order matters and is unchanged: build on the first present that can carry it, then read
     * the keys, then draw. `nr_menu_layer_present` returns 1 when it drew into the frame. */
    nr_menu_layer_present(data, queue, info);

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
#ifdef _WIN32
					log_gpu_note(data);
#endif
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
