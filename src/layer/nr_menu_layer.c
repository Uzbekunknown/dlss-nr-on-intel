/* The in-game menu's Vulkan side: the menu built on the game's device, and the composite pass.
 *
 * **Windows only, and built only on Windows.** See `nr_menu_layer.h` for why the split exists.
 * Everything here used to live in `nr_layer.c`; it moved so that a Linux build compiles none of
 * the menu, which is what the maintainer asked for - Linux's layer should be `newnr`'s layer, with
 * no ImGui, no `libstdc++` and no STORAGE on a swapchain.
 *
 * What stayed in `nr_layer.c` is the layer: capture, the transport, the trigger file, and the
 * three hooks below. The panel's logic - the keys, the rows, the pointer, what the user did - moved
 * on again, into `nr_panel.c`, which the DirectX 12 proxy builds too (2026-10-31). What is here is
 * what needs a Vulkan device and a swapchain: building the menu and the UI on the device, and
 * drawing the panel into the image being presented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

/* `access`'s mode argument. Windows spells it in <io.h> as `_access` and has no F_OK; `nr_layer.c`
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

/* First, because it sets the Vulkan platform define that `nr_gpu_bridge.h` needs and includes
 * Vulkan itself. Anything that includes Vulkan before this is a compile error on Win32. */
#include "nr_layer_state.h"

#include "nr_transport.h"
#include "nr_layer_stats.h"
#include "nr_surface_policy.h"
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"
#include "nr_menu_font.h"
#include "nr_menu_layer.h"
#include "nr_panel.h"

/* ------------------------------------------------------------------ shared with nr_layer.c
 *
 * These are the things the glue needs from the file it used to live in: accessors, because
 * exporting a `VkInstance` as a global is how two translation units end up disagreeing about which
 * instance is current.
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
/* The effect's trigger, for the panel: the layer's own file, which `nr_layer.c` names. */
static int layer_trigger_get(void *user)
{
	(void)user;
	/* **`*trigger_path` as well as `trigger_path`.** NR_LAYER_TRIGGER can be the empty string,
	 * which is not NULL, and _access("") on Windows is not a defined failure. */
	const char *trigger_path = nr_layer_trigger_path();
	if (!trigger_path || !*trigger_path) return -1;
	return access(trigger_path, F_OK) == 0;
}

static void layer_trigger_set(void *user, int on)
{
	(void)user;
	const char *trigger_path = nr_layer_trigger_path();
	if (trigger_path && *trigger_path) nr_layer_set_trigger_file(trigger_path, on);
}

/* The frame counter's totals, which `nr_layer.c` keeps on the device: presents of the primary
 * swapchain, and the frames NR answered and wrote back into it, with the network's time the
 * daemon gave for them. Read under the present lock the layer holds while the panel runs. */
static void layer_counts(void *user, nr_frame_counts *out)
{
	const struct device_data *data = (const struct device_data *)user;
	if (!data || !out) return;
	out->presented = data->presents_counted;
	out->nr_frames = data->nr_frames;
	out->network_us = data->nr_network_us;
	out->network_frames = data->nr_network_frames;
}

/* Every recording of the panel's that may still be on the GPU, finished: waited for on its fence.
 *
 * The ring (`menu_slot_*`) lets a frame's panel run on the GPU while the next one is recorded, so
 * the present does not wait for the GPU. What it shares between frames is safe because each slot is
 * reused only after its own fence - but a few things are not per slot, and before one of them is
 * destroyed or rewritten, every frame that may use it has to be done: the UI image on a resize, the
 * font atlas the backend uploads a new glyph into (`nrex_vk_drain_hook`), the views of a swapchain
 * that is going, the pool when the queue family changes. All rare, and each a wait for at most a few
 * frames of the panel's own work.
 *
 * Returns 0 when a fence did not signal in 2 s - a hung or lost device, where nothing the caller
 * was about to destroy is safe to destroy. */
