/* The ImGui-based menu, in the one C++ translation unit.
 *
 * Everything here is behind the plain-C interface in nr_menu_ui.h. The style follows OptiScaler's
 * settings window - a single dark panel, a scrolled list of labelled controls with the value on the
 * right, a footer of action buttons - which is read off its screenshots and its layout code, not
 * copied from it: OptiScaler is GPL-3.0 and this file is new work in an Apache-2.0 tree. What is
 * borrowed is the arrangement and the ImGui idiom, which is what ImGui's own demo shows anyway.
 *
 * The frame path, once more because it is the part that has to survive contact with a layer:
 *
 *   ImGui renders (CPU) -> the backend draws into `intermediate` (a colour attachment this file
 *   owns) -> the caller blend-copies `intermediate` onto the game's swapchain image (a compute
 *   dispatch in nr_menu.c, which has the storage-image access and the semaphore contract).
 *
 * The intermediate exists because the swapchain image was created without COLOR_ATTACHMENT usage
 * and a layer cannot retro-fit usage flags onto a game's image.
 *
 * Input: the C side polls GetAsyncKeyState - a layer has no window and no message pump - and feeds
 * this file through nrex_key/nrex_mouse. ImGui derives edges itself, so holding a key is holding it.
 */
#include "nr_menu_ui.h"

#include "imgui.h"
/* For `SetKeyOwner` only.
 *
 * It is `[Internal]` in 1.93, but the public shortcut `SetItemKeyOwner` registers an owner only
 * while the item is hovered or active, and this needs the key claimed while the item is merely
 * FOCUSED - which is the case the arrows have to work in. The header is a stock part of the
 * vendored tree; nothing in `third_party/imgui` is patched, and `src/tools/check_imgui_pristine.py`
 * still passes. */
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#ifndef _WIN32
#include <time.h>    /* CLOCK_MONOTONIC, for nrex_now_seconds */
#endif

/* ------------------------------------------------------------------ diagnostics
 *
 * Two kinds of message, and they must not be confused.
 *
 * NREX_ERROR is a real failure the person running the game needs to know about: a backend that
 * would not start, an image that would not resize. It always prints.
 *
 * NREX_TRACE is development chatter - "backend functions loaded", "queue idle", the pointer's
 * position every time it moves. It used to print unconditionally, which meant a released layer wrote
 * a line to the game's stderr on every mouse move and dumped Vulkan function pointers and a queue
 * handle while starting. That is noise at best and a small information leak at worst, in somebody
 * else's process. It compiles to nothing unless NREX_TRACE_LOGGING is defined, which is how this
 * file was developed and is not how it ships.
 *
 * The one that mattered most is the pointer trace: it was per-move, unthrottled, and it is what
 * made the menu feel like it was doing work while the pointer was nowhere near it. */
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

struct nrex_ui {
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

    bool                  in_frame;
};

/* ------------------------------------------------------------------ helpers */

static bool nrex_find_memory(nrex_ui *u, VkMemoryRequirements reqs, VkFlags want,
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
static bool nrex_make_target(nrex_ui *u, uint32_t w, uint32_t h, const char **why)
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

static void nrex_drop_target(nrex_ui *u)
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

    nrex_ui *u = new (std::nothrow) nrex_ui();
    if (!u) { *why = "out of memory for the menu UI"; return nullptr; }
    memset(u, 0, sizeof *u);
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
    setvbuf(stderr, NULL, _IONBF, 0);   /* a layer's stderr is redirected; block buffering ate
                                             every log after the last flush and made the freeze
                                             look like it happened somewhere it did not */
    NREX_TRACE("[ui] target made\n");

    NREX_TRACE("[ui] creating ImGui context\n");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    NREX_TRACE("[ui] context ok, loading font\n");
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;              /* a layer must not write an imgui.ini beside the game */

    /* Key repeat, for the keyboard-navigation half of the panel only.
     *
     * This decides how fast a held arrow walks FOCUS from row to row (and Tab through the items).
     * It does not touch value stepping inside a row: that is `nrex_repeat` below, with a delay and
     * an acceleration of its own, because one fixed rate cannot both leave a single click alone and
     * cross a knob's whole range in a second.
     *
     * Windows' defaults are 0.500 / 0.031. The delay is pulled in a little so a held key does not
     * read as dead, and the rate is left near enough to the default that a held Tab does not blur. */
    io.KeyRepeatDelay = 0.300f;
    io.KeyRepeatRate  = 0.045f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* OptiScaler's look, read off its window: dark, slightly blue, compact rows. */
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.GrabRounding = 3.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 4);
    st.ScrollbarSize = 12.0f;
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.07f, 0.09f, 0.97f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.13f, 0.19f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f, 0.32f, 0.42f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.45f, 0.70f, 0.95f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.42f, 0.58f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.45f, 0.75f, 0.95f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);

    /* The font. The C side hands over a TTF and the glyph ranges the four languages need; without
     * it the embedded proggy covers ASCII only, which is fine for English and useless for the
     * rest - the exact hole the hand-drawn menu fell into. */
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 1;
    const ImWchar *ranges = nullptr;
    static ImVector<ImWchar> built_ranges;
    if (codepoints && codepoint_count > 0) {
        /* ImGui wants pairs of (first, last), terminated by 0. The caller hands over the exact
         * codepoints the menu's strings use, sorted; each run of consecutive values becomes one
         * pair. That is a few hundred glyphs instead of the twenty-one thousand a whole CJK block
         * asks for - and a block that large does not fit one atlas, which is what made every glyph
         * blank rather than only the Chinese ones. */
        built_ranges.clear();
        built_ranges.push_back(0x20);   /* always keep printable ASCII: labels, numbers, units */
        built_ranges.push_back(0x7E);
        int run_start = -1, previous = -2;
        for (int i = 0; i < codepoint_count; ++i) {
            int cp = codepoints[i];
            if (cp < 0x20 || cp == 0x7F) continue;
            if (cp == previous) continue;
            if (cp == previous + 1) { previous = cp; continue; }
            if (run_start >= 0) {
                built_ranges.push_back((ImWchar)run_start);
                built_ranges.push_back((ImWchar)previous);
            }
            run_start = cp;
            previous = cp;
        }
        if (run_start >= 0) {
            built_ranges.push_back((ImWchar)run_start);
            built_ranges.push_back((ImWchar)previous);
        }
        built_ranges.push_back(0);
        ranges = built_ranges.Data;
    }
    if (font_bytes && font_size)
        io.Fonts->AddFontFromMemoryTTF((void *)font_bytes, (int)font_size, 16.0f, &fc, ranges);
    else
        io.Fonts->AddFontDefault();

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

