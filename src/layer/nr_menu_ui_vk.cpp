/* The panel's Vulkan renderer: ImGui's Vulkan backend drawing into an image of the panel's own,
 * which the layer then blends onto the swapchain image (nr_menu.c's overlay pass).
 *
 * It was the second half of nr_menu_ui.cpp until the panel gained a Direct3D 12 renderer
 * (nr_menu_ui_d3d12.cpp, 2026-10-31); the panel itself - its widgets, its keys, its pointer - is
 * nr_menu_ui.cpp, behind the same plain-C interface (nr_menu_ui.h), and this file adds what needs a
 * Vulkan device. The frame path, once more because it is the part that has to survive contact with
 * a layer:
 *
 *   ImGui renders (CPU) -> the backend draws into `intermediate` (a colour attachment this file
 *   owns) -> the caller blend-copies `intermediate` onto the game's swapchain image (a compute
 *   dispatch in nr_menu.c, which has the storage-image access and the semaphore contract).
 *
 * The intermediate exists because the swapchain image was created without COLOR_ATTACHMENT usage
 * and a layer cannot retro-fit usage flags onto a game's image.
 */
#include "nr_menu_ui.h"
#include "nr_menu_ui_backend.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cstring>

#ifndef NREX_TRACE_LOGGING
#define NREX_TRACE(...) ((void)0)
#else
#define NREX_TRACE(...) fprintf(stderr, __VA_ARGS__)
#endif
#define NREX_ERROR(...) fprintf(stderr, __VA_ARGS__)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <vector>

#ifdef _WIN32
/* The .def file names the exports; dllexport here would disagree with the header's plain
 * declarations and C2375 follows, so on Windows the macro is empty. */
#define NREX_API
#else
#define NREX_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ Vulkan, resolved lazily */

/* The backend needs the device's functions. Rather than linking vulkan-1.dll - a layer must not
 * open its own Vulkan library, it rides the one the game loaded - every call goes through the
 * vkGetDeviceProcAddr the C side hands in. A small table of the ones this file uses directly. */
struct nrex_vk {
    PFN_vkCreateImage         create_image;
    PFN_vkDestroyImage        destroy_image;
    PFN_vkAllocateMemory      alloc_mem;
    PFN_vkFreeMemory          free_mem;
    PFN_vkBindImageMemory     bind_img;
    PFN_vkGetImageMemoryRequirements img_reqs;
    PFN_vkGetPhysicalDeviceMemoryProperties mem_props;
    PFN_vkCreateImageView     create_view;
    PFN_vkDestroyImageView    destroy_view;
    PFN_vkCreateRenderPass    create_pass;
    PFN_vkDestroyRenderPass   destroy_pass;
    PFN_vkCreateFramebuffer   create_fb;
    PFN_vkDestroyFramebuffer  destroy_fb;
    PFN_vkCmdBeginRenderPass  begin_pass;
    PFN_vkCmdEndRenderPass    end_pass;
    /* Verification only: `NR_MENU_DUMP` copies the UI's own image to a PPM so the panel can be
     * looked at. A Vulkan swapchain image cannot be screen-captured - the desktop compositor never
     * sees it - so the UI's intermediate, which holds exactly what the panel drew, is the only
     * direct witness. Resolved here rather than on the C side because this file already owns the
     * image, its layout and its format. */
    PFN_vkCmdPipelineBarrier   barrier;
    PFN_vkCmdCopyImageToBuffer copy_image_to_buffer;
    PFN_vkCreateBuffer         create_buffer;
    PFN_vkDestroyBuffer        destroy_buffer;
    PFN_vkGetBufferMemoryRequirements buffer_reqs;
    PFN_vkBindBufferMemory     bind_buffer;
    PFN_vkMapMemory            map_memory;
    PFN_vkUnmapMemory          unmap_memory;
    PFN_vkQueueSubmit          submit;
    PFN_vkWaitForFences        wait_fences;
    PFN_vkResetFences          reset_fences;
    PFN_vkCreateFence          create_fence;
    PFN_vkDestroyFence         destroy_fence;
    PFN_vkCreateCommandPool    create_pool;
    PFN_vkDestroyCommandPool   destroy_pool;
    PFN_vkAllocateCommandBuffers alloc_cmds;
    PFN_vkBeginCommandBuffer   begin_cmd;
    PFN_vkEndCommandBuffer     end_cmd;
    VkQueue                    queue;
};