static int menu_drain(struct device_data *data)
{
	PFN_vkWaitForFences wait = (PFN_vkWaitForFences)
		data->get_device_proc(data->device, "vkWaitForFences");
	if (!wait) return 0;
	VkFence fences[NR_MENU_SLOTS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < NR_MENU_SLOTS; ++i)
		if (data->menu_slot_fences[i]) fences[n++] = data->menu_slot_fences[i];
	if (!n) return 1;
	if (wait(data->device, n, fences, VK_TRUE, 2000000000ull) == VK_SUCCESS) return 1;
	fprintf(stderr, "[nr_layer] the panel's earlier frames did not finish in 2 s\n");
	return 0;
}

static void menu_drain_hook(void *user)
{
	menu_drain((struct device_data *)user);
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
	/* **What the panel asks of a device, which is less than it used to.** The pass is a compute
	 * dispatch over two storage images, two storage buffers and a push constant - all Vulkan 1.0,
	 * no feature of the application's to inspect, no extension to enable. It is built at
	 * `--target-env vulkan1.0` for exactly that reason.
	 *
	 * Two requirements were removed here after refusing real games, and both were reported as
	 * hardware facts that were really leftovers:
	 *
	 *   - a Vulkan version test, on the theory that a SPIR-V 1.6 module needed a 1.3 device. It
	 *     compared the version the GAME DECLARED, which is not a ceiling on the device - a game on
	 *     1.0 still creates a device at the hardware's version. The module is 1.0 now anyway.
	 *   - `bufferDeviceAddress`, on the theory that the blend pass took both buffers by address.
	 *     The pass moved to storage buffers and the usage flag stayed behind, where it made
	 *     `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` illegal on a device without the feature
	 *     (VUID-VkMemoryAllocateInfo-flags-03331) and so demanded a 1.2 feature nothing used.
	 *
	 * Measured, not argued: with both gone the panel builds and draws on vkcube's device, which
	 * enables no features at all. `menu_device_reason` is what is left of the device test and
	 * answers NULL; the surface's own `storage_ok` above is the only real gate. */
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
		char *font = nr_panel_font(&font_size);
		static int codepoints[NR_FONT_MAX_CODEPOINTS];
		int codepoint_count = nr_panel_codepoints(codepoints, NR_FONT_MAX_CODEPOINTS);
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
		/* The UI's own command buffers get the loader's data, as the ring's do below. */
		nrex_vk_loader_data((void *)(uintptr_t)data->set_loader_data);
		/* The backend's ring of vertex buffers is as long as the layer's ring of recordings, which
		 * is what makes the one it rewrites each frame one no recording in flight still reads. The
		 * swapchain's own count stays the minimum the backend is told. */
		uint32_t min_images = chain_image_count(chain);
		if (min_images > NR_MENU_SLOTS) min_images = NR_MENU_SLOTS;
		data->menu_ui = nrex_create(data->device, data->physical, (void *)ui_instance,
					    (void *)ui_queue, ui_family_out,
					    (uint32_t)chain->format, 420u << 12,
					    min_images, NR_MENU_SLOTS,
					    (void *)(uintptr_t)data->get_device_proc,
					    (void *)(uintptr_t)ui_next,
					    font, font_size, codepoints, codepoint_count, &ui_why);
		if (data->menu_ui) nrex_vk_drain_hook(data->menu_ui, menu_drain_hook, data);
		/* NOT freed here. AddFontFromMemoryTTF adopts the buffer (FontData with
		 * FontDataOwnedByAtlas left true) and frees it when the atlas is destroyed - freeing it
		 * on this side too was a double free, which is the access violation this menu hit the
		 * moment a font was supplied. Ownership transferred, so the pointer is not touched again;
		 * if the UI never asked for it, the buffer is freed here instead. */
		if (!data->menu_ui) free(font);
		/* next_instance_proc needs the instance; `nrex_create` was given it beside the resolver
		 * above, so the two together reach the instance-level commands. */
		if (!data->menu_ui) {
			fprintf(stderr, "[nr_layer] no ImGui UI (%s); the quad menu serves instead\n",
				ui_why ? ui_why : "unknown");
		}
	}

	/* The panel's logic, which the DirectX 12 proxy shares: the bridge, the keys, the rows, the
	 * pointer (`nr_panel.c`). The trigger is the layer's, and so are the pacing rows. */
	char settings[1088];
	menu_settings_path(settings, sizeof settings);
	nr_panel_host host = { layer_trigger_get, layer_trigger_set, data, 1, layer_counts };
	data->panel = nr_panel_create(data->menu, data->menu_ui, settings, data->menu_language, &host);
}