extern "C" NREX_API void nrex_destroy(nrex_ui *ui)
{
    if (!ui) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    nrex_drop_target(ui);
    delete ui;
}

/* ------------------------------------------------------------------ frames */

/* Defined with the cursor, below, and called here - which is the only place it can be called from:
 * `NewFrame` is what turns the queued position events into `io.MousePos`, so the fallback has to go
 * in after it and before anything is drawn. */
static void nrex_place_software_cursor(nrex_ui *ui);

extern "C" NREX_API void nrex_new_frame(nrex_ui *ui, float w, float h, float dt)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    nrex_place_software_cursor(ui);
    ui->in_frame = true;
}

/* The rows the panel is drawing this frame, so a widget can keep its own value while it is being
 * edited. Sized to the most rows any menu has. */
#define NREX_MAX_ROWS 24

/* The panel's language, as the selector left it. Read back by the caller, which asks the menu for
 * the change - the same flag the L key sets. */
static int g_language = 0;

/* The panel-level line the caller hands in each frame: the panel's keys, and the fact that the game
 * still receives them. Empty draws nothing. 256 is the longest of the four languages' hint plus
 * room - the Chinese is the longest and it wraps in the footer rather than being cut. */
static char g_panel_hint[256] = "";

static nrex_ui *g_ui = nullptr;
static int g_actions = 0;
/* The effect's state, as the layer reported it for this frame, and what the user asked for by
 * clicking the checkbox. -1 in g_nr_enabled means "this install has no trigger path", which is
 * drawn differently from "off": one is a state, the other is the absence of one. */
static int g_nr_enabled = 0;
static int g_nr_request = -1;
/* The caller's row array for this frame. The widgets write their edits into it directly, which is
 * what lets the caller read them back after `nrex_end_panel` - a local copy would lose them, and
 * that lost copy is why a dragged slider changed nothing. */
static nrex_row *g_rows = nullptr;

/* The knob's explanation, shown while the pointer rests on the row just submitted.
 *
 * Call it immediately after the item the tip belongs to: `IsItemHovered` asks about the last item,
 * which is why this takes no argument and reads the hint separately - the item is implicit.
 *
 * OptiScaler shows a tooltip on hover and it is the difference between a settings list you can use
 * and a list of numbers with names beside them; nobody should have to leave the game to find out
 * what "hold unchanged pixels" does. The text comes from the same generated table the installer's
 * tooltips use, so the two explain a knob with the same words in the same language.
 *
 * A NULL or empty hint draws nothing, which is what the display-only rows want - they are their own
 * explanation.
 *
 * The wrap position is an ABSOLUTE local X, which is how ImGui's own demo writes it
 * (`PushTextWrapPos(GetFontSize() * 35.0f)`). Two earlier attempts in this file were both wrong and
 * both measured: `0.0f` is the very start of the line, so every glyph wrapped onto its own line; and
 * `GetCursorPos().x + 420.0f` inside a freshly-begun tooltip is the wrap point plus 420, past which
 * nothing is drawn - the tip came out 16x12 pixels, an empty window with a visible frame. 320 pixels
 * is about 40 characters at this font size, which wraps the longest hint over four lines. */