/* `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr`, resolved through the C side and called here.
 *
 * **`VKAPI_PTR` is load-bearing, and only on x86.** Vulkan's entry points are `__stdcall` there,
 * which is what `VKAPI_PTR` expands to; declaring the pointer without it makes it `__cdecl`, and
 * every call unbalances the stack by the size of the two arguments it never cleaned up. On x64 every
 * convention is the same, so the code was correct for two years on every 64-bit game and crashed a
 * 32-bit one - Dead or Alive 5, through DXVK, about four seconds in and right after DXVK creates the
 * swapchain, at an address in no module. That is what a corrupted return address looks like once the
 * corruption reaches a `ret`.
 *
 * **Two typedefs, not one.** They take different first arguments - `VkDevice` for the device
 * resolver, `VkInstance` for the instance one - and on x86 both of those are `uint64_t` too, so a
 * single `void *` signature that happened to compile was silently truncating them as well. The C
 * side passes both through `void *`, which is why nothing complained until the calling convention
 * was fixed and the compiler could see the mismatch. */
typedef PFN_vkVoidFunction (VKAPI_PTR *nrex_gpa_device_fn)(VkDevice device, const char *name);
typedef PFN_vkVoidFunction (VKAPI_PTR *nrex_gpa_instance_fn)(VkInstance instance, const char *name);

/* The Vulkan renderer's state, after the part every renderer shares (nr_menu_ui_backend.h). */
struct nrex_vk_ui : nrex_ui {
    VkDevice              device;
    VkPhysicalDevice      physical;
    uint32_t              queue_family;
    uint32_t              format;
    nrex_vk               vk;

    /* The UI's own colour attachment, swapchain-sized, recreated on resize. */
    VkImage               image;
    VkDeviceMemory        image_mem;
    VkImageView           image_view;
    VkRenderPass          pass;
    VkFramebuffer         fb;
    uint32_t              width, height;
};

/* ------------------------------------------------------------------ helpers */

static bool nrex_find_memory(nrex_vk_ui *u, VkMemoryRequirements reqs, VkFlags want,
                             uint32_t *type_out)
{
    VkPhysicalDeviceMemoryProperties props;
    u->vk.mem_props(u->physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            *type_out = i;
            return true;
        }
    return false;
}

