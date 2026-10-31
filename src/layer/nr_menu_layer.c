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
		if (!data->menu_ui) {
			fprintf(stderr, "[nr_layer] no ImGui UI (%s); the quad menu serves instead\n",
				ui_why ? ui_why : "unknown");
		}
	}

	/* The panel's logic, which the DirectX 12 proxy shares: the bridge, the keys, the rows, the
	 * pointer (`nr_panel.c`). The trigger is the layer's, and so are the pacing rows. */
	char settings[1088];
	menu_settings_path(settings, sizeof settings);
	nr_panel_host host = { layer_trigger_get, layer_trigger_set, NULL, 1 };
	data->panel = nr_panel_create(data->menu, data->menu_ui, settings, data->menu_language, &host);
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

		/* The panel's frame: its rows, the pointer, ImGui's frame and what the user did in it. */
		nr_panel_frame(data->panel, chain->extent.width, chain->extent.height);

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
				LAYER_TRACE("[ov] menu=%p ui=%p view=%p img=%p cmd=%p target=%p %ux%u\n",
					(void *)data->menu, (void *)data->menu_ui,
					nrex_image_view(data->menu_ui), nrex_image(data->menu_ui),
					(void *)data->menu_commands, (void *)chain->images[index],
					chain->extent.width, chain->extent.height);
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

void nr_menu_layer_destroy(struct device_data *data)
{
	if (!data) return;
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