static void nrex_hint_tooltip(const char *hint)
{
    if (!hint || !hint[0]) return;
    if (!ImGui::IsItemHovered()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(320.0f);
    ImGui::TextUnformatted(hint);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

/* ------------------------------------------------- hold-to-repeat for the arrow keys
 *
 * How a held Left/Right steps a value, and why it is not ImGui's repeat.
 *
 * ImGui has a typematic repeat of its own, and 1.93 exposes presets for it
 * (`ImGuiInputFlags_RepeatRate*`), but every one of them is a FIXED rate behind one global delay.
 * A settings slider is exactly the shape that breaks, and it breaks in both directions at once:
 *
 *   - the stock 500 ms delay means the first repeat of a hold arrives late enough that the key reads
 *     as "not working at all" (the report: press and hold, nothing happens, give up);
 *   - winding that delay down globally makes a click land on the repeat, so one tap steps two or
 *     three values instead of one, and the number jumps about instead of walking.
 *
 * So the repeat is computed here, as two rules rather than one number:
 *
 *   click   the press steps exactly once, immediately - no timer involved;
 *   hold    nothing further for `NREX_REPEAT_WARMUP`, then an interval that starts at
 *           `NREX_REPEAT_FIRST` and shrinks by `NREX_REPEAT_ACCEL` on every repeat down to
 *           `NREX_REPEAT_MIN`.
 *
 * A 175 ms press - longer than most clicks - is therefore still one step and only one, while a
 * third of a second of holding is already walking at a useful rate. The ceiling is what matters:
 * the first version of this reached ~75 steps/s, which is a knob that crosses its whole range in a
 * quarter second and cannot be stopped on a number - the report that sent this comment back. It
 * now tops out at 20 steps/s, so a 0.05..1.0 range (19 steps) takes about a second to cross while
 * still being quicker than tapping.
 *
 * Timed on a WALL CLOCK, not `ImGui::GetTime()`. ImGui's clock runs at whatever step the layer
 * feeds it - a fixed 1/60 once, now the real seconds between presents - so at 30 FPS every
 * interval would double here and at 144 FPS nearly halve. A hold is felt in real seconds, so it
 * is measured in them. */
#define NREX_REPEAT_WARMUP 0.140f
#define NREX_REPEAT_FIRST  0.075f
#define NREX_REPEAT_ACCEL  0.850f
#define NREX_REPEAT_MIN    0.050f      /* 20 steps/s at the ceiling */
#define NREX_REPEAT_BURST  2           /* steps one frame may apply, so a hitch cannot fling the value */

struct nrex_repeat {
    int    dir;      /* -1 = left, +1 = right, 0 = idle */
    float  interval; /* the gap the NEXT repeat is due after */
    double next;     /* when that repeat is due, in seconds */
};

static nrex_repeat s_repeat[NREX_MAX_ROWS];

/* When the panel thinks "now" is. Windows: the performance counter, which is what the value-repeat
 * arithmetic wants (a monotonic clock with sub-microsecond resolution, and one the OS keeps across
 * suspend). Off Windows: `CLOCK_MONOTONIC`, which is the same promise on POSIX.
 *
 * **Why this is here and not in the C side.** The repeat logic owns its own clock so that a frame
 * that takes 100 ms and one that takes 10 ms both repeat at the right rate - the caller's frame
 * time is an input to the panel, not a clock. One function, one place, no second definition of
 * "now" to drift. */
static double nrex_now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static int have_frequency;
    if (!have_frequency) { QueryPerformanceFrequency(&frequency); have_frequency = 1; }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* The panel: one window, a scrolled body of rows, a footer. OptiScaler's arrangement. */
static void nrex_draw_panel(nrex_ui *u, const char *title, int language,
                            const nrex_row *rows, int count, int selected)
{
    (void)u;
    /* Focus on the first frame, and keep it.
     *
     * ImGui's keyboard navigation only moves inside the FOCUSED window, and a layer has no message
     * pump to focus one the way a platform backend does. Without this the arrows reach `io` and are
     * dropped for want of a focused window - which is precisely "the arrow keys do nothing" while
     * the key trace shows them arriving. The panel is the only window the layer has, so it takes
     * focus whenever focus is free rather than only on the first frame.
     *
     * No `AlwaysAutoResize`: it overrides the requested size, so the window shrinks to its content
     * and ImGui's hit rectangles no longer match where the panel is drawn. A fixed size keeps the
     * mouse test and the picture in agreement.
     *
     * `NoNavInputs` is deliberately NOT set - the whole point is that the keyboard drives it. */
    /* The panel is sized against the PICTURE, not against a fixed number of pixels.
     *
     * It was a fixed 560x380, which was fine for eleven knobs and is not fine for fourteen: the
     * last three could not be reached, and the only way to see them was to drag the window
     * bigger. Doing that is what crashed the game - the window could be dragged past the edges of
     * the swapchain image it has to be drawn into, and ImGui happily drew outside it.
     *
     * So the default size is now "as much of the picture as the panel wants", and the size is
     * CLAMPED to the picture every frame. The clamp is the part that matters: a resize survives
     * across frames and across game window sizes, so a size that was legal once can be illegal
     * later - a user who drags the panel wide in a large window and then starts the game smaller
     * would otherwise be drawing off the image again. */
    {
        ImGuiIO &io = ImGui::GetIO();
        float want_w = io.DisplaySize.x - 48.0f;
        float want_h = io.DisplaySize.y - 48.0f;
        /* A ceiling as well as a floor: on a 1920x1080 picture "as tall as the picture" would be
         * a settings panel covering the game, and the point of an overlay is to leave something
         * to look at. 640x720 shows all fourteen rows with room to spare. */
        if (want_w > 640.0f) want_w = 640.0f;
        if (want_h > 720.0f) want_h = 720.0f;
        if (want_h < 240.0f) want_h = 240.0f;
        if (want_w < 200.0f) want_w = 200.0f;
        ImGui::SetNextWindowSize(ImVec2(want_w, want_h), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
        ImGui::SetNextWindowFocus();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    /* Movable, and that is deliberate.
     *
     * I pinned this with `NoMove` after a report that dragging moved the panel, and that was the
     * wrong fix: the panel is the only thing on screen the user can arrange, and taking the ability
     * away to avoid a stray press threw out the feature to dodge the bug. It has been put back.
     *
     * What made me think it was unsafe was an argument that does not hold: that a moved panel would
     * disagree with the hit test. It cannot - ImGui positions the window in the same UI-image pixels
     * the pointer arrives in, so a window at (400, 200) and a pointer at (450, 220) are in one
     * coordinate space, and ImGui does the hit test itself. `SetNextWindowPos` is `FirstUseEver`, so
     * wherever the user leaves it is where it stays.
     *
     * The stray press is real but it is not a reason to forbid moving: the panel only receives a
     * click while it is open, and while it is open a press on its own title bar is a press on the
     * panel. A press anywhere else belongs to the game and never reaches ImGui, because
     * `nrex_mouse` reports "no position" outside the UI image. */
    /* `NoResize`: the panel is sized to the picture, and a user-resized window that outgrows the
     * swapchain image makes ImGui draw outside the render target - which is an access violation in
     * the driver, not a warning. Dragging the border to see the last three rows is what crashed
     * the game.
     *
     * In exchange the panel is made tall enough to show everything without resizing (see the
     * default size above), and it keeps a scrollbar for the case where it is not.
     *
     * A per-frame clamp was tried first and is wrong: it has to run BEFORE `Begin` to have any
     * effect, and reading the window's current size there means `FindWindowByName` on a window
     * that has not been created yet on the first frame. It crashed the game on every INS press. */
    if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
        { ImGui::End(); ImGui::PopStyleVar(); return; }

    /* Home belongs to the effect toggle, not to ImGui's navigation.
     *
     * ImGui's keyboard nav treats Home as "jump to the first item", so pressing it with the panel
     * open moved the selection to the first slider and never reached the layer - the report was
     * "with the panel open, Home selects the first slider; with it closed, Home does nothing".
     * Claiming it for the window stops nav acting on it; the layer reads the physical key with
     * `GetAsyncKeyState` and does not care who ImGui thinks owns it. */
    ImGui::SetKeyOwner(ImGuiKey_Home, ImGui::GetID(title));

    /* Home belongs to the effect toggle, not to ImGui's navigation.
     *
     * ImGui's keyboard nav treats Home as "jump to the first item", so pressing it with the panel
     * open moved the selection to the first slider and never reached the layer - the report was
     * "with the panel open, Home selects the first slider; with it closed, Home does nothing".
     * Claiming it for the window stops nav acting on it; the layer reads the physical key with
     * `GetAsyncKeyState` and does not care who ImGui thinks owns it.
     *
     * `ImGuiKey_Home`, not the End key: the toggle is Home and the comparison it replaced was
     * Home, so there is no second key to keep clear here. */

    /* Three fixed columns, so the controls line up as a table rather than drifting with each
     * label's length.
     *
     * Before this, every row placed its widgets back to back and the window was narrower than
     * slider + field + label wanted, so the label was clipped and the controls sat on top of one
     * another. A column width also means the eye can run down the values, which is the whole point
     * of a settings list. */
    const float kSliderWidth = 190.0f;
    const float kFieldWidth  = 86.0f;
    const float kLabelColumn = 210.0f;

    for (int i = 0; i < count; ++i) {
        const nrex_row &r = rows[i];
        ImGui::PushID(i);
        /* A disabled row is drawn greyed and refuses input as a whole: every widget below is
         * submitted under BeginDisabled, which is what makes "cannot be changed right now" mean
         * the same thing for a slider, a combo and a checkbox. */
        if (r.disabled) ImGui::BeginDisabled();
        switch (r.kind) {
        case NREX_KIND_FLOAT: {
            /* While the row is being edited the widget keeps its own number.
             *
             * The C side's string is stale until the change has been written to the settings file
             * and read back, so re-reading it every frame rebuilt the slider from the PRE-edit value
             * - which is what made Enter open a slider and then lose it, and why the arrows never
             * moved anything. `IsItemActive` is also exactly the condition under which ImGui lets
             * the arrows move a slider. */
            static float held[NREX_MAX_ROWS];
            static int   holding[NREX_MAX_ROWS];
            if (!holding[i]) held[i] = (float)atof(r.value ? r.value : "0");
            float v = held[i];

            /* No value text on the slider: the field beside it already shows the number, and two
             * copies of it read as a rendering fault rather than as a control. */
            ImGui::SetNextItemWidth(kSliderWidth);
            if (ImGui::SliderFloat("##v", &v, r.min, r.max, "")) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = v; }
            }
            holding[i] = ImGui::IsItemActive() ? 1 : 0;

            /* Arrows step the value while this row has keyboard nav focus - no Enter needed.
             *
             * The two ways ImGui lets an arrow move a slider, and why BOTH are needed here:
             *
             *   ACTIVE   `SliderBehavior` claims Left/Right via `IsActiveIdUsingNavDir` while the
             *            item is active (`imgui_widgets.cpp`, `make_active = clicked ||
             *            g.NavActivateId == id`), so nav is suppressed and ImGui's own code steps the
             *            value. Enter activates, which is why Enter-then-arrow works.
             *
             *   FOCUSED  An item that is merely focused - what Tab gives - has claimed nothing, so
             *            `UpdateNavInput` (imgui.cpp) queues a directional move on the same keypress
             *            and nav wins. Left did nothing (the slider is the leftmost item in the row)
             *            while Right moved focus into the number field: reported exactly as "left
             *            works, right selects the number box".
             *
             * The second case is why this handler exists: a settings list where you Tab to a row and
             * then have to press Enter before the arrows do anything reads as broken.
             *
             * `IsKeyPressed(key, false)` is the EDGE only, and the hold is timed by `nrex_repeat`
             * above. `repeat = true` would add ImGui's own timer on top of this one - two
             * independent repeats on one key, which is what makes a value tick back and forth
             * instead of walking - and that is the bug this replaced. */
            nrex_repeat &rep = s_repeat[i];
            float dir = 0.0f;
            int steps = 0;
            if (ImGui::IsItemFocused()) {
                /* Both arrows belong to this row while it has focus, which is the other half of
                 * keeping a hold inside the row: ImGui's nav asks for the directional keys with
                 * `ImGuiKeyOwner_NoOwner` (`NavUpdateCreateMoveRequest`), so an owner is exactly
                 * what stops it re-targeting focus halfway through a value. Claimed every frame -
                 * ownership is dropped when the key is released - and harmless to ImGui's own
                 * slider tweak, which reads the keys through `GetKeyPressedAmount` and does not
                 * look at ownership at all. */
                const ImGuiID row_id = ImGui::GetItemID();
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow,  row_id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, row_id);

                const bool left_press  = ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
                const bool right_press = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
                const bool left_down   = ImGui::IsKeyDown(ImGuiKey_LeftArrow);
                const bool right_down  = ImGui::IsKeyDown(ImGuiKey_RightArrow);

                if (left_press || right_press) {
                    /* the click: one step, and the hold is armed from here */
                    dir = right_press ? 1.0f : -1.0f;
                    steps = 1;
                    rep.dir = right_press ? 1 : -1;
                    rep.interval = NREX_REPEAT_FIRST;
                    rep.next = nrex_now_seconds() + NREX_REPEAT_WARMUP + NREX_REPEAT_FIRST;
                } else if ((rep.dir < 0 && left_down) || (rep.dir > 0 && right_down)) {
                    /* the hold: catch up on however many repeats are due */
                    dir = (float)rep.dir;
                    const double now = nrex_now_seconds();
                    while (steps < NREX_REPEAT_BURST && now >= rep.next) {
                        steps++;
                        rep.interval *= NREX_REPEAT_ACCEL;
                        if (rep.interval < NREX_REPEAT_MIN) rep.interval = NREX_REPEAT_MIN;
                        rep.next += rep.interval;
                    }
                    /* A stall - alt-tab, a load hitch - must not leave the timer seconds behind and
                     * then fire a burst at full speed: rebase instead. */
                    if (steps >= NREX_REPEAT_BURST && now > rep.next)
                        rep.next = now + rep.interval;
                } else {
                    rep.dir = 0;
                }
            } else {
                rep.dir = 0;
            }
            if (steps > 0) {
                float step = (r.step > 0.0f) ? r.step : 0.01f;
                float moved = held[i] + dir * step * (float)steps;
                if (moved < r.min) moved = r.min;
                if (moved > r.max) moved = r.max;
                if (moved != held[i]) {
                    held[i] = moved;
                    holding[i] = 0;
                    g_actions |= NREX_ACT_CHANGED;
                    if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = moved; }
                }
            }

            /* A field beside it, because a slider has no field to type into - ImGui hides one
             * behind Ctrl+Click and nothing says so. Commits on Enter, through the same write-back
             * as the slider: one path, two ways in.
             *
             * `ImGuiItemFlags_NoNav` keeps it out of keyboard focus entirely - Tab AND the arrows.
             * The number it shows is the one the slider already shows, so stepping focus through
             * both is stepping through the same value twice; and it is what made the Right arrow
             * `ImGuiItemFlags_NoNav` keeps the field out of keyboard focus entirely - Tab AND the
             * arrows - so Enter on a row selects the SLIDER. That is the user's rule: "Enter should
             * select the progress bar, not the number box, because it duplicates the value; only the
             * slider is adjustable".
             *
             * It also removes the field as a directional target, which is the other half of why the
             * Right arrow used to land here instead of changing the value: nav moves between
             * focusable items, and this was the only one to the right of the slider.
             *
             * The flag rather than `PushTabStop(false)`: that one removes the item from Tab only,
             * and the arrows are the half that was actually broken. A mouse still reaches the field,
             * which is the only way in that needs it - `IsItemActive` above is unaffected. */
            ImGui::SameLine();
            ImGui::SetNextItemWidth(kFieldWidth);
            float typed = v;
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            bool committed = ImGui::InputFloat("##t", &typed, 0.0f, 0.0f, "%.3f",
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopItemFlag();
            if (committed) {
                if (typed < r.min) typed = r.min;
                if (typed > r.max) typed = r.max;
                held[i] = typed;
                holding[i] = 0;
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = typed; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        case NREX_KIND_CHOICE: {
            int idx = r.choice_index;
            ImGui::SetNextItemWidth(kSliderWidth + kFieldWidth +
                                    ImGui::GetStyle().ItemSpacing.x);
            bool picked = ImGui::Combo("##c", &idx, r.choices, r.choice_count);
            /* Left/Right step the choice, as the hint says they change a row. ImGui gives a combo
             * no arrow tweak - only Enter opens it - so Right on the Network row did nothing and
             * the hint was wrong there. The keys are claimed the way the slider rows above claim
             * them, so nav does not take Right as a move to another item. One step a press, no
             * repeat, and no wrap: the ends of a list stay ends. */
            if (!picked && ImGui::IsItemFocused()) {
                const ImGuiID row_id = ImGui::GetItemID();
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow,  row_id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, row_id);
                const int step = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ? 1
                               : ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false) ? -1 : 0;
                if (step != 0 && idx + step >= 0 && idx + step < r.choice_count) {
                    idx += step;
                    picked = true;
                }
            }
            if (picked) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_i = idx; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        case NREX_KIND_FLAG: {
            bool on = r.choice_index != 0;
            if (ImGui::Checkbox("##f", &on)) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_i = on ? 1 : 0; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        default:
            ImGui::TextDisabled("%s  %s", r.label, r.value ? r.value : "");
        }
        (void)kLabelColumn;
        if (r.disabled) ImGui::EndDisabled();
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Save"))   g_actions |= NREX_ACT_SAVE;
    ImGui::SameLine();
    if (ImGui::Button("Reset"))  g_actions |= NREX_ACT_RESET;
    ImGui::SameLine();
    /* Revert: back to what is SAVED, where Reset goes to the code's defaults. Two different
     * questions - "I broke something, give me the standard values" and "I was experimenting, give
     * me back what I had" - and one button cannot answer both. */
    if (ImGui::Button("Revert")) g_actions |= NREX_ACT_REVERT;
    ImGui::SameLine();
    if (ImGui::Button("Close"))  g_actions |= NREX_ACT_CLOSE;

    /* The effect, on the footer where the other whole-picture controls are.
     *
     * It reads the state the layer passes in rather than keeping one: the Home key and the setup
     * window's button flip the same trigger file, and a checkbox with its own copy would show the
     * wrong thing as soon as either was used. A click only REPORTS - the layer decides, so the
     * three ways in cannot disagree. */
    {
        bool on = g_nr_enabled > 0;
        ImGui::SameLine();
        ImGui::TextUnformatted("NR");
        ImGui::SameLine();
        if (ImGui::Checkbox("##nr", &on)) {
            g_nr_request = on ? 1 : 0;
            g_actions |= NREX_ACT_NR_TOGGLE;
        }
        /* No state to show - the trigger path did not resolve. The box is drawn unticked and
         * disabled rather than pretending the effect is off. */
        if (g_nr_enabled < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("this install has no trigger path; NR cannot be switched here");
        }
    }

    /* The language, as a control and not only a hotkey. The strings live on the C side, so this
     * only records the choice; the caller reads it back and asks the menu for the change through
     * the same flag the L key sets. */
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    static const char *lang_names[] = { "English", "中文", "Русский", "Español" };
    int chosen = g_language;
    ImGui::Combo("##lang", &chosen, lang_names, 4);
    if (chosen != g_language) {
        g_language = chosen;
        g_actions |= NREX_ACT_LANGUAGE;
    }

    /* The panel-level hint, on the footer under the controls.
     *
     * This is where the sentence "the game also sees these keys" belongs. It lived only in
     * `NR_STR_HINT`, and only the hand-drawn menu was handed it - so the ImGui panel, which is
     * what every game actually loads, showed no key list and no warning at all. A player reading
     * the panel had no way to learn that walking its rows also walks the game's menus, which is
     * what the MK11 title-screen report was.
     *
     * `TextWrapped`, because the Chinese and Russian lines are long and a cut sentence is worse
     * than a wrapped one: the footer is the only place the panel has for prose. */
    if (g_panel_hint[0]) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", g_panel_hint);
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

extern "C" NREX_API void nrex_begin_panel(nrex_ui *ui, const char *title, int language,
                                          const char *hint)
{    g_ui = ui;
    g_actions = 0;
    /* Cleared here, not in `nrex_rows`: the checkbox is drawn in the footer, after the rows, and a
     * request from the PREVIOUS frame must not be read twice. */
    g_nr_request = -1;
    (void)title;
    if (language >= 0 && language < 4) g_language = language;
    /* The footer line, kept for the draw below. Copied rather than remembered: the caller's string
     * is the generated table's, which lives for the process, but a pointer that outlives its owner
     * is the kind of thing that works until it does not. */
    snprintf(g_panel_hint, sizeof g_panel_hint, "%s", hint ? hint : "");
}

extern "C" NREX_API void nrex_set_nr_enabled(nrex_ui *ui, int on)
{
    (void)ui;
    g_nr_enabled = on < 0 ? -1 : (on ? 1 : 0);
}

extern "C" NREX_API int nrex_nr_toggle_request(nrex_ui *ui)
{
    (void)ui;
    return g_nr_request;
}

extern "C" NREX_API int nrex_panel_language(nrex_ui *ui)
{
    (void)ui;
    return g_language;
}

extern "C" NREX_API void nrex_rows(nrex_ui *ui, const nrex_row *rows, int count, int selected)
{
    /* `const` in the signature, written to in practice: the array belongs to the caller and the
     * edits have to reach it. The cast is confined to this line, which is the honest place for it -
     * a copy would leave the caller holding the pre-edit values. */
    g_rows = (nrex_row *)(uintptr_t)rows;
    nrex_draw_panel(ui, "DLSS-NR on Intel", 0, rows, count, selected);
}

extern "C" NREX_API int nrex_end_panel(nrex_ui *ui)
{
    (void)ui;
    return g_actions;
}

/* The hint the footer was last handed, for a test that wants to know it was drawn.
 *
 * The draw itself is the only real evidence, and this is deliberately NOT that: it answers "was the
 * string here", not "did the compositor emit vertices for it". The fixture pairs it with the
 * panel's actions being unchanged, which is the half that could regress silently - a hint that
 * swallowed a click, say. Returning 0 for an empty hint means "nothing to draw", which is what an
 * empty string means to the drawer too. */
extern "C" NREX_API int nrex_last_footer_text(char *out, size_t size)
{
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!g_panel_hint[0]) return 0;
    int n = snprintf(out, size, "%s", g_panel_hint);
    return n > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ verification dump */

/* A throwaway command buffer for the dump.
 *
 * The dump runs after `nrex_render` has already closed this frame's command buffer, and the C side
 * submits that one immediately afterwards, so the dump cannot reuse it. `vkCmdCopyImageToBuffer`
 * also needs the image in a transfer layout, which is a barrier the panel's own pass does not
 * contain. A pool of its own, created and destroyed around the single copy, keeps the dump from
 * touching anything the running frame owns. */
static bool nrex_oneshot_begin(nrex_ui *u, VkCommandPool *pool, VkCommandBuffer *cmd)
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

static bool nrex_oneshot_submit(nrex_ui *u, VkCommandBuffer cmd, VkFence fence)
{
    if (u->vk.end_cmd(cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (u->vk.submit(u->vk.queue, 1, &si, fence) != VK_SUCCESS) return false;
    return u->vk.wait_fences(u->device, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
}

static void nrex_oneshot_end(nrex_ui *u, VkCommandPool pool)
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
static void nrex_dump_ui(nrex_ui *u)
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

extern "C" NREX_API void nrex_render(nrex_ui *ui, uint64_t cmd_handle)
{
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

/* The C side polls GetAsyncKeyState - a layer has no window and no message pump - and hands the
 * state here. This is the platform-backend role, and ImGui's contract for it is specific:
 *
 *   AddKeyEvent describes a CHANGE, not a level. Reporting a held key as down on every frame is
 *   reporting a press on every frame, which is what made Enter reopen and re-confirm the combo it
 *   had just opened. Only transitions are forwarded, so a held key is one press followed by
 *   ImGui's own repeat.
 *
 *   AddInputCharacter is what puts a glyph into a text field. Keys move focus and drive widgets;
 *   they do not type. Without characters, activating a slider showed a selected field that no
 *   number would go into.
 *
 * The virtual-key to ImGuiKey mapping covers what the menu binds. An unmapped key is ignored rather
 * than guessed at. Keys whose character cannot be derived from the virtual key alone (anything
 * needing a layout lookup) still move focus; they simply do not type. */
static unsigned char s_key_down[256];

extern "C" NREX_API void nrex_key(nrex_ui *ui, int vk, int down)
{
    (void)ui;
    if (vk < 0 || vk > 255) return;
    int now = down != 0;
    if (s_key_down[vk] == (unsigned char)now) return;   /* no change: not an event */
    s_key_down[vk] = (unsigned char)now;

    ImGuiIO &io = ImGui::GetIO();
    ImGuiKey key = ImGuiKey_None;
    switch (vk) {
    case 0x26: key = ImGuiKey_UpArrow;    break;
    case 0x28: key = ImGuiKey_DownArrow;  break;
    case 0x25: key = ImGuiKey_LeftArrow;  break;
    case 0x27: key = ImGuiKey_RightArrow; break;
    case 0x0D: key = ImGuiKey_Enter;      break;
    case 0x1B: key = ImGuiKey_Escape;     break;
    case 0x2D: key = ImGuiKey_Insert;     break;
    case 0x23: key = ImGuiKey_End;        break;
    case 0x24: key = ImGuiKey_Home;       break;
    case 0x10: key = ImGuiKey_LeftShift;  break;
    case 0x20: key = ImGuiKey_Space;      break;
    case 0x08: key = ImGuiKey_Backspace;  break;
    case 0x2E: key = ImGuiKey_Delete;     break;
    case 0x09: key = ImGuiKey_Tab;        break;
    case 0xBE: key = ImGuiKey_Period;     break;
    case 0xBD: key = ImGuiKey_Minus;      break;
    default:
        if (vk >= 'A' && vk <= 'Z') key = (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
        else if (vk >= '0' && vk <= '9') key = (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
        break;
    }
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, now);

    /* The character, on the press only, so a key with no character (an arrow, a function key)
     * types nothing and Control is excluded so Ctrl+S is a shortcut rather than a typed 's'.
     *
     * **Windows** translates through the active keyboard layout with `ToUnicode`, so a layout that
     * puts a digit behind Shift still types that digit - which guessing from the virtual key would
     * not. The key state is BUILT rather than asked for: `GetKeyboardState` reports the calling
     * thread's message queue, and it is only accurate for a thread that pumps messages - which a
     * Vulkan layer does not, it runs inside the game's render thread. The first attempt used it and
     * the typed characters never arrived. `GetAsyncKeyState` is system-wide and needs no pump, so
     * the state is assembled from it, and it also makes a layout's own modifiers come through.
     *
     * **Off Windows** there is no equivalent to ask, so the character is inferred from the virtual
     * key alone - correct for a US layout, wrong for one that needs a modifier. That is the honest
     * limit of a panel that has no window to own an input method; the alternative (no typing at all
     * where a text field exists) is worse. The row that needs text is a number, and digits are
     * digits on every layout. */
#ifdef _WIN32
    if (now && !(GetAsyncKeyState(0x11) & 0x8000)) {
        BYTE state[256];
        memset(state, 0, sizeof state);
        for (int k = 0; k < 256; ++k) {
            SHORT s = GetAsyncKeyState(k);
            if (s & 0x8000) state[k] |= 0x80;      /* the key is down now */
            if (s & 0x0001) state[k] |= 0x01;      /* it was pressed since the last poll */
        }
        state[0x10] = (BYTE)((GetAsyncKeyState(0x10) & 0x8000) ? 0x80 : 0);
        state[0x11] = (BYTE)((GetAsyncKeyState(0x11) & 0x8000) ? 0x80 : 0);
        state[0x12] = (BYTE)((GetAsyncKeyState(0x12) & 0x8000) ? 0x80 : 0);
        state[0x14] = (BYTE)((GetAsyncKeyState(0x14) & 0x8000) ? 0x01 : 0);  /* Caps Lock is a toggle */
        WCHAR buf[8] = {0};
        int n = ToUnicode((UINT)vk, 0, state, buf, 4, 0);
        for (int i = 0; i < n; ++i)
            if (buf[i] >= 0x20)
                io.AddInputCharacter((unsigned int)buf[i]);
    }
#else
    (void)vk;
    if (now && vk >= 0x20 && vk < 0x7F)
        io.AddInputCharacter((unsigned int)vk);
#endif
}

/* ImGui's own pointer, drawn into the frame.
 *
 * `MouseDrawCursor` makes it rasterise an arrow from a texture it already builds into the font
 * atlas, and clearing `HasMouseCursors` stops it asking the OS to change the cursor shape - so the
 * two never fight over who is drawing the pointer. Set per frame by the caller.
 *
 * **It draws nothing without a position**, which is what "the fallback cursor does not appear"
 * turned out to be. ImGui gates its cursor on `io.MousePos` being valid, and `nrex_mouse` reports
 * "no position" (`-FLT_MAX`) whenever the pointer is outside the panel's own image - so the fallback
 * worked only when the pointer was already over the panel, which is precisely when nobody needs it.
 * Measured with `test_soft_cursor.cpp`: 62 vertices with a valid position and 46 without, the
 * difference being the arrow's quad.
 *
 * `nrex_cursor_hint` carries the position separately, so the arrow can be drawn anywhere on screen
 * while hit-testing still sees "no position" and refuses to hover whatever is at the edge. */
static float g_cursor_hint_x = -FLT_MAX;
static float g_cursor_hint_y = -FLT_MAX;

extern "C" NREX_API void nrex_cursor_hint(nrex_ui *ui, float x, float y)
{
    (void)ui;
    g_cursor_hint_x = x;
    g_cursor_hint_y = y;
}

extern "C" NREX_API void nrex_set_software_cursor(nrex_ui *ui, int on)
{
    (void)ui;
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = on != 0;
    if (on)
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
    else
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
}

/* Put the software cursor where the pointer is, for the frame about to be built.
 *
 * Called from `nrex_new_frame` AFTER `ImGui::NewFrame`, because `NewFrame` is what processes the
 * queued position events - writing `io.MousePos` before it would be overwritten. Only when ImGui
 * has no position of its own: a real position is still the truth for what is hovered, and moving
 * the arrow to the hint on top of it would make the drawn pointer disagree with the hovered widget.
 *
 * The hint is in the same space the layer computed it in (UI-image pixels), so it is clamped to the
 * image rather than transformed: an arrow at the very edge of the swapchain is still useful, and an
 * arrow outside it would be clipped away with no explanation. */
static void nrex_place_software_cursor(nrex_ui *ui)
{
    if (!ui) return;
    ImGuiIO &io = ImGui::GetIO();
    if (!io.MouseDrawCursor) return;
    if (io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX) return;   /* already valid */
    if (g_cursor_hint_x == -FLT_MAX || g_cursor_hint_y == -FLT_MAX) return;
    float x = g_cursor_hint_x, y = g_cursor_hint_y;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > io.DisplaySize.x) x = io.DisplaySize.x;
    if (y > io.DisplaySize.y) y = io.DisplaySize.y;
    io.MousePos = ImVec2(x, y);
}

extern "C" NREX_API void nrex_mouse(nrex_ui *ui, float x, float y, int down)
{
    (void)ui;
    ImGuiIO &io = ImGui::GetIO();
    /* A position outside the panel's own image is reported as ImGui's "no position" sentinel rather
     * than as a coordinate. Clamping it would hover whatever sits at the edge, and a click there
     * would land on a widget the pointer was never over. */
    if (x < 0.0f || y < 0.0f || x >= io.DisplaySize.x || y >= io.DisplaySize.y) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        return;
    }
    /* Trace the pointer in ImGui's coordinates when it moves or clicks. The next report then says
     * whether coordinates arrive at all and where they land, instead of a guess.
     *
     * Behind NREX_TRACE, which is off in a release. It was on, unthrottled, on every pointer move -
     * in somebody else's process, writing to the game's stderr while they played. It earned its keep
     * while the pointer conversion was being worked out and it costs nothing to keep it there. */
    static float last_x = -1.0f, last_y = -1.0f;
    static int last_down = 0;
    if (x != last_x || y != last_y || down != last_down) {
        last_x = x; last_y = y; last_down = down;
        NREX_TRACE("[mouse] x=%.0f y=%.0f down=%d display=%.0fx%.0f\n",
                   x, y, down, io.DisplaySize.x, io.DisplaySize.y);
    }
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down != 0);
}
/* `uint64_t`, not `void *`: on x86 a Vulkan non-dispatchable handle is 64 bits and a pointer is 32,
 * so a `void *` return dropped the top half of `VkImage`/`VkImageView`. See the header. */
extern "C" NREX_API uint64_t nrex_image(nrex_ui *ui) { return ui ? (uint64_t)ui->image : 0; }
/* The view, for the blend pass: the shader samples the UI through an image view, and the caller
 * has no other way to reach one it did not create. */
extern "C" NREX_API uint64_t nrex_image_view(nrex_ui *ui) { return ui ? (uint64_t)ui->image_view : 0; }
extern "C" NREX_API void *nrex_image_layout_transition(nrex_ui *ui) { (void)ui; return nullptr; }
extern "C" NREX_API uint32_t nrex_image_width(nrex_ui *ui) { return ui ? ui->width : 0; }
extern "C" NREX_API uint32_t nrex_image_height(nrex_ui *ui) { return ui ? ui->height : 0; }
extern "C" NREX_API int nrex_resize(nrex_ui *ui, uint32_t w, uint32_t h)
{
    if (!ui) return 0;
    if (ui->width == w && ui->height == h) return 1;
    nrex_drop_target(ui);
    const char *why;
    return nrex_make_target(ui, w, h, &why) ? 1 : 0;
}
extern "C" NREX_API const char *nrex_language_name(int language)
{
    static const char *names[] = {"English", "中文", "Русский", "Español"};
    return (language >= 0 && language < 4) ? names[language] : names[0];
}