/* The intermediate: colour attachment for ImGui, transfer source for the blend-copy. */
static bool nrex_make_target(nrex_vk_ui *u, uint32_t w, uint32_t h, const char **why)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)u->format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* STORAGE as well as the colour attachment, and that is not decoration: the menu shader reads
     * this image through an image2D declared as a storage image, so a descriptor naming it as one
     * is only valid if the image was created with that usage. Without it the blend pass binds an
     * image the driver was never told could be read that way, and the result is the null
     * dereference this menu kept arriving at. TRANSFER_SRC stays for a plain copy path. */
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (u->vk.create_image(u->device, &ici, nullptr, &u->image) != VK_SUCCESS) {
        *why = "no image for the menu UI";
        return false;
    }
    VkMemoryRequirements reqs;
    u->vk.img_reqs(u->device, u->image, &reqs);
    uint32_t type;
    if (!nrex_find_memory(u, reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        *why = "no device-local memory for the menu UI";
        return false;
    }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0};
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = type;
    if (u->vk.alloc_mem(u->device, &mai, nullptr, &u->image_mem) != VK_SUCCESS ||
        u->vk.bind_img(u->device, u->image, u->image_mem, 0) != VK_SUCCESS) {
        *why = "cannot bind the menu UI's image";
        return false;
    }

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0};
    vci.image = u->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)u->format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (u->vk.create_view(u->device, &vci, nullptr, &u->image_view) != VK_SUCCESS) {
        *why = "no view for the menu UI's image";
        return false;
    }

    /* One colour attachment, stored, load = clear (ImGui covers every pixel it cares about and
     * the rest is transparent - the clear colour is 0 alpha), store = keep. */
    VkAttachmentDescription att = {};
    att.format = (VkFormat)u->format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    /* GENERAL as the initial layout too: that is where the previous frame's pass left it, and
     * naming a different layout than the previous finalLayout is the same class of mismatch. The
     * first frame's contents are undefined and the load op clears. */
    att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    /* GENERAL, because the menu shader reads this image as a STORAGE image and that is the only
     * layout a storage image may be in. The alternative - SHADER_READ_ONLY plus a barrier before
     * every blend - buys nothing here: the image is written once a frame and read once a frame.
     * A render pass performs the transition to finalLayout itself. */
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo pci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0};
    pci.attachmentCount = 1;
    pci.pAttachments = &att;
    pci.subpassCount = 1;
    pci.pSubpasses = &sub;
    if (u->vk.create_pass(u->device, &pci, nullptr, &u->pass) != VK_SUCCESS) {
        *why = "no render pass for the menu UI";
        return false;
    }

    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0};
    fci.renderPass = u->pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &u->image_view;
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    if (u->vk.create_fb(u->device, &fci, nullptr, &u->fb) != VK_SUCCESS) {
        *why = "no framebuffer for the menu UI";
        return false;
    }

    u->width = w;
    u->height = h;
    return true;
}

static void nrex_drop_target(nrex_vk_ui *u)
{
    /* VK_NULL_HANDLE, not nullptr. vulkan_core.h defines the handles as pointers only when
     * VK_USE_64_BIT_PTR_DEFINES is 1, which vk_platform.h sets from the pointer width - true on
     * amd64, false on x86, where every handle is a uint64_t instead. The x86 build then rejects
     * `nullptr` as a conversion to an integer type and the whole layer fails to compile for
     * 32-bit games. VK_NULL_HANDLE is 0 in that case and nullptr in the other, so one spelling
     * builds on both. */
    if (u->fb)          { u->vk.destroy_fb(u->device, u->fb, nullptr); u->fb = VK_NULL_HANDLE; }
    if (u->pass)        { u->vk.destroy_pass(u->device, u->pass, nullptr); u->pass = VK_NULL_HANDLE; }
    if (u->image_view)  { u->vk.destroy_view(u->device, u->image_view, nullptr); u->image_view = VK_NULL_HANDLE; }
    if (u->image)       { u->vk.destroy_image(u->device, u->image, nullptr); u->image = VK_NULL_HANDLE; }
    if (u->image_mem)   { u->vk.free_mem(u->device, u->image_mem, nullptr); u->image_mem = VK_NULL_HANDLE; }
}

/* The load functions, kept so the header's contract has a home; everything resolves at create.
 * The instance is kept as `VkInstance` rather than `void *` for the same reason the loader context
 * is: it is 64 bits on x86 and a pointer is 32. */
static nrex_gpa_device_fn g_saved_gpa = nullptr;
static nrex_gpa_instance_fn g_saved_gpa_i = nullptr;
static VkInstance g_saved_instance = VK_NULL_HANDLE;
extern "C" void *nrex_saved_gpa(void) { return (void *)(uintptr_t)g_saved_gpa; }

/* A stub for the window-system commands the backend's table names but this menu never calls. */
static void VKAPI_PTR nrex_noop(void) {}