/* The panel's frame: one recording from the ring, one submit, and the semaphore its present waits on.
 *
 * **The present no longer waits for the GPU** (2026-11-06). This used to end in a wait on the menu's
 * fence: the submit took the game's present semaphores, so the present had nothing left to wait on
 * and the CPU waited instead - for the panel's blend, and with it for the whole of the game's frame,
 * since a fence covers everything submitted before it on the queue. With NR on that cost nothing new
 * (NR waits for the frame anyway), but with NR off the frame counter in a corner made every present
 * wait for its frame: 1.1 ms a present in `test_gpu_present`, and in a game that keeps the GPU busy,
 * the CPU's head start on the next frame gone. It is how MangoHud's overlay presents, and Mesa's
 * overlay layer before it: the submit signals a semaphore of the layer's own and the present waits on
 * that one.
 *
 * What changed with it, because a frame's panel can now still be running when the next is recorded:
 *
 *   - the command buffer, its fence and the overlay's descriptor set are one of NR_MENU_SLOTS in a
 *     ring, each reused only after its own fence (which also covers everything before it on the
 *     queue), and the UI backend's ring of vertex buffers is as long (`menu_build`);
 *   - the semaphore is one per swapchain image (`menu_done`): a semaphore a present waited on is
 *     free again only once that image is acquired again, and the next composite into the image is
 *     exactly then - validation reports any other reuse;
 *   - what is shared and not per slot waits for the ring to empty first (`menu_drain`): the UI
 *     image on a resize, the font atlas when a glyph is new, the views of a swapchain going;
 *   - the UI's render pass orders its layout transition after the previous frame's blend
 *     (`nr_menu_ui_vk.cpp`), which it did not, because nothing could be in flight.
 *
 * Returns 1 when the frame was drawn, 0 when there was nothing to record or the recording failed -
 * in which case the frame presents without a menu and with the game's own semaphores, which is a
 * missing menu rather than a lost game. */