/* The Vulkan backend's step for each frame, before ImGui's own. */
static void nrex_vk_new_frame(nrex_ui *ui)
{
    (void)ui;
    ImGui_ImplVulkan_NewFrame();
}

/* ------------------------------------------------------------------ creation */

extern "C" NREX_API nrex_ui *nrex_create(void *device_v, void *physical_v, void *instance_v,
                                         void *queue_v, uint32_t queue_family, uint32_t format,
                                         uint32_t api_version,
                                         uint32_t min_image_count, uint32_t image_count,
                                         void *load_fn, void *load_instance_fn,
                                         const char *font_bytes, size_t font_size,
                                         const int *codepoints, int codepoint_count,
                                         const char **why)
{
    static const char *last_why = "the menu UI could not start";
    if (!why) why = &last_why;

    nrex_vk_ui *u = new (std::nothrow) nrex_vk_ui();
    if (!u) { *why = "out of memory for the menu UI"; return nullptr; }
    u->backend_new_frame = nrex_vk_new_frame;
    u->device = (VkDevice)device_v;
    u->physical = (VkPhysicalDevice)physical_v;
    u->queue_family = queue_family;
    u->format = format;

    /* The two resolvers arrive as `void *` because that is what the plain-C interface can name.
     * They are cast to their real types here, once, and every later call goes through the typed
     * pointer - which is what makes the calling convention and the handle width visible to the
     * compiler instead of silently wrong. */
    nrex_gpa_device_fn gpa = (nrex_gpa_device_fn)(uintptr_t)load_fn;
    if (!gpa) { *why = "no vkGetDeviceProcAddr for the menu UI"; delete u; return nullptr; }
    g_saved_gpa = gpa;
    nrex_gpa_instance_fn gpa_i = (nrex_gpa_instance_fn)(uintptr_t)load_instance_fn;
    if (!gpa_i) { *why = "no vkGetInstanceProcAddr for the menu UI"; delete u; return nullptr; }
    g_saved_gpa_i = gpa_i;
    g_saved_instance = (VkInstance)(uintptr_t)instance_v;   /* a real instance: instance-level commands refuse a NULL one */
#define NREX_RESOLVE(field, name) \
    u->vk.field = reinterpret_cast<decltype(u->vk.field)>(gpa(u->device, name)); \
    if (!u->vk.field) { *why = "the device has no " name; delete u; return nullptr; }
    NREX_RESOLVE(create_image, "vkCreateImage")
    NREX_RESOLVE(destroy_image, "vkDestroyImage")
    NREX_RESOLVE(alloc_mem, "vkAllocateMemory")
    NREX_RESOLVE(free_mem, "vkFreeMemory")
    NREX_RESOLVE(bind_img, "vkBindImageMemory")
    NREX_RESOLVE(img_reqs, "vkGetImageMemoryRequirements")
    /* The physical-device entry points are instance-level; the device proc addr returns them too
     * on every loader this layer supports. */
    /* Instance-level: the device resolver returns NULL for it by the loader's contract, so this
     * one goes to the instance resolver with the instance it was given. */
    u->vk.mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gpa_i(g_saved_instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!u->vk.mem_props) {
        NREX_ERROR("[menu ui] instance-resolver gave NULL for vkGetPhysicalDeviceMemoryProperties\n");
        *why = "no vkGetPhysicalDeviceMemoryProperties"; delete u; return nullptr;
    }
    NREX_RESOLVE(create_view, "vkCreateImageView")
    NREX_RESOLVE(destroy_view, "vkDestroyImageView")
    NREX_RESOLVE(create_pass, "vkCreateRenderPass")
    NREX_RESOLVE(destroy_pass, "vkDestroyRenderPass")
    NREX_RESOLVE(create_fb, "vkCreateFramebuffer")
    NREX_RESOLVE(destroy_fb, "vkDestroyFramebuffer")
    NREX_RESOLVE(begin_pass, "vkCmdBeginRenderPass")
    NREX_RESOLVE(end_pass, "vkCmdEndRenderPass")
#undef NREX_RESOLVE

    /* The dump path, resolved best-effort: the panel must work whether or not these are there, so a
     * miss disables the dump rather than failing creation. */
#define NREX_TRY(field, name) \
    u->vk.field = reinterpret_cast<decltype(u->vk.field)>(gpa(u->device, name));
    NREX_TRY(barrier, "vkCmdPipelineBarrier")
    NREX_TRY(copy_image_to_buffer, "vkCmdCopyImageToBuffer")
    NREX_TRY(create_buffer, "vkCreateBuffer")
    NREX_TRY(destroy_buffer, "vkDestroyBuffer")
    NREX_TRY(buffer_reqs, "vkGetBufferMemoryRequirements")
    NREX_TRY(bind_buffer, "vkBindBufferMemory")
    NREX_TRY(map_memory, "vkMapMemory")
    NREX_TRY(unmap_memory, "vkUnmapMemory")
    NREX_TRY(submit, "vkQueueSubmit")
    NREX_TRY(wait_fences, "vkWaitForFences")
    NREX_TRY(reset_fences, "vkResetFences")
    NREX_TRY(create_fence, "vkCreateFence")
    NREX_TRY(destroy_fence, "vkDestroyFence")
    NREX_TRY(create_pool, "vkCreateCommandPool")
    NREX_TRY(destroy_pool, "vkDestroyCommandPool")
    NREX_TRY(alloc_cmds, "vkAllocateCommandBuffers")
    NREX_TRY(begin_cmd, "vkBeginCommandBuffer")
    NREX_TRY(end_cmd, "vkEndCommandBuffer")
#undef NREX_TRY
    u->vk.queue = (VkQueue)queue_v;

    if (!nrex_make_target(u, 640, 360, why)) { delete u; return nullptr; }
    NREX_TRACE("[ui] target made\n");

    /* The panel's ImGui context, which every renderer shares (nr_menu_ui.cpp). */
    if (!nrex_context_create(font_bytes, font_size, codepoints, codepoint_count, why)) {
        nrex_drop_target(u);
        delete u;
        return nullptr;
    }

    ImGui_ImplVulkan_InitInfo init = {};
    init.ApiVersion = api_version;
    init.PhysicalDevice = u->physical;
    init.Device = u->device;
    init.QueueFamily = queue_family;
    /* A real queue, resolved by the caller. Leaving this NULL makes the backend call
     * vkGetDeviceQueue itself and then wait forever on the font upload, because a fence on the
     * presenting family completes only when the layer's own present semantics allow it - the game
     * hung inside its present exactly there. */
    init.Queue = (VkQueue)queue_v;
    init.DescriptorPoolSize = 16;             /* its own pool, so nothing of the game's is touched */
    init.MinImageCount = min_image_count;
    init.ImageCount = image_count;
    init.PipelineInfoMain.RenderPass = u->pass;
    init.UseDynamicRendering = false;
    init.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
            NREX_ERROR("[menu ui] ImGui Vulkan backend: VkResult %d\n", (int)err);
    };
    NREX_TRACE("[ui] loading backend functions through the layer's resolvers\n");
    /* Device commands first - nearly all of them - and instance commands from the instance
     * resolver when the device one declines, which is the case for the handful of physical-device
     * queries: a resolver handed no instance refuses them, so it is the second resolver's job.
     *
     * The context is `uint64_t` slots rather than `void *`, because two of the four things in it
     * are handles that are 64 bits on x86 and 32 bits as a pointer: `VkDevice` and `VkInstance` are
     * non-dispatchable, so `void *ctx[]` truncated them and the resolver was called with half a
     * device. Function pointers are the other two, and they fit either way. */
    struct NrexLoader {
        static PFN_vkVoidFunction load(const char *name, void *user) {
            uint64_t *ctx = (uint64_t *)user;
            nrex_gpa_device_fn gpa_dev = (nrex_gpa_device_fn)(uintptr_t)ctx[0];
            nrex_gpa_instance_fn gpa_ins = (nrex_gpa_instance_fn)(uintptr_t)ctx[1];
            PFN_vkVoidFunction f = gpa_dev((VkDevice)ctx[2], name);
            if (!f) f = gpa_ins((VkInstance)ctx[3], name);
            if (!f) {
                /* Window-system functions (vkDestroySurfaceKHR and friends) come from the
                 * loader's global entry point, which the instance resolver has already covered;
                 * a loader that declines them anyway is one the layer cannot satisfy, and the
                 * backend only needs them for swapchain helpers this menu does not use. A stub
                 * that does nothing is the honest answer: the menu never creates a surface. */
                if (strstr(name, "SurfaceKHR")) f = (PFN_vkVoidFunction)(uintptr_t)&nrex_noop;
            }
            NREX_TRACE("[ui]   load '%s' -> %p\n", name, (void *)f);
            return f;
        }
    };
    /* `uint64_t` slots, not `void *`: two of the four are 64-bit handles on x86. */
    uint64_t loaders[4] = { (uint64_t)(uintptr_t)g_saved_gpa, (uint64_t)(uintptr_t)g_saved_gpa_i,
                            (uint64_t)u->device, (uint64_t)g_saved_instance };
    if (!ImGui_ImplVulkan_LoadFunctions(init.ApiVersion, &NrexLoader::load, loaders)) {
        *why = "the ImGui Vulkan backend could not load its commands";
        ImGui::DestroyContext();
        nrex_drop_target(u);
        delete u;
        return nullptr;
    }
    NREX_TRACE("[ui] backend functions loaded\n");
    NREX_TRACE("[ui] init backend (queue=%p)\n", (void *)init.Queue);
    if (!ImGui_ImplVulkan_Init(&init)) {
        *why = "the ImGui Vulkan backend would not start";
        ImGui::DestroyContext();
        nrex_drop_target(u);
        delete u;
        return nullptr;
    }

    NREX_TRACE("[ui] backend started\n");
    return u;
}

extern "C" NREX_API void nrex_destroy(nrex_ui *base)
{
    nrex_vk_ui *ui = static_cast<nrex_vk_ui *>(base);
    if (!ui) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    nrex_drop_target(ui);
    delete ui;
}

/* ------------------------------------------------------------------ verification dump */

/* A throwaway command buffer for the dump.
 *
 * The dump runs after `nrex_render` has already closed this frame's command buffer, and the C side
 * submits that one immediately afterwards, so the dump cannot reuse it. `vkCmdCopyImageToBuffer`
 * also needs the image in a transfer layout, which is a barrier the panel's own pass does not
 * contain. A pool of its own, created and destroyed around the single copy, keeps the dump from
 * touching anything the running frame owns. */
static bool nrex_oneshot_begin(nrex_vk_ui *u, VkCommandPool *pool, VkCommandBuffer *cmd)
{
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = u->queue_family;
    if (u->vk.create_pool(u->device, &pci, nullptr, pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0};
    ai.commandPool = *pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (u->vk.alloc_cmds(u->device, &ai, cmd) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return u->vk.begin_cmd(*cmd, &bi) == VK_SUCCESS;
}

static bool nrex_oneshot_submit(nrex_vk_ui *u, VkCommandBuffer cmd, VkFence fence)
{
    if (u->vk.end_cmd(cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (u->vk.submit(u->vk.queue, 1, &si, fence) != VK_SUCCESS) return false;
    return u->vk.wait_fences(u->device, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
}

static void nrex_oneshot_end(nrex_vk_ui *u, VkCommandPool pool)
{
    if (pool) u->vk.destroy_pool(u->device, pool, nullptr);
}

/* `NR_MENU_DUMP=1` (or a path) writes what the panel drew to a PPM, once.
 *
 * A Vulkan swapchain image cannot be screen-captured: the desktop compositor never sees it, and a
 * window capture of a Vulkan game returns a black or stale frame. This copies the UI's own
 * intermediate - which holds exactly the panel's pixels, before the blend onto the game - so
 * "the panel renders correctly" can be checked by looking at it instead of by inference.
 *
 * PPM because it needs no encoder and no dependency: a short header and then the raw bytes, which
 * every image viewer and this project's own tools read. */
static void nrex_dump_ui(nrex_vk_ui *u)
{
    const char *env = getenv("NR_MENU_DUMP");
    if (!env || !*env) return;
    /* Not on the first frames: the panel's first composite happens before ImGui has any content to
     * draw, so an immediate dump is a fully transparent image that says nothing. Let a few frames
     * pass so the font atlas and the widgets are in place, then write once - a single write, so a
     * reader cannot catch a half-written file. */
    static int frames = 0;
    static bool dumped = false;
    if (dumped) return;
    if (++frames < 30) return;
    dumped = true;
    if (!u->vk.barrier || !u->vk.copy_image_to_buffer || !u->vk.create_buffer ||
        !u->vk.buffer_reqs || !u->vk.bind_buffer || !u->vk.map_memory || !u->vk.submit ||
        !u->vk.wait_fences || !u->vk.create_fence || !u->vk.create_pool || !u->vk.alloc_cmds ||
        !u->vk.begin_cmd || !u->vk.end_cmd)
        return;

    const VkDeviceSize bytes = (VkDeviceSize)u->width * u->height * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (u->vk.create_buffer(u->device, &bci, nullptr, &buf) != VK_SUCCESS) return;

    VkMemoryRequirements req;
    u->vk.buffer_reqs(u->device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    u->vk.mem_props(u->physical, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) { u->vk.destroy_buffer(u->device, buf, nullptr); return; }

    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (u->vk.alloc_mem(u->device, &mai, nullptr, &mem) != VK_SUCCESS) {
        u->vk.destroy_buffer(u->device, buf, nullptr);
        return;
    }
    if (u->vk.bind_buffer(u->device, buf, mem, 0) != VK_SUCCESS) return;

    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0};
    VkFence fence = VK_NULL_HANDLE;
    if (u->vk.create_fence(u->device, &fci, nullptr, &fence) != VK_SUCCESS) return;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!nrex_oneshot_begin(u, &pool, &cmd)) return;
    if (cmd) {
        /* The image is in COLOR_ATTACHMENT_OPTIMAL when `nrex_render` returns; the task below
         * migrates it to TRANSFER_SRC and back, leaving it exactly as the next frame expects. */
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = u->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        u->vk.barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {u->width, u->height, 1};
        u->vk.copy_image_to_buffer(cmd, u->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                   &region);

        VkImageMemoryBarrier back = b;
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        u->vk.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                      &back);

        if (nrex_oneshot_submit(u, cmd, fence)) {
            void *mapped = nullptr;
            if (u->vk.map_memory(u->device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
                const char *path = (env[0] == '1' && env[1] == '\0') ? "nr-menu-dump.ppm" : env;
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", u->width, u->height);
                    const unsigned char *px = (const unsigned char *)mapped;
                    /* The intermediate carries the swapchain's format, so a 10-bit surface has to
                     * be widened rather than read as bytes: reading R10G10B10A2 as four bytes gives
                     * a plausible-looking but wrong picture, which is worse than none. */
                    const bool ten_bit = (u->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                    std::vector<unsigned char> row((size_t)u->width * 3);
                    for (uint32_t y = 0; y < u->height; ++y) {
                        for (uint32_t x = 0; x < u->width; ++x) {
                            unsigned char r, g, bl;
                            if (ten_bit) {
                                uint32_t p;
                                memcpy(&p, px + ((size_t)y * u->width + x) * 4, 4);
                                r  = (unsigned char)(((p >>  0) & 0x3FFu) >> 2);
                                g  = (unsigned char)(((p >> 10) & 0x3FFu) >> 2);
                                bl = (unsigned char)(((p >> 20) & 0x3FFu) >> 2);
                            } else {
                                const unsigned char *p = px + ((size_t)y * u->width + x) * 4;
                                r = p[0]; g = p[1]; bl = p[2];
                            }
                            row[(size_t)x * 3 + 0] = r;
                            row[(size_t)x * 3 + 1] = g;
                            row[(size_t)x * 3 + 2] = bl;
                        }
                        fwrite(row.data(), 3, u->width, f);
                    }
                    fclose(f);
                    /* A NREX_ERROR rather than a trace, and the only one that is not a failure:
                     * this line can only run when somebody set NR_MENU_DUMP, and the whole point of
                     * that switch is to be told where the file went. Silence would defeat it. */
                    NREX_ERROR("[ui] wrote %s (%ux%u, %s)\n", path, u->width, u->height,
                               ten_bit ? "rgb10a2" : "rgba8");
                }
                u->vk.unmap_memory(u->device, mem);
            }
        }
    }
    nrex_oneshot_end(u, pool);
    if (fence) u->vk.destroy_fence(u->device, fence, nullptr);
    u->vk.free_mem(u->device, mem, nullptr);
    u->vk.destroy_buffer(u->device, buf, nullptr);
}

extern "C" NREX_API void nrex_render(nrex_ui *base, uint64_t cmd_handle)
{
    nrex_vk_ui *ui = static_cast<nrex_vk_ui *>(base);
    if (!ui || !ui->in_frame) return;
    ImGui::Render();
    VkCommandBuffer cmd = (VkCommandBuffer)(uintptr_t)cmd_handle;

    /* Clear to transparent, then draw. The blend-copy on the C side keeps alpha 0 pixels
     * invisible over the game. */
    VkClearValue clear = {};
    clear.color = {{0.f, 0.f, 0.f, 0.f}};
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0};
    bi.renderPass = ui->pass;
    bi.framebuffer = ui->fb;
    bi.renderArea = {{0, 0}, {ui->width, ui->height}};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    ui->vk.begin_pass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    ui->vk.end_pass(cmd);
    ui->in_frame = false;

    nrex_dump_ui(ui);
}

/* `uint64_t`, not `void *`: on x86 a Vulkan non-dispatchable handle is 64 bits and a pointer is 32,
 * so a `void *` return dropped the top half of `VkImage`/`VkImageView`. See the header. */
extern "C" NREX_API uint64_t nrex_image(nrex_ui *ui) { return ui ? (uint64_t)static_cast<nrex_vk_ui *>(ui)->image : 0; }
/* The view, for the blend pass: the shader samples the UI through an image view, and the caller
 * has no other way to reach one it did not create. */
extern "C" NREX_API uint64_t nrex_image_view(nrex_ui *ui) { return ui ? (uint64_t)static_cast<nrex_vk_ui *>(ui)->image_view : 0; }
extern "C" NREX_API void *nrex_image_layout_transition(nrex_ui *ui) { (void)ui; return nullptr; }
extern "C" NREX_API uint32_t nrex_image_width(nrex_ui *ui) { return ui ? static_cast<nrex_vk_ui *>(ui)->width : 0; }
extern "C" NREX_API uint32_t nrex_image_height(nrex_ui *ui) { return ui ? static_cast<nrex_vk_ui *>(ui)->height : 0; }
extern "C" NREX_API int nrex_resize(nrex_ui *base, uint32_t w, uint32_t h)
{
    nrex_vk_ui *ui = static_cast<nrex_vk_ui *>(base);
    if (!ui) return 0;
    if (ui->width == w && ui->height == h) return 1;
    nrex_drop_target(ui);
    const char *why;
    return nrex_make_target(ui, w, h, &why) ? 1 : 0;
}