static int menu_composite(struct device_data *data, struct swapchain_data *chain, VkQueue queue,
                          uint32_t index)
{
	if (!data->menu || data->device_error || data->transfer_error) return 0;
	if (index >= chain->image_count || index >= MAX_IMAGES) return 0;
	if (!chain->storage_ok) return 0;

/* "may be set and not read" - GCC and Clang take the attribute, MSVC has no such thing and treats
 * an initialised local as fine by default, so the macro is empty there. */
#if defined(__GNUC__) || defined(__clang__)
#  define NR_MAYBE_UNUSED __attribute__((unused))
#else
#  define NR_MAYBE_UNUSED
#endif

/* Resolve one device entry point for this function's use.
 *
 * `NR_MAYBE_UNUSED` is not decoration: an entry can be in the list for a call that lives elsewhere,
 * and GCC's `-Wunused-but-set-variable` calls that a warning - the Linux build had 20 of them where
 * `newnr` had none. Marking the whole family is what keeps the list honest: adding an entry for a
 * call that turns out to live elsewhere does not cost a warning, and removing one does not cost a
 * build. */
#define FN(type, name) type name NR_MAYBE_UNUSED = \
	(type)data->get_device_proc(data->device, "vk" #name)
	FN(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers);
	FN(PFN_vkResetCommandBuffer, ResetCommandBuffer);
	FN(PFN_vkBeginCommandBuffer, BeginCommandBuffer);
	FN(PFN_vkEndCommandBuffer, EndCommandBuffer);
	FN(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier);
	FN(PFN_vkQueueSubmit, QueueSubmit);
	FN(PFN_vkCreateFence, CreateFence);
	FN(PFN_vkDestroyFence, DestroyFence);
	FN(PFN_vkResetFences, ResetFences);
	FN(PFN_vkWaitForFences, WaitForFences);
	FN(PFN_vkCreateSemaphore, CreateSemaphore);
	FN(PFN_vkCreateCommandPool, CreateCommandPool);
	FN(PFN_vkDestroyCommandPool, DestroyCommandPool);
#undef FN
	if (!CreateCommandPool || !CreateSemaphore || !DestroyFence) return 0;

	/* A pool of the menu's own, not `data->pool`.
	 *
	 * `data->pool` belongs to the capture path: `transfer()` creates it lazily, and `transfer()`
	 * only runs while the layer is capturing - which needs the trigger to exist. The menu has
	 * nothing to do with capture, so on a game where NR is idle the pool is still VK_NULL_HANDLE
	 * when the menu first draws, and `vkAllocateCommandBuffers` with a null pool faults inside the
	 * loader. That was this menu's crash: an access violation reading 0x70, which is a field of
	 * the dispatch table behind a NULL handle.
	 *
	 * One pool per queue family, rebuilt when the family changes. A command pool belongs to a family
	 * for its whole life and its command buffers may only be submitted to a queue of that family.
	 * The layer reassigns `data->queue_family` at present time when the presenting queue is a
	 * different one, and frees the CAPTURE pool there (`release_transfer_resources`) for exactly
	 * this reason - the menu's pool was not included, so it went stale. A submit from a stale pool
	 * is accepted by the loader and does nothing, which is what the readback found: no pixel of the
	 * panel anywhere in the image, no error, no crash. Its buffers may still be running, so the ring
	 * empties first. */
	if (data->menu_pool && data->menu_pool_family != data->queue_family) {
		if (!menu_drain(data)) return 0;
		for (uint32_t i = 0; i < NR_MENU_SLOTS; ++i) data->menu_slot_commands[i] = VK_NULL_HANDLE;
		if (DestroyCommandPool) DestroyCommandPool(data->device, data->menu_pool, NULL);
		data->menu_pool = VK_NULL_HANDLE;
	}
	if (!data->menu_pool) {
		/* **`RESET_COMMAND_BUFFER` as well as `TRANSIENT`, and the first is not optional.**
		 * Every frame the menu draws begins with `vkResetCommandBuffer` on one of this pool's
		 * buffers, and resetting a buffer from a pool without this flag is invalid usage - the
		 * maintainer's validation run on vkcube reported it, and under validation vkcube then exited
		 * 0xC0000409. `TRANSIENT` stays because the buffers are genuinely short-lived; the two flags
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

	/* This frame's place in the ring, its buffer and its fence. The fence is made signalled, so the
	 * wait below is a wait for whatever the slot last ran and returns at once on its first use; it
	 * is reset only just before the submit, so a frame that gives up earlier leaves it as it was. (It
	 * was once made unsignalled and reset at the top, and the resize waited on it: a second's stall
	 * at the first frame and after every resize, 2026-11-05.) */
	uint32_t slot = data->menu_slot_next % NR_MENU_SLOTS;
	if (!data->menu_slot_commands[slot]) {
		VkCommandBufferAllocateInfo alloc = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = data->menu_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1 };
		VkCommandBuffer made = VK_NULL_HANDLE;
		if (AllocateCommandBuffers(data->device, &alloc, &made) != VK_SUCCESS) return 0;
		/* A command buffer is dispatchable: the loader's data, or a layer below cannot find its
		 * device by it. */
		if (data->set_loader_data && data->set_loader_data(data->device, made) != VK_SUCCESS)
			return 0;
		data->menu_slot_commands[slot] = made;
	}
	if (!data->menu_slot_fences[slot]) {
		VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT };
		if (CreateFence(data->device, &fi, NULL, &data->menu_slot_fences[slot]) != VK_SUCCESS)
			return 0;
	}
	/* The semaphore this image's present will wait on. */
	if (!chain->menu_done[index]) {
		VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (CreateSemaphore(data->device, &si, NULL, &chain->menu_done[index]) != VK_SUCCESS)
			return 0;
	}
	VkCommandBuffer cmd = data->menu_slot_commands[slot];
	VkFence fence = data->menu_slot_fences[slot];

	/* What this slot ran NR_MENU_SLOTS frames ago has finished - and with it everything submitted
	 * before it on the queue, so the backend's vertex buffer for this frame, which it last wrote as
	 * many frames ago, is free as well. With fewer images than slots this returns at once. */
	if (WaitForFences(data->device, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return 0;
	/* Taken from here on, whether or not this frame reaches its submit: the backend's ring advances
	 * with every frame it draws, and the slot counter must not fall behind it. */
	data->menu_slot_next++;
	if (ResetCommandBuffer(cmd, 0) != VK_SUCCESS) return 0;

	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	if (BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return 0;

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
	CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (data->menu_ui) {
		/* The ImGui path: the frame is built by the panel (`nr_panel_frame`), rendered into the UI's
		 * own image, then blended onto the game's - which is the step that was missing when the
		 * backend reported itself started and the screen stayed clean. */
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
			/* `nrex_resize` destroys the UI image and makes a new one, and earlier frames may
			 * still be drawing into it and blending from it: the whole ring first. Destroying an
			 * image the GPU is still using is undefined - a corrupt panel, a device loss, or
			 * nothing at all, depending on the driver. */
			if (!menu_drain(data)) {
				EndCommandBuffer(cmd);
				return 0;
			}
			if (!nrex_resize(data->menu_ui, chain->extent.width, chain->extent.height)) {
				fprintf(stderr, "[nr_layer] the UI image would not resize to %ux%u; "
						"keeping %ux%u\n", chain->extent.width, chain->extent.height,
					nrex_image_width(data->menu_ui), nrex_image_height(data->menu_ui));
			}
		}

		/* The panel's frame: its rows, the pointer, ImGui's frame and what the user did in it. */
		nr_panel_frame(data->panel, chain->extent.width, chain->extent.height);

		nrex_render(data->menu_ui, (uint64_t)(uintptr_t)cmd);

		/* The render pass has just written the UI image and the blend is about to read it as a
		 * storage image. Without this the dispatch is free to run while those writes are still in
		 * flight, and what it reads is a mixture of this frame and the last - the banding the
		 * first working build showed, with pieces of the game's own frame inside the panel. The
		 * pass's own dependency out says the same since 2026-11-06; this stays as the statement
		 * at the place it matters.
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
			ui_barrier.image = (VkImage)nrex_image(data->menu_ui);
			ui_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			ui_barrier.subresourceRange.levelCount = 1;
			ui_barrier.subresourceRange.layerCount = 1;
			CmdPipelineBarrier(cmd,
					   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					   0, 0, NULL, 0, NULL, 1, &ui_barrier);
		}
		{
			static int shown = 0;
			if (!shown) {
				shown = 1;
				LAYER_TRACE("[ov] menu=%p ui=%p view=%p img=%p cmd=%p target=%p %ux%u\n",
					(void *)data->menu, (void *)data->menu_ui,
					nrex_image_view(data->menu_ui), nrex_image(data->menu_ui),
					(void *)cmd, (void *)chain->images[index],
					chain->extent.width, chain->extent.height);
			}
		}
		/* Over the part the frame drew into and no more: the panel's rectangle while it is open,
		 * and with it shut the frame counter's corner alone, which is a few thousand pixels where
		 * a pass over the whole image is millions on every frame the counter is shown. */
		uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
		if (nrex_drawn_rect(data->menu_ui, &rx, &ry, &rw, &rh) &&
		    rx + rw <= chain->extent.width && ry + rh <= chain->extent.height) {
			/* A view of this image for the blend to write through, kept between frames. A full
			 * cache - more images than it holds, which a swapchain going normally prevents - is
			 * emptied only once nothing can be writing through those views. */
			/* Straight to 64 bits, not through `uintptr_t`: an image and a view are 64-bit
			 * handles on x86 too, where `uintptr_t` would cut them to 32 (nr_menu.c says the
			 * same of its side). The command buffer is a pointer, so it may go through it. */
			uint64_t target = (uint64_t)chain->images[index];
			if (!nr_menu_view_ready(data->menu, target)) {
				if (menu_drain(data)) nr_menu_forget_views(data->menu);
			}
			nr_menu_draw_overlay_rect(data->menu, slot, (uint64_t)(uintptr_t)cmd, target,
						  nrex_image_view(data->menu_ui), rx, ry, rw, rh);
		}
	}

	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (EndCommandBuffer(cmd) != VK_SUCCESS) return 0;

	/* The game's present waits, so this store is ordered after the frame that is being presented.
	 *
	 * An empty wait list was the first attempt and it does not work: the panel was drawn, the
	 * submit completed on its fence, and nothing appeared - because a submit with nothing to wait
	 * for is unordered against the queue's other work, and on this driver the store landed before
	 * the frame's own rendering finished. `transfer()` waits on `data->present_wait` for the same
	 * reason and its pixels do reach the screen.
	 *
	 * Zero waits is a real case, not a fallback: the waits are gone once a capture has taken them,
	 * and the menu still has to draw - ordered then by the capture's own completion, which the layer
	 * waited for on its private fence before this submit was made.
	 *
	 * **Waits a capture already took are not waited again** (2026-11-05). A binary semaphore is
	 * unsignalled by the wait that consumes it, so each can be waited once. The GPU path's capture
	 * takes the game's waits and left `present_wait_count` as it was, so the menu waited on them a
	 * second time; and with NR off the menu took them and the present waited on them again. Both
	 * are waits on a semaphore with no signal to come, which Intel's driver has let pass. Now the
	 * menu waits only on what nobody took, marks them taken, and signals this image's own
	 * semaphore, which is the one the present waits on (`present_now`): it covers the game's waits
	 * as well, since this submit waited for them before it ran. */
	uint32_t waits = data->waits_consumed ? 0 : data->present_wait_count;
	if (waits > data->transfer_stage_capacity) {
		VkPipelineStageFlags *grown = realloc(data->transfer_stages,
						      (size_t)waits * sizeof *grown);
		if (!grown) return 0;
		data->transfer_stages = grown;
		data->transfer_stage_capacity = waits;
		for (uint32_t i = 0; i < waits; ++i)
			grown[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
	VkSemaphore done = chain->menu_done[index];
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = waits, .pWaitSemaphores = waits ? data->present_wait : NULL,
		.pWaitDstStageMask = waits ? data->transfer_stages : NULL,
		.commandBufferCount = 1, .pCommandBuffers = &cmd,
		.signalSemaphoreCount = 1, .pSignalSemaphores = &done };
	if (ResetFences(data->device, 1, &fence) != VK_SUCCESS) {
		data->device_error = VK_ERROR_DEVICE_LOST;
		return 0;
	}
	if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
		/* Reset and never submitted, the fence would hold this slot for a second at every turn of
		 * the ring. Made again, signalled, when the slot comes round - after the rest of the ring
		 * has finished, so that a fresh fence on this slot does not stand for work it never saw. */
		menu_drain(data);
		DestroyFence(data->device, fence, NULL);
		data->menu_slot_fences[slot] = VK_NULL_HANDLE;
		return 0;
	}
	if (waits) {
		data->waits_consumed = 1;
		data->present_wait_count = 0;
	}
	data->menu_present_signal = done;
	return 1;
}

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
	if (!data->menu && !data->menu_state && nr_panel_enabled()
	    && info->swapchainCount > 0 && info->pSwapchains) {
		struct swapchain_data *first = nr_layer_find_swapchain(info->pSwapchains[0]);
		if (first && first->device == data->device)
			menu_build(data, first);
	}
	if (!data->panel) return 0;

	/* The keys, every present, open or shut, whether or not this present has a swapchain the layer
	 * tracks: the toggle keeps responding on a present-only queue (`nr_panel_input`). The draw only
	 * when the panel has something to show. */
	if (!nr_panel_input(data->panel, NULL)) return 0;
	if (info->swapchainCount > 0 && info->pSwapchains && info->pImageIndices) {
		struct swapchain_data *chain = nr_layer_find_swapchain(info->pSwapchains[0]);
		if (chain && chain->device == data->device)
			return menu_composite(data, chain, queue, info->pImageIndices[0]);
	}
	return 0;
}

void nr_menu_layer_swapchain_gone(struct device_data *data, struct swapchain_data *chain)
{
	if (!data || !chain) return;
	int drawn = 0;
	for (uint32_t i = 0; i < MAX_IMAGES; ++i)
		if (chain->menu_done[i]) drawn = 1;
	/* Nothing to do for a swapchain the panel never drew into: it has no semaphores, and the
	 * views it keeps are of images it drew into. */
	if (!drawn) return;
	/* The panel's recordings into these images finish first. The game has waited for its own work
	 * before destroying the swapchain - `vkDestroySwapchainKHR` asks that every use of its images
	 * has completed - but it cannot know about the layer's, which came after its own. */
	menu_drain(data);
	if (data->menu) nr_menu_forget_views(data->menu);
	PFN_vkDestroySemaphore destroy_semaphore = (PFN_vkDestroySemaphore)
		data->get_device_proc(data->device, "vkDestroySemaphore");
	for (uint32_t i = 0; i < MAX_IMAGES; ++i) {
		if (chain->menu_done[i] && destroy_semaphore)
			destroy_semaphore(data->device, chain->menu_done[i], NULL);
		chain->menu_done[i] = VK_NULL_HANDLE;
	}
}

void nr_menu_layer_destroy(struct device_data *data)
{
	if (!data) return;
	/* The ring's recordings finish before anything they used goes. A device being destroyed is
	 * idle already - the game waited - so this returns at once. */
	menu_drain(data);
	/* The panel first: it gives the cursor back, ends its hold on the game's input, and ends the
	 * bridge - which is what removes the live preview, the daemon's view of what the user is
	 * trying RIGHT NOW. Leaving that behind made a Save look like it had not happened. */
	if (data->panel) {
		nr_panel_destroy(data->panel);
		data->panel = NULL;
	}
	if (data->menu_ui) {
		nrex_destroy(data->menu_ui);
		data->menu_ui = NULL;
	}
	if (data->menu) {
		nr_menu_destroy(data->menu);
		data->menu = NULL;
	}
	{
		PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)
			data->get_device_proc(data->device, "vkDestroyFence");
		for (uint32_t i = 0; i < NR_MENU_SLOTS; ++i) {
			if (data->menu_slot_fences[i] && destroy_fence)
				destroy_fence(data->device, data->menu_slot_fences[i], NULL);
			data->menu_slot_fences[i] = VK_NULL_HANDLE;
			/* Freed with the pool below. */
			data->menu_slot_commands[i] = VK_NULL_HANDLE;
		}
		data->menu_present_signal = VK_NULL_HANDLE;
	}
	if (data->menu_pool) {
		PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)
			data->get_device_proc(data->device, "vkDestroyCommandPool");
		if (destroy_pool) destroy_pool(data->device, data->menu_pool, NULL);
		data->menu_pool = VK_NULL_HANDLE;
	}
}
