/* The in-game settings menu. See nr_menu.h for where it draws and what it must not disturb.
 *
 * This file talks to Vulkan and to the keyboard. It does not know what a knob is: the caller hands
 * it rows of text and it draws them, and reports which row moved and in which direction. The knob
 * list lives in `nr_knobs.py` and reaches here as text, which is why there is no second copy of it.
 *
 * Three rules this file is built around, each of them a way the layer breaks a game if it is
 * broken:
 *
 *   1. **No `vk*` by name.** Every entry point is resolved through the two resolvers the caller
 *      passes, the same ones `nr_layer.c` uses. The layer deliberately links no vulkan-1.lib, so a
 *      second loader in the game's process would be a bug.
 *
 *   2. **No submit.** `nr_menu_draw` records into the caller's command buffer. The submit path is
 *      where the semaphore contract lives (`waits_consumed`, `present_wait_count`), and a menu
 *      that submitted would have to own a wait semaphore it did not create.
 *
 *   3. **No assumption about the swapchain.** The caller adds TRANSFER usage before creating the
 *      swapchain; a storage image needs STORAGE as well, and this checks that the format actually
 *      supports being one before building anything. If it does not, the menu does not exist.
 */
#include "nr_menu.h"
#include "nr_menu_strings.h"
#include "nr_menu_font.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

/* ------------------------------------------------------------------ layout constants */

#define NR_PANEL_W      460
#define NR_PANEL_H      400
#define NR_ROW_H         22
#define NR_TEXT_PAD       8
#define NR_TITLE_SCALE    2

/* A generous menu: a title, twenty rows, a hint, the note, the panel, and the compare overlay's
 * quads. A menu that needs more drops the extras rather than growing the buffer, because growing
 * means a fence and a stall inside somebody's present path. */
#define NR_QUAD_CAPACITY 3072

/* One rectangle, byte-for-byte what `menu.comp` reads.
 *
 * The shader declares
 *
 *     vec4 rect; vec4 uv; vec4 colour; uint flags; uint _pad0..2;
 *
 * which is 64 bytes, read through a `scalar` buffer_reference - so the layout is exactly as written
 * with no padding inserted anywhere.
 *
 * The first version of this struct used `uint32_t colour` where the shader has a `vec4`, making it
 * 48 bytes. Two things followed, and the second is why the panel never appeared:
 *
 *   * the stride was wrong, so every quad after the first was read from the wrong offset;
 *   * the colour the shader got was four unrelated fields reinterpreted as floats. For the panel's
 *     own background constant that produced an alpha of 0.0, and the shader ends with
 *     `if (out_colour.a <= 0.0) return;` - so EVERY quad was skipped, every frame, with no error.
 *
 * The `_Static_assert` is the check that would have caught it before a game was involved. */
typedef struct {
    float x, y, w, h;
    float u0, v0, u1, v1;
    float cr, cg, cb, ca;      /* the colour as the shader's vec4 - NOT a packed uint32 */
    uint32_t flags;
    uint32_t _pad0, _pad1, _pad2;
} nr_menu_quad;

/* `static_assert` rather than `_Static_assert`: the latter is the C11 spelling and this file is
 * built `/TC` without a C standard flag, where MSVC rejects it. With <assert.h> included the
 * lowercase form is the same check under the name MSVC accepts. */
static_assert(sizeof(nr_menu_quad) == 64,
              "nr_menu_quad must be 64 bytes to match menu.comp's Quad");

/* A colour, in the order the eye reads it: r, g, b, a.
 *
 * A named type rather than a packed integer, because the packed form is what broke this. The
 * shader's `colour` is a `vec4`; a `uint32_t` arriving where a vec4 is expected becomes four
 * unrelated fields, and for the panel's own background that meant an alpha of 0.0 - so the shader's
 * `if (out_colour.a <= 0.0) return;` skipped every quad and nothing was ever drawn.
 *
 * Four loose `uint8_t` parameters would work too and are easier to get in the wrong order. */
typedef struct { uint8_t r, g, b, a; } nr_colour;

#define COLOUR(R, G, B, A) ((nr_colour){ (uint8_t)(R), (uint8_t)(G), (uint8_t)(B), (uint8_t)(A) })

#define NR_QUAD_SOLID 1u
#define NR_QUAD_REFERENCE 2u

/* A swapchain has three to five images and they recur. Their views are kept between frames. */
#define NR_VIEW_CACHE 8

struct nr_view_entry {
    VkImage image;
    VkImageView view;
};

/* `menu.comp`'s push constant block, laid out to match - and the block is std430.
 *
 * std430 aligns a vec2 to 8 bytes, so the block is
 *
 *     count @0, (unused word @4), panel_origin @8, panel_size @16,
 *     atlas_size @24, compare_split @32, compare_swap @36, compare_mode @40, _pad @44
 *
 * The word after `count` is padding the alignment demands, not a field; writing this struct without
 * it was what made the std430 build draw nothing, because the shader then read its panel origin from
 * the wrong offset and the dispatch covered a rectangle nobody asked for. The static assert pins the
 * size the block implies, so a future edit to either side fails to compile instead of drawing
 * nothing. */
typedef struct {
    uint32_t count;
    uint32_t _align0;          /* std430 puts the first vec2 on an 8-byte boundary */
    float    panel_x, panel_y;
    float    panel_w, panel_h;
    uint32_t atlas_w, atlas_h;
    /* The comparison: where the split is, which side the reference is on, and which mode. */
    float    compare_split;
    uint32_t compare_swap;
    uint32_t compare_mode;
    uint32_t _pad;
} nr_menu_push;

static_assert(sizeof(nr_menu_push) == 48,
              "nr_menu_push must be 48 bytes to match menu.comp's std430 Push block");

struct nr_menu {
    VkDevice device;
    VkPhysicalDevice physical;
    uint32_t queue_family;
    VkFormat format;
    /* 1 when the shader build declares 
gb10a2, which changes how a colour constant is packed -
     * see colour_pack. Set from the format at create and constant after. */
    int rgb10a2;
    nr_menu_resolve_fn resolve_device;
    nr_menu_resolve_fn resolve_instance;

    /* Every Vulkan entry point the menu uses, resolved once at create. A NULL here means the
     * driver has no such function, which create treats as "cannot draw" and reports. */
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkBindBufferMemory bind_buffer_memory;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
    PFN_vkMapMemory map_memory;
    PFN_vkUnmapMemory unmap_memory;
    PFN_vkCreateImageView create_image_view;
    PFN_vkDestroyImageView destroy_image_view;
    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout;
    PFN_vkCreatePipelineLayout create_pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
    PFN_vkCreateDescriptorPool create_descriptor_pool;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets;
    PFN_vkFreeDescriptorSets free_descriptor_sets;
    PFN_vkUpdateDescriptorSets update_descriptor_sets;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor;
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdDispatch cmd_dispatch;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;

    VkShaderModule shader;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    /* Two sets: the target and the reference. Both allocated at create, so nothing is allocated
     * inside a frame - an allocation on the present path is a stall. */
    VkDescriptorSet set;
    VkDescriptorSet compare_set;

    /* The font, and the atlas as the shader sees it: a storage buffer indexed by address. An R8
     * image would need a sampled-image descriptor and a layout transition on every use; a buffer
     * needs neither, and the atlas is a byte a texel either way. */
    nr_font font;
    VkBuffer atlas_buffer;
    VkDeviceMemory atlas_memory;
    void *atlas_mapped;

    VkBuffer quad_buffer;
    VkDeviceMemory quad_memory;
    void *quad_mapped;
    uint32_t quad_capacity;

    /* Where the game's own frame goes for the comparison: a device-local image the caller's
     * staging buffer is copied into. Created lazily at the swapchain's size and recreated on
     * resize. Not host-visible: the copy is GPU to GPU, and the point is that nothing comes back. */
    VkImage reference;
    VkDeviceMemory reference_memory;
    VkImageView reference_view;
    uint32_t reference_w, reference_h;

    const nr_menu_row *rows;
    int row_count;
    const char *title;
    const char *hint;
    const char *note;
    float note_until;

    int selected;
    int language;
    int open;
    int step;
    int save_pressed;
    int reset_pressed;
    int language_changed;
    int toggle_pressed;
    int compare_changed;
    int compare_mode;
    int compare_swap;
    float compare_split;
    float split_step;
    int nr_enabled;

    uint8_t key_down[256];
    /* 1 while this file reads the panel's keys itself; 0 when ImGui owns the panel and this file
     * must keep its hands off them. Set once at creation by the layer. */
    int read_keys;
    float elapsed;

    /* Views of the swapchain images, kept between frames. See image_view_for(). */
    struct nr_view_entry views[NR_VIEW_CACHE];
};

/* The keys the menu reads.
 *
 * INS opens it, which is what the pre-SR project uses and what a player already expects. END
 * toggles NR and HOME cycles the comparison, and both of those act with the menu closed - that is
 * the point of them being keys. Everything else is read only while the menu is open, so a game
 * keeps its own input. */
#define NR_KEY_INS     0x2D
#define NR_KEY_UP      0x26
#define NR_KEY_DOWN    0x28
#define NR_KEY_LEFT    0x25
#define NR_KEY_RIGHT   0x27
#define NR_KEY_ENTER   0x0D
#define NR_KEY_SHIFT   0x10
#define NR_KEY_L       0x4C
#define NR_KEY_S       0x53
#define NR_KEY_R       0x52
#define NR_KEY_C       0x43
#define NR_KEY_ESC     0x1B
#define NR_KEY_TOGGLE  0x23   /* END  - neural rendering on or off */
#define NR_KEY_COMPARE 0x24   /* HOME - the picture comparison */

static int key_pressed(nr_menu *m, int vk)
{
#ifdef _WIN32
    /* The high bit means "down now". The low bit means "pressed since the last call" and is
     * documented as unreliable across input stacks, so the edge is derived here instead. */
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int was = m->key_down[vk & 0xFF];
    m->key_down[vk & 0xFF] = (uint8_t)down;
    return down && !was;
#else
    (void)m; (void)vk;
    return 0;
#endif
}

static int key_down(nr_menu *m, int vk)
{
#ifdef _WIN32
    (void)m;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)m; (void)vk;
    return 0;
#endif
}

/* ------------------------------------------------------------------ colour and memory */

/* A colour as the target image wants it. Two packings, because the swapchain has two kinds of
 * format and the shader cannot convert between them:
 *
 *   rgba8     one byte a channel. The shader indexes the constant as B, G, R, A, which is the
 *             order a B8G8R8A8 surface wants - and for an R8G8B8A8 surface the channels land
 *             swapped, which is why the format list has both and why this takes the format.
 *   rgb10a2   10 bits a channel with the 2-bit alpha in the HIGH bits, which is what
 *             VK_FORMAT_A2B10G10R10_UNORM_PACK32 means. Getting this wrong is not a crash: it is
 *             a menu in the wrong colours, which is worse to diagnose.
 *
 * 
gb10a2 is m->rgb10a2, so the caller does not have to remember which build is loaded. */
/* The colour conversion this file used to do now belongs to the shader, which writes into an
 * image of the format the pipeline was built for - see nr_colour above. */

static uint32_t find_memory_type(nr_menu *m, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    m->get_memory_properties(m->physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Host-visible, mapped for the process's lifetime. The menu rewrites its quads every frame, and a
 * staging copy plus a fence would cost more than the write does. */
static int make_host_buffer(nr_menu *m, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer *buffer, VkDeviceMemory *memory, void **mapped,
                            const char **why)
{
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m->create_buffer(m->device, &bci, NULL, buffer) != VK_SUCCESS) {
        *why = "no buffer for the menu";
        return 0;
    }
    VkMemoryRequirements req;
    m->get_buffer_memory_requirements(m->device, *buffer, &req);
    uint32_t type = find_memory_type(m, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        *why = "no host-visible memory for the menu";
        return 0;
    }
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    /* The two host buffers are created with SHADER_DEVICE_ADDRESS_BIT because the shader takes
     * their address in a push constant, and the spec then requires the memory to be allocated with
     * the matching flag: VUID-vkBindBufferMemory-bufferDeviceAddress-03339, which named both of
     * them. Bound on the buffer's own usage, so a buffer without the address bit is unaffected. */
    VkMemoryAllocateFlagsInfo address_flags = {0};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        address_flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        mai.pNext = &address_flags;
    }
    if (m->allocate_memory(m->device, &mai, NULL, memory) != VK_SUCCESS) {
        *why = "no memory for the menu";
        return 0;
    }
    if (m->bind_buffer_memory(m->device, *buffer, *memory, 0) != VK_SUCCESS) {
        *why = "cannot bind the menu's buffer";
        return 0;
    }
    if (m->map_memory(m->device, *memory, 0, size, 0, mapped) != VK_SUCCESS) {
        *why = "cannot map the menu's buffer";
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ the format check */

/* The formats the menu can draw on, and which build of the shader each needs.
 *
 * A storage image's declared format must match the image's, so this is not a preference - it is
 * which `.spv` to load. `A2B10G10R10` is in the list because a real game presents it: Aperture
 * Desk Job's swapchain is `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (an HDR surface), and the first
 * game test refused the menu with "this surface's format is not BGRA8". The layer already tracked
 * that format for its own copies; the menu now matches it.
 *
 * `shader` names the file the build produces, and `rgb10a2` says how to pack a colour constant -
 * 10 bits a channel with the alpha in the high bits, rather than one byte a channel with the alpha
 * last. Getting that wrong is a menu in the wrong colours, not a crash, which is why it is decided
 * here once rather than at every call site. */
struct nr_menu_format {
    VkFormat format;
    const char *shader;
    int rgb10a2;
};

static const struct nr_menu_format menu_formats[] = {
    { VK_FORMAT_B8G8R8A8_UNORM,           "menu.spv",         0 },
    { VK_FORMAT_B8G8R8A8_SRGB,            "menu.spv",         0 },
    { VK_FORMAT_R8G8B8A8_UNORM,           "menu.spv",         0 },
    { VK_FORMAT_R8G8B8A8_SRGB,            "menu.spv",         0 },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "menu_rgb10a2.spv", 1 },
};

static const struct nr_menu_format *menu_format_for(VkFormat format)
{
    for (size_t i = 0; i < sizeof menu_formats / sizeof menu_formats[0]; ++i)
        if (menu_formats[i].format == format)
            return &menu_formats[i];
    return NULL;
}

/* Whether this format can be a storage image that is written with stores.
 *
 * The swapchain path requests only TRANSFER usage, which says nothing about whether the format can
 * be a storage image at all. A format that lacks the feature must leave the game untouched rather
 * than fail on the first dispatch, which on a validation layer is an error and on a player is a
 * crash.
 *
 * `VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT` is deliberately not asked for: the menu writes with
 * plain stores and no atomics, and requiring it would refuse surfaces the menu can draw on. */
static int format_supports_storage(nr_menu *m, VkFormat format, const char **why)
{
    if (!m->get_format_properties) {
        *why = "the driver has no vkGetPhysicalDeviceFormatProperties";
        return 0;
    }
    VkFormatProperties props;
    m->get_format_properties(m->physical, format, &props);
    if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
        *why = "this surface's format cannot be a storage image";
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ the atlas */

/* Every string the menu can print, flattened, so the atlas is built from them.
 *
 * `nr_font_build` bakes ASCII plus whatever non-ASCII characters it is given, and this is where
 * they come from - the generated tables themselves, not a hand-written list beside them. That is
 * what makes "the atlas covers what the menu prints" true by construction: a language added to
 * `nr_menu_strings.h` brings its glyphs, and one removed takes them away. A hand-written list is
 * how an earlier version ended up baking ASCII and Chinese and nothing else, so Russian and
 * Spanish drew as blanks.
 *
 * The bound is `NR_MENU_STRINGS_PER_LANGUAGE` and NOT the two tables the first draft counted:
 * each language contributes keys, compare names AND hints, and leaving the hints out of the
 * arithmetic under-sized the array by 87 pointers while the loop still wrote all of them. That
 * overflow is what crashed the game on the menu's first real run - an access violation a few
 * frames after the swapchain was created, with `NR_MENU=0` making it go away. The count is
 * asserted below so a language or a table added later fails loudly instead of overflowing again.
 *
 * Built once and cached: the tables do not change at run time. */
#define NR_MENU_STRINGS_PER_LANGUAGE \
    (NR_MENU_KEY_COUNT + NR_MENU_COMPARE_COUNT + NR_MENU_KNOB_COUNT)

static const char **menu_atlas_strings(void)
{
    static const char *list[NR_MENU_LANG_COUNT * NR_MENU_STRINGS_PER_LANGUAGE + 1];
    static int built = 0;
    if (!built) {
        int n = 0;
        for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
            for (int key = 0; key < NR_MENU_KEY_COUNT; ++key)
                list[n++] = nr_menu_strings[lang][key];
            for (int mode = 0; mode < NR_MENU_COMPARE_COUNT; ++mode)
                list[n++] = nr_menu_compare_names[lang][mode];
            /* KNOB_COUNT, not KEY_COUNT: hints are per knob. Using the key count read past the
             * end of every row and walked into NULLs, which is what stopped the glyph collection
             * at the first one - the menu lost its Chinese and Russian, and the read is what
             * faulted on the first real game. */
            for (int knob = 0; knob < NR_MENU_KNOB_COUNT; ++knob)
                list[n++] = nr_menu_hints[lang][knob];
        }
        /* The loop above must fill exactly the array it was sized for. If it does not, the size
         * is wrong - which is a bug that silently corrupts memory rather than failing, so it is
         * checked here while the cost is one comparison. */
        assert(n == NR_MENU_LANG_COUNT * NR_MENU_STRINGS_PER_LANGUAGE);
        list[n] = NULL;
        built = 1;
    }
    return list;
}

static int build_atlas(nr_menu *m, const char **why)
{
    if (!nr_font_build(&m->font, NR_FONT_CELL_H, menu_atlas_strings())) {
        *why = "no usable font for the menu";
        return 0;
    }
    /* Padded to a multiple of four: the shader reads the atlas as `uint` and picks the byte by
     * shift, so a trailing partial word would be read past the end. Four bytes of slack is
     * cheaper than a branch in the shader. */
    VkDeviceSize bytes = (VkDeviceSize)m->font.width * m->font.height;
    VkDeviceSize padded = (bytes + 3u) & ~(VkDeviceSize)3u;

    if (!make_host_buffer(m, padded, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          &m->atlas_buffer, &m->atlas_memory, &m->atlas_mapped, why))
        return 0;
    memcpy(m->atlas_mapped, m->font.pixels, (size_t)bytes);

    return 1;
}

static int build_quads(nr_menu *m, const char **why)
{
    VkDeviceSize bytes = sizeof(nr_menu_quad) * NR_QUAD_CAPACITY;
    if (!make_host_buffer(m, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          &m->quad_buffer, &m->quad_memory, &m->quad_mapped, why))
        return 0;
    m->quad_capacity = NR_QUAD_CAPACITY;

    return 1;
}

/* ------------------------------------------------------------------ the pipeline */

/* Where this layer's own directory is, so its shaders can be found beside it. Same resolution the
 * daemon spawn uses: the loader knows our path, `/proc/self/exe` would be the game. */
static void own_directory(char *out, size_t size)
{
    out[0] = '\0';
#ifdef _WIN32
    HMODULE hmod;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)(const void *)&own_directory, &hmod)) {
        WCHAR w[1024];
        if (GetModuleFileNameW(hmod, w, 1024))
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL);
    }
#else
    (void)size;
#endif
    char *slash = out;
    for (char *p = out; *p; ++p) if (*p == '/' || *p == '\\') slash = p;
    *slash = '\0';
}

/* The menu's SPIR-V, read from disk the way everything else in this tree reads its shaders: the
 * layer already has `work/` beside it for the graph's own modules, and `menu.spv` sits there with
 * them. Embedding it would mean a generated header and a build step that turns a .spv into an
 * array; one file open at start-up keeps the build the shape it already has. */
/* Find a shader beside the layer.
 *
 * Three layouts, and the order matters:
 *
 *   <dir>/work/<name>    where an INSTALLED layer keeps them. Setup puts the layer at
 *                        `<game>\dlss-nr\nr_layer.dll` and its runtime - shaders included - in
 *                        `<game>\dlss-nr\work\`. Looking only next to the DLL is what made the
 *                        first in-game run of this build report "the menu's shader was not found
 *                        beside the layer" while the file sat one directory down.
 *   <dir>/<name>         the checkout layout, where the build writes the layer into `work/`
 *                        beside the shaders and the two are the same directory.
 *   <dir>/../work/<name> a layer built into a subdirectory of the runtime.
 *
 * `/` rather than `\`: Windows accepts either for file access, so one spelling serves both
 * platforms and only the separator-free part differs.
 */
static int load_spv(const char *dir, const char *name, void **data, size_t *size)
{
    char path[1024];
    FILE *f = NULL;

    snprintf(path, sizeof path, "%s/work/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof path, "%s/%s", dir, name);
        f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof path, "%s/../work/%s", dir, name);
            f = fopen(path, "rb");
            if (!f) return 0;
        }
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long length = ftell(f);
    if (length <= 0) { fclose(f); return 0; }
    rewind(f);
    void *buffer = malloc((size_t)length);
    if (!buffer) { fclose(f); return 0; }
    if (fread(buffer, 1, (size_t)length, f) != (size_t)length) {
        free(buffer);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data = buffer;
    *size = (size_t)length;
    return 1;
}

static int build_pipeline(nr_menu *m, const char **why)
{
    char dir[1024];
    own_directory(dir, sizeof dir);

    void *spv = NULL;
    size_t spv_size = 0;
    /* The shader for THIS format. `menu.spv` declares `rgba8`; `menu_rgb10a2.spv` declares
     * `rgb10a2`. A storage image's format has to match the image's, so this is not a choice. */
    const char *shader = m->rgb10a2 ? "menu_rgb10a2.spv" : "menu.spv";
    if (!load_spv(dir, shader, &spv, &spv_size)) {
        *why = "the menu's shader was not found beside the layer";
        return 0;
    }

    VkShaderModuleCreateInfo sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = spv_size;
    sci.pCode = (const uint32_t *)spv;
    VkResult made = m->create_shader_module(m->device, &sci, NULL, &m->shader);
    free(spv);
    if (made != VK_SUCCESS) {
        *why = "menu.spv will not load as a shader";
        return 0;
    }

    /* Four bindings, matching the shader: the image the menu writes, the reference the comparison
     * reads, the quads and the font atlas.
     *
     * The two buffers are storage buffers rather than addresses in push constants, which is what
     * keeps this off `vkGetBufferDeviceAddress` - see the note in menu.comp. All four live in the
     * layer's own descriptor set, which the application never sees and so cannot be disturbed. */
    VkDescriptorSetLayoutBinding bindings[4] = {{0}, {0}, {0}, {0}};
    for (int i = 0; i < 4; ++i) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo slci = {0};
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    if (m->create_descriptor_set_layout(m->device, &slci, NULL, &m->set_layout) != VK_SUCCESS) {
        *why = "no descriptor layout for the menu";
        return 0;
    }

    VkPushConstantRange range = {0};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(nr_menu_push);

    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m->set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (m->create_pipeline_layout(m->device, &plci, NULL, &m->layout) != VK_SUCCESS) {
        *why = "no pipeline layout for the menu";
        return 0;
    }

    /* The pool has to be able to satisfy the layout, and the layout has two kinds of descriptor.
     * Declaring only storage images is what made the first run of this build fail with "no
     * descriptor for the menu": `vkAllocateDescriptorSets` refuses a set whose layout asks for a
     * type the pool did not declare, and it refuses by returning an error rather than by saying
     * which type was missing.
     *
     * Counts are per-set times the number of sets: 2 sets, each with 2 images and 2 buffers. */
    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
    };
    VkDescriptorPoolCreateInfo dpci = {0};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = pool_sizes;
    if (m->create_descriptor_pool(m->device, &dpci, NULL, &m->pool) != VK_SUCCESS) {
        *why = "no descriptor pool for the menu";
        return 0;
    }
    /* `pSetLayouts` names the LAYOUTS the sets are allocated from - not the sets themselves, which
     * do not exist yet. The two are both opaque 64-bit handles, so getting it wrong compiles with
     * only a warning and then fails inside the driver. Caught here by MSVC's C4133. */
    VkDescriptorSetLayout set_layouts[2] = { m->set_layout, m->set_layout };
    VkDescriptorSetAllocateInfo dsai = {0};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = m->pool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = set_layouts;
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    if (m->allocate_descriptor_sets(m->device, &dsai, sets) != VK_SUCCESS) {
        *why = "no descriptor for the menu";
        return 0;
    }
    m->set = sets[0];
    m->compare_set = sets[1];

    VkComputePipelineCreateInfo cpci = {0};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = m->shader;
    cpci.stage.pName = "main";
    cpci.layout = m->layout;
    if (m->create_compute_pipelines(m->device, VK_NULL_HANDLE, 1, &cpci, NULL, &m->pipeline)
        != VK_SUCCESS) {
        *why = "the menu's pipeline will not build";
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ create / destroy */

/* Every entry point, resolved once. A macro rather than a table so the field and the name cannot
 * drift apart, and so a missing one is caught at create with the name it wanted. */
#define RESOLVE(field, name) \
    do { \
        m->field = (void *)m->resolve_device(m->device, name); \
        if (!m->field) { *why = "the driver has no " name; goto fail; } \
    } while (0)
#define RESOLVE_I(field, name) \
    do { \
        m->field = (void *)m->resolve_instance(m->physical, name); \
        if (!m->field) { *why = "the driver has no " name; goto fail; } \
    } while (0)

nr_menu *nr_menu_create(void *device_void, void *physical_void, uint32_t queue_family,
                        uint32_t format, nr_menu_resolve_fn resolve_device,
                        nr_menu_resolve_fn resolve_instance, const char **why)
{
    static const char *last_why = NULL;
    VkDevice device = (VkDevice)device_void;
    VkPhysicalDevice physical = (VkPhysicalDevice)physical_void;

    if (!resolve_device || !resolve_instance) {
        last_why = "the menu was given no way to reach Vulkan";
        if (why) *why = last_why;
        return NULL;
    }

    /* The format gate is below, against `menu_formats`, which is the list of what the shader has a
     * build for. It used to be here, as a BGRA8-only check - and that check is what refused the
     * menu on the first real game, whose surface is A2B10G10R10. Keeping both would have left the
     * old refusal in front of the new table, which is exactly the bug this comment replaces. */

    nr_menu *m = (nr_menu *)calloc(1, sizeof *m);
    if (!m) {
        last_why = "out of memory";
        if (why) *why = last_why;
        return NULL;
    }
    m->device = device;
    m->physical = physical;
    m->queue_family = queue_family;
    m->format = (VkFormat)format;
    m->resolve_device = resolve_device;
    m->resolve_instance = resolve_instance;
    m->compare_split = 0.5f;
    m->language = NR_MENU_LANG_EN;

    /* Which build of the shader this format needs, before anything is resolved: an unsupported
     * format should cost nothing to refuse. The five formats here are the ones the layer itself
     * already tracks for its own copies, so a surface the layer can capture from is one the menu
     * can draw on - which is the property worth keeping, rather than a second list that could
     * disagree with the first. */
    const struct nr_menu_format *fmt = menu_format_for(m->format);
    if (!fmt) {
        last_why = "this surface's format is not one the menu can draw on";
        if (why) *why = last_why;
        free(m);
        return NULL;
    }
    m->rgb10a2 = fmt->rgb10a2;

    RESOLVE_I(get_memory_properties, "vkGetPhysicalDeviceMemoryProperties");
    RESOLVE_I(get_format_properties, "vkGetPhysicalDeviceFormatProperties");
    RESOLVE(create_image, "vkCreateImage");
    RESOLVE(destroy_image, "vkDestroyImage");
    RESOLVE(get_image_memory_requirements, "vkGetImageMemoryRequirements");
    RESOLVE(allocate_memory, "vkAllocateMemory");
    RESOLVE(free_memory, "vkFreeMemory");
    RESOLVE(bind_image_memory, "vkBindImageMemory");
    RESOLVE(bind_buffer_memory, "vkBindBufferMemory");
    RESOLVE(create_buffer, "vkCreateBuffer");
    RESOLVE(destroy_buffer, "vkDestroyBuffer");
    RESOLVE(get_buffer_memory_requirements, "vkGetBufferMemoryRequirements");
    RESOLVE(map_memory, "vkMapMemory");
    RESOLVE(unmap_memory, "vkUnmapMemory");
    /* No `vkGetBufferDeviceAddress` here, deliberately. The quads and the atlas are ordinary
     * storage buffers in the layer's own descriptor set - see the note in menu.comp for why the
     * address route was abandoned: it needs a Vulkan 1.1 device and the game this runs in does not
     * declare one. */
    RESOLVE(create_image_view, "vkCreateImageView");
    RESOLVE(destroy_image_view, "vkDestroyImageView");
    RESOLVE(create_shader_module, "vkCreateShaderModule");
    RESOLVE(destroy_shader_module, "vkDestroyShaderModule");
    RESOLVE(create_descriptor_set_layout, "vkCreateDescriptorSetLayout");
    RESOLVE(destroy_descriptor_set_layout, "vkDestroyDescriptorSetLayout");
    RESOLVE(create_pipeline_layout, "vkCreatePipelineLayout");
    RESOLVE(destroy_pipeline_layout, "vkDestroyPipelineLayout");
    RESOLVE(create_descriptor_pool, "vkCreateDescriptorPool");
    RESOLVE(destroy_descriptor_pool, "vkDestroyDescriptorPool");
    RESOLVE(allocate_descriptor_sets, "vkAllocateDescriptorSets");
    RESOLVE(free_descriptor_sets, "vkFreeDescriptorSets");
    RESOLVE(update_descriptor_sets, "vkUpdateDescriptorSets");
    RESOLVE(create_compute_pipelines, "vkCreateComputePipelines");
    RESOLVE(destroy_pipeline, "vkDestroyPipeline");
    RESOLVE(cmd_bind_pipeline, "vkCmdBindPipeline");
    RESOLVE(cmd_bind_descriptor, "vkCmdBindDescriptorSets");
    RESOLVE(cmd_push_constants, "vkCmdPushConstants");
    RESOLVE(cmd_dispatch, "vkCmdDispatch");
    RESOLVE(cmd_copy_buffer_to_image, "vkCmdCopyBufferToImage");
    RESOLVE(cmd_pipeline_barrier, "vkCmdPipelineBarrier");
#undef RESOLVE
#undef RESOLVE_I

    /* The format question, before anything is built. The swapchain path requests TRANSFER only,
     * which says nothing about whether the format can be a storage image. Refusing here is what
     * keeps an unsupported surface usable without the menu. */
    if (!format_supports_storage(m, m->format, why))
        goto fail;

    if (!build_atlas(m, why) || !build_pipeline(m, why) || !build_quads(m, why))
        goto fail;

    last_why = NULL;
    return m;

fail:
    nr_menu_destroy(m);
    if (why) last_why = *why;
    return NULL;
}

void nr_menu_destroy(nr_menu *m)
{
    if (!m) return;
    if (m->device) {
        for (int i = 0; i < NR_VIEW_CACHE; ++i)
            if (m->views[i].view != VK_NULL_HANDLE && m->destroy_image_view)
                m->destroy_image_view(m->device, m->views[i].view, NULL);
        if (m->reference_view && m->destroy_image_view)
            m->destroy_image_view(m->device, m->reference_view, NULL);
        if (m->reference && m->destroy_image)
            m->destroy_image(m->device, m->reference, NULL);
        if (m->reference_memory && m->free_memory)
            m->free_memory(m->device, m->reference_memory, NULL);
        if (m->pipeline && m->destroy_pipeline)
            m->destroy_pipeline(m->device, m->pipeline, NULL);
        if (m->layout && m->destroy_pipeline_layout)
            m->destroy_pipeline_layout(m->device, m->layout, NULL);
        if (m->set_layout && m->destroy_descriptor_set_layout)
            m->destroy_descriptor_set_layout(m->device, m->set_layout, NULL);
        if (m->pool && m->destroy_descriptor_pool)
            m->destroy_descriptor_pool(m->device, m->pool, NULL);
        if (m->shader && m->destroy_shader_module)
            m->destroy_shader_module(m->device, m->shader, NULL);
        if (m->quad_buffer && m->destroy_buffer)
            m->destroy_buffer(m->device, m->quad_buffer, NULL);
        if (m->quad_memory && m->free_memory)
            m->free_memory(m->device, m->quad_memory, NULL);
        if (m->atlas_buffer && m->destroy_buffer)
            m->destroy_buffer(m->device, m->atlas_buffer, NULL);
        if (m->atlas_memory && m->free_memory)
            m->free_memory(m->device, m->atlas_memory, NULL);
    }
    nr_font_free(&m->font);
    free(m);
}

int nr_menu_is_open(const nr_menu *m) { return m ? m->open : 0; }
int nr_menu_selected(const nr_menu *m) { return m ? m->selected : 0; }
int nr_menu_font_ready(const nr_menu *m) { return m && m->font.cell_count > 0; }

/* Whether a command buffer is worth opening this frame. The panel open is the obvious case; the
 * comparison counts too, because it keeps working after the menu is closed - that is what makes it
 * usable for playing. Neither true is ordinary play, and then nothing is submitted at all. */
int nr_menu_needs_composite(const nr_menu *m)
{
    if (!m) return 0;
    return m->open || m->compare_mode != NR_MENU_COMPARE_OFF;
}

/* ------------------------------------------------------------------ per frame */

int nr_menu_update(nr_menu *m, float frame_seconds)
{
    if (!m) return 0;
    m->elapsed += frame_seconds;

    /* The three that act with the menu closed. A game keeps its own input for everything else,
     * which is why these are polled first and the rest only inside the `if`. */
    /* INS opens and closes the panel. In the ImGui build the panel is ImGui's window and this
     * reader must not also act on the same key - two owners of one key is what made Enter open a
     * combo and change the value behind it at the same time. `nr_menu_set_key_reading` is how the
     * layer says which build is running. */
    if (m->read_keys && key_pressed(m, NR_KEY_INS)) m->open = !m->open;
    if (key_pressed(m, NR_KEY_TOGGLE)) m->toggle_pressed = 1;
    if (key_pressed(m, NR_KEY_COMPARE)) {
        m->compare_mode = (m->compare_mode + 1) % NR_MENU_COMPARE_COUNT;
        m->compare_changed = 1;
    }

    int was_open = m->open;
    if (m->open && m->read_keys) {
        if (key_pressed(m, NR_KEY_UP) && m->row_count > 0)
            m->selected = (m->selected + m->row_count - 1) % m->row_count;
        if (key_pressed(m, NR_KEY_DOWN) && m->row_count > 0)
            m->selected = (m->selected + 1) % m->row_count;
        if (key_pressed(m, NR_KEY_LEFT))  m->step = -1;
        if (key_pressed(m, NR_KEY_RIGHT)) m->step = +1;
        if (key_pressed(m, NR_KEY_ENTER)) m->step = +1;
        if (key_pressed(m, NR_KEY_S)) m->save_pressed = 1;
        if (key_pressed(m, NR_KEY_R)) m->reset_pressed = 1;
        if (key_pressed(m, NR_KEY_C)) {
            m->compare_mode = (m->compare_mode + 1) % NR_MENU_COMPARE_COUNT;
            m->compare_changed = 1;
        }
        if (key_pressed(m, NR_KEY_L)) {
            m->language = (m->language + 1) % NR_MENU_LANG_COUNT;
            m->language_changed = 1;
        }
        if (key_pressed(m, NR_KEY_ESC)) m->open = 0;

        /* Shift and an arrow move the split, and are the one key pair the menu wants held rather
         * than tapped - a wipe is adjusted by eye, not by counting presses. Shift is what
         * separates them from the arrow keys that move the cursor. */
        if (key_down(m, NR_KEY_SHIFT) && m->compare_mode != NR_MENU_COMPARE_OFF) {
            if (key_down(m, NR_KEY_LEFT))  m->split_step = -1.0f;
            if (key_down(m, NR_KEY_RIGHT)) m->split_step = +1.0f;
        }
    }
    if (m->open != was_open) return 2;
    return m->open ? 1 : 0;
}

int nr_menu_take_toggle_request(nr_menu *m)
{
    if (!m || !m->toggle_pressed) return 0;
    m->toggle_pressed = 0;
    return 1;
}

void nr_menu_set_nr_enabled(nr_menu *m, int enabled) { if (m) m->nr_enabled = enabled; }
int  nr_menu_nr_enabled(const nr_menu *m) { return m ? m->nr_enabled : 0; }

int nr_menu_take_compare_change(nr_menu *m, int *mode)
{
    if (!m || !m->compare_changed) return 0;
    m->compare_changed = 0;
    if (mode) *mode = m->compare_mode;
    return 1;
}

int nr_menu_compare_mode(const nr_menu *m) { return m ? m->compare_mode : 0; }

float nr_menu_take_split_step(nr_menu *m)
{
    if (!m) return 0.0f;
    float s = m->split_step;
    m->split_step = 0.0f;
    return s;
}

void nr_menu_set_split(nr_menu *m, float split)
{
    if (!m) return;
    if (split < 0.0f) split = 0.0f;
    if (split > 1.0f) split = 1.0f;
    m->compare_split = split;
}

float nr_menu_split(const nr_menu *m) { return m ? m->compare_split : 0.5f; }

int nr_menu_take_save_request(nr_menu *m)
{
    if (!m || !m->save_pressed) return 0;
    m->save_pressed = 0;
    return 1;
}

int nr_menu_take_language_change(nr_menu *m, int *language)
{
    if (!m || !m->language_changed) return 0;
    m->language_changed = 0;
    if (language) *language = m->language;
    return 1;
}

int nr_menu_take_reset(nr_menu *m)
{
    if (!m || !m->reset_pressed) return 0;
    m->reset_pressed = 0;
    return 1;
}

int nr_menu_take_step(nr_menu *m)
{
    if (!m) return 0;
    int s = m->step;
    m->step = 0;
    return s;
}

/* What a key would have asked for, for a caller with no keyboard. See nr_menu.h: these set the
 * same flags the key reading sets, so the path through the bridge is the one a keypress takes and
 * a test is not exercising a second implementation of it. */
void nr_menu_inject_step(nr_menu *m, int direction)
{
    if (m) m->step = direction < 0 ? -1 : (direction > 0 ? 1 : 0);
}

void nr_menu_inject_save(nr_menu *m)
{
    if (m) m->save_pressed = 1;
}

void nr_menu_set_key_reading(nr_menu *m, int on)
{
    if (m) m->read_keys = on ? 1 : 0;
}

void nr_menu_inject_reset(nr_menu *m)
{
    if (m) m->reset_pressed = 1;
}

void nr_menu_inject_language(nr_menu *m, int language)
{
    if (!m) return;
    if (language < 0 || language >= NR_MENU_LANG_COUNT) return;
    m->language = language;
    /* The same flag the L key sets, so the bridge's own handler does the work and the two ways in
     * cannot disagree about what changing the language means. */
    m->language_changed = 1;
}

/* The language the menu is showing. The layer needs it to cycle with L: the injector takes an
 * absolute index, so stepping it means reading what it currently is. Ownership of the setting
 * stays here - the layer only asks and then hands back a new absolute value. */
int nr_menu_language(const nr_menu *m)
{
    if (!m) return 0;
    if (m->language < 0 || m->language >= NR_MENU_LANG_COUNT) return 0;
    return m->language;
}

void nr_menu_inject_select(nr_menu *m, int row)
{
    if (!m) return;
    if (row < 0) row = 0;
    if (m->row_count > 0 && row >= m->row_count) row = m->row_count - 1;
    m->selected = row;
}

void nr_menu_inject_open(nr_menu *m, int open)
{
    if (m) m->open = open ? 1 : 0;
}

void nr_menu_set_rows(nr_menu *m, const nr_menu_row *rows, int count)
{
    if (!m) return;
    m->rows = rows;
    m->row_count = count;
    if (m->selected >= count) m->selected = count > 0 ? count - 1 : 0;
}

void nr_menu_set_title(nr_menu *m, const char *title) { if (m) m->title = title; }
void nr_menu_set_hint(nr_menu *m, const char *hint)   { if (m) m->hint = hint; }

void nr_menu_set_note(nr_menu *m, const char *note)
{
    if (!m) return;
    m->note = note;
    m->note_until = m->elapsed + 2.0f;
}

/* ------------------------------------------------------------------ image views */

/* A swapchain image is not a view, and writing to one as a storage image needs a view. Making one
 * a frame would allocate and free every frame inside somebody's present path; a swapchain has a
 * small fixed set of images that recur, so the views are kept and looked up by handle.
 *
 * Linear and bounded: a swapchain is three to five images, and this runs once a frame. */
static VkImageView image_view_for(nr_menu *m, VkImage image)
{
    for (int i = 0; i < NR_VIEW_CACHE; ++i)
        if (m->views[i].image == image && m->views[i].view != VK_NULL_HANDLE)
            return m->views[i].view;

    /* A new image: the swapchain was recreated, so the old views are dead. Dropping them here
     * rather than tracking recreation is correct and one line - the images they named are gone,
     * and a stale view would be a use-after-free the driver reports as a device loss. */
    for (int i = 0; i < NR_VIEW_CACHE; ++i) {
        if (m->views[i].view != VK_NULL_HANDLE) {
            m->destroy_image_view(m->device, m->views[i].view, NULL);
            m->views[i].view = VK_NULL_HANDLE;
            m->views[i].image = VK_NULL_HANDLE;
        }
    }

    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = m->format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (m->create_image_view(m->device, &vci, NULL, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    m->views[0].image = image;
    m->views[0].view = view;
    return view;
}

/* The reference image the comparison samples: device-local, the swapchain's size, recreated when
 * the swapchain changes size. Created lazily because a game that never presses the compare key
 * should not pay for it. */
static int ensure_reference(nr_menu *m, uint32_t width, uint32_t height, const char **why)
{
    if (m->reference && m->reference_w == width && m->reference_h == height)
        return 1;

    if (m->reference_view) { m->destroy_image_view(m->device, m->reference_view, NULL);
                             m->reference_view = VK_NULL_HANDLE; }
    if (m->reference)      { m->destroy_image(m->device, m->reference, NULL);
                             m->reference = VK_NULL_HANDLE; }
    if (m->reference_memory) { m->free_memory(m->device, m->reference_memory, NULL);
                               m->reference_memory = VK_NULL_HANDLE; }

    VkImageCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = m->format;
    ici.extent.width = width;
    ici.extent.height = height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* STORAGE so the comparison can read it, TRANSFER_DST so the caller's staging buffer can be
     * copied into it. No host access: the point of the exercise is that nothing comes back. */
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m->create_image(m->device, &ici, NULL, &m->reference) != VK_SUCCESS) {
        *why = "no reference image for the comparison";
        return 0;
    }

    VkMemoryRequirements req;
    m->get_image_memory_requirements(m->device, m->reference, &req);
    uint32_t type = find_memory_type(m, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        *why = "no device-local memory for the comparison";
        return 0;
    }
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (m->allocate_memory(m->device, &mai, NULL, &m->reference_memory) != VK_SUCCESS) {
        *why = "no memory for the comparison";
        return 0;
    }
    if (m->bind_image_memory(m->device, m->reference, m->reference_memory, 0) != VK_SUCCESS) {
        *why = "cannot bind the comparison image";
        return 0;
    }

    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = m->reference;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = m->format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (m->create_image_view(m->device, &vci, NULL, &m->reference_view) != VK_SUCCESS) {
        *why = "no comparison image view";
        return 0;
    }

    m->reference_w = width;
    m->reference_h = height;
    return 1;
}

/* ------------------------------------------------------------------ the layout */

static void quad_push(nr_menu *m, uint32_t *used, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, nr_colour colour, uint32_t flags)
{
    if (*used >= m->quad_capacity) return;
    nr_menu_quad *q = &((nr_menu_quad *)m->quad_mapped)[*used];
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
    q->cr = (float)colour.r / 255.0f;
    q->cg = (float)colour.g / 255.0f;
    q->cb = (float)colour.b / 255.0f;
    q->ca = (float)colour.a / 255.0f;
    q->flags = flags;
    (*used)++;
}

/* UTF-8 decode, so a Chinese or Russian string is one glyph a character rather than three. */
static int utf8_next(const char **p, int *codepoint)
{
    const uint8_t *s = (const uint8_t *)*p;
    int cp = s[0], n = 1;
    if (cp >= 0xF0) { cp = s[0] & 0x07; n = 4; }
    else if (cp >= 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else if (cp >= 0xC0) { cp = s[0] & 0x1F; n = 2; }
    if (n > 1) {
        for (int i = 1; i < n; ++i) {
            if ((s[i] & 0xC0) != 0x80) { cp = s[0]; n = 1; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
    }
    *p += n;
    *codepoint = cp;
    return n;
}

static void text_push(nr_menu *m, uint32_t *used, float x, float y, const char *s,
                      nr_colour colour, int scale)
{
    if (!s) return;
    float cx = x;
    while (*s) {
        int cp;
        utf8_next(&s, &cp);
        if (cp == '\n') continue;
        const struct nr_font_cell *cell = nr_font_cell_for(&m->font, cp);
        if (!cell) { cx += 4 * scale; continue; }   /* a space, or a glyph we do not have */
        quad_push(m, used, cx, y,
                  (float)cell->w * scale, (float)cell->h * scale,
                  (float)cell->x / (float)m->font.width,
                  (float)cell->y / (float)m->font.height,
                  (float)(cell->x + cell->w) / (float)m->font.width,
                  (float)(cell->y + cell->h) / (float)m->font.height,
                  colour, 0u);
        cx += (float)cell->w * scale;
    }
}

static float text_width(const nr_menu *m, const char *s, int scale)
{
    if (!s) return 0;
    float w = 0;
    while (*s) {
        int cp;
        utf8_next(&s, &cp);
        const struct nr_font_cell *cell = nr_font_cell_for(&m->font, cp);
        w += (cell ? (float)cell->w : 4.0f) * scale;
    }
    return w;
}

/* Wrapped text, three lines at most. The widths come from the atlas, so a line breaks where the
 * glyphs actually end rather than where a count of characters guesses - which matters most for
 * Chinese, where every character is a different width from every Latin one. */
static void text_push_wrapped(nr_menu *m, uint32_t *used, float x, float y, float max_width,
                              const char *s, nr_colour colour)
{
    if (!s) return;
    char line[512];
    size_t n = 0;
    float cx = x, cy = y;
    const float line_height = (float)NR_FONT_CELL_H + 2.0f;

    line[0] = '\0';
    while (*s) {
        int cp;
        const char *start = s;
        utf8_next(&s, &cp);
        size_t bytes = (size_t)(s - start);

        float glyph = text_width(m, start, 1);
        if (cx > x + max_width || (cp == ' ' && cx + glyph > x + max_width)) {
            text_push(m, used, x, cy, line, colour, 1);
            n = 0;
            line[0] = '\0';
            cx = x;
            cy += line_height;
            if (cy > y + line_height * 2) return;   /* three lines is enough */
            if (cp == ' ') continue;                /* do not start a line with the break */
        }
        if (n + bytes < sizeof line - 1) {
            memcpy(line + n, start, bytes);
            n += bytes;
            line[n] = '\0';
            cx += glyph;
        }
    }
    if (n) text_push(m, used, x, cy, line, colour, 1);
}

/* Lay the frame out into the quad buffer and dispatch. The layout is here rather than in the
 * shader because the shader has no table: positions and colours are decided once, on the host, and
 * the shader is a rectangle test. */
void nr_menu_draw(nr_menu *m, uint64_t cmd_handle, uint64_t image_handle,
                  uint32_t width, uint32_t height)
{
    if (!m || !m->open || !m->pipeline || !m->rows) return;

    /* The handles are integers, not pointers - see the note in nr_menu.h. Casting here rather than
     * at every call site keeps the mismatch from being a thing each caller has to remember. */
    VkCommandBuffer cmd = (VkCommandBuffer)(uintptr_t)cmd_handle;
    VkImage image = (VkImage)image_handle;

    uint32_t used = 0;

    /* The panel fits the image, and is centred in it.
     *
     * `NR_PANEL_W`/`NR_PANEL_H` are what the layout wants; the image is what it gets. A swapchain
     * can be any size - 640x360 on the first run that tested this, 1920x1080 on a full screen -
     * and a panel taller than the image means the dispatch covers rows that do not exist.
     * `imageStore` past the end of an image is undefined, and the quiet end of "undefined" is
     * that nothing appears, which is exactly what happened: 323 quads built, submitted, waited,
     * and not one pixel on screen.
     *
     * The margin keeps a border of game visible, so it is obvious the panel is an overlay. */
    float panel_w = (float)NR_PANEL_W;
    float panel_h = (float)NR_PANEL_H;
    if (panel_w > (float)width  - 8.0f) panel_w = (float)width  - 8.0f;
    if (panel_h > (float)height - 8.0f) panel_h = (float)height - 8.0f;
    if (panel_w < 64.0f) panel_w = 64.0f;
    if (panel_h < 64.0f) panel_h = 64.0f;

    float px = ((float)width  - panel_w) / 2.0f;
    float py = ((float)height - panel_h) / 2.0f;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    const nr_colour bg      = COLOUR(0x10, 0x12, 0x16, 0xF2);
    const nr_colour border  = COLOUR(0x4A, 0x52, 0x60, 0xFF);
    const nr_colour title_c = COLOUR(0xEA, 0xEE, 0xF4, 0xFF);
    const nr_colour label_c = COLOUR(0xC6, 0xCC, 0xD6, 0xFF);
    const nr_colour value_c = COLOUR(0x7F, 0xD1, 0xFF, 0xFF);
    const nr_colour sel_c   = COLOUR(0x2A, 0x38, 0x4C, 0xFF);
    const nr_colour hint_c  = COLOUR(0x8A, 0x92, 0x9E, 0xFF);
    const nr_colour note_c  = COLOUR(0x8C, 0xD9, 0x7A, 0xFF);
    const nr_colour off_c   = COLOUR(0xE0, 0x6C, 0x6C, 0xFF);

    /* The border first, so the fill sits inside it: two solid quads, no outline primitive. */
    quad_push(m, &used, px - 1, py - 1, panel_w + 2, panel_h + 2,
              0, 0, 0, 0, border, NR_QUAD_SOLID);
    quad_push(m, &used, px, py, panel_w, panel_h,
              0, 0, 0, 0, bg, NR_QUAD_SOLID);

    float y = py + 10;
    if (m->title) {
        text_push(m, &used, px + NR_TEXT_PAD, y, m->title, title_c, NR_TITLE_SCALE);
        y += (float)NR_FONT_CELL_H * NR_TITLE_SCALE + 6.0f;
    }

    /* NR's own state, on its own line: the toggle key changes it with the menu closed, so the one
     * thing a user needs to see is whether the effect is on. */
    {
        char state[192];
        snprintf(state, sizeof state, "%s: %s",
                 nr_menu_text(m->language, NR_STR_TOGGLE_NR),
                 nr_menu_text(m->language, m->nr_enabled ? NR_STR_ON : NR_STR_OFF));
        text_push(m, &used, px + NR_TEXT_PAD, y, state, m->nr_enabled ? note_c : off_c, 1);
        y += NR_ROW_H;
    }

    for (int i = 0; i < m->row_count; ++i) {
        const nr_menu_row *r = &m->rows[i];
        if (i == m->selected)
            quad_push(m, &used, px + 4, y - 2, panel_w - 8, (float)NR_ROW_H,
                      0, 0, 0, 0, sel_c, NR_QUAD_SOLID);

        text_push(m, &used, px + NR_TEXT_PAD, y, r->label, label_c, 1);
        if (r->value) {
            /* Right-aligned, so a column of values reads as a column. A choice gets the arrows
             * that say it cycles, which is the only hint the widget itself gives. */
            char shown[128];
            if (r->is_choice) snprintf(shown, sizeof shown, "< %s >", r->value);
            else              snprintf(shown, sizeof shown, "%s", r->value);
            float w = text_width(m, shown, 1);
            text_push(m, &used, px + panel_w - NR_TEXT_PAD - w, y, shown, value_c, 1);
        }
        y += NR_ROW_H;
    }

    /* The comparison's own settings, so the split is adjustable from inside the menu. */
    if (m->compare_mode != NR_MENU_COMPARE_OFF) {
        y += 4;
        char line[224];
        snprintf(line, sizeof line, "%s: %s   %s: %.2f   (Shift+Left/Right)",
                 nr_menu_text(m->language, NR_STR_COMPARE),
                 nr_menu_compare_names[m->language][m->compare_mode],
                 nr_menu_text(m->language, NR_STR_COMPARE_SPLIT), (double)m->compare_split);
        text_push(m, &used, px + NR_TEXT_PAD, y, line, value_c, 1);
        y += NR_ROW_H;
    }

    if (m->note && m->elapsed < m->note_until) {
        y += 2;
        text_push(m, &used, px + NR_TEXT_PAD, y, m->note, note_c, 1);
    }

    /* The selected row's own hint, above the key list: a knob's explanation is longer than the row
     * is wide and this is the only place it is ever shown. */
    if (m->selected < m->row_count && m->rows[m->selected].hint) {
        float hy = py + panel_h - NR_FONT_CELL_H - 6
                 - ((float)NR_FONT_CELL_H + 2.0f) * 3.0f - 2.0f;
        text_push_wrapped(m, &used, px + NR_TEXT_PAD, hy,
                          panel_w - NR_TEXT_PAD * 2,
                          m->rows[m->selected].hint, hint_c);
    }

    if (m->hint)
        text_push(m, &used, px + NR_TEXT_PAD, py + panel_h - NR_FONT_CELL_H - 6,
                  m->hint, hint_c, 1);

    if (used == 0) return;


    /* Point the descriptors at this frame's image, so the menu writes where the game was about to
     * hand the driver. Re-pointed every frame because a swapchain has several images and the layer
     * presents whichever the driver gave it. */
    VkImageView view = image_view_for(m, image);
    if (view == VK_NULL_HANDLE) {
        return;
    }

    VkDescriptorImageInfo dii[2] = {{0}, {0}};
    dii[0].imageView = view;
    dii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    /* The reference slot gets the image itself when there is nothing to compare, so the shader
     * never reads an unbound descriptor - which on a validation layer is an error and on a driver
     * is undefined. The panel pass does not read it, but binding it costs nothing. */
    dii[1].imageView = m->reference_view ? m->reference_view : view;
    dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    /* The two buffers the shader reads its quads and its glyphs from. */
    VkDescriptorBufferInfo buffers[2] = {{0}, {0}};
    buffers[0].buffer = m->quad_buffer;
    buffers[0].offset = 0;
    buffers[0].range = VK_WHOLE_SIZE;
    buffers[1].buffer = m->atlas_buffer;
    buffers[1].offset = 0;
    buffers[1].range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[4] = {{0}, {0}, {0}, {0}};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m->set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &dii[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i - 2];
        }
    }
    m->update_descriptor_sets(m->device, 4, writes, 0, NULL);

    /* `cmd` is the caller's, already recording. The menu never begins, ends or submits one: the
     * submit is where the semaphore contract lives and the caller owns it. */
    m->cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipeline);
    m->cmd_bind_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 1, &m->set, 0, NULL);

    nr_menu_push push;
    memset(&push, 0, sizeof push);
    push.count = used;
    push.panel_x = px;
    push.panel_y = py;
    push.panel_w = panel_w;
    push.panel_h = panel_h;
    push.atlas_w = m->font.width;
    push.atlas_h = m->font.height;
    push.compare_split = m->compare_split;
    push.compare_swap = (uint32_t)m->compare_swap;
    push.compare_mode = 0;   /* the panel pass draws no comparison */
    m->cmd_push_constants(cmd, m->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);

    /* Exactly the panel, so the cost is the menu's size and not the game's resolution. */
    m->cmd_dispatch(cmd, ((uint32_t)panel_w + 7) / 8, ((uint32_t)panel_h + 7) / 8, 1);

}

/* The ImGui overlay: mode 3 of the menu shader, over the whole image.
 *
 * Same binding and descriptor dance as `nr_menu_draw_compare` - which is where this file's
 * storage-image machinery lives - with the UI's view where the reference image goes. The push
 * block's panel_size is the full image, because a whole-frame pass has no panel rectangle.
 */
void nr_menu_draw_overlay(nr_menu *m, uint64_t cmd_handle, uint64_t target_handle,
                          uint64_t ui_view_handle, uint32_t width, uint32_t height)
{
    if (!m || !m->pipeline) return;

    VkCommandBuffer cmd = (VkCommandBuffer)(uintptr_t)cmd_handle;
    VkImage target = (VkImage)target_handle;
    VkImageView ui_view = (VkImageView)(uintptr_t)ui_view_handle;
    if (ui_view == VK_NULL_HANDLE) return;

    VkImageView target_view = image_view_for(m, target);
    if (target_view == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo dii[2] = {{0}, {0}};
    dii[0].imageView = target_view;
    dii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii[1].imageView = ui_view;
    dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;   /* storage images live in GENERAL */

    VkDescriptorBufferInfo buffers[2] = {{0}, {0}};
    buffers[0].buffer = m->quad_buffer;
    buffers[0].range = VK_WHOLE_SIZE;
    buffers[1].buffer = m->atlas_buffer;
    buffers[1].range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[4] = {{0}, {0}, {0}, {0}};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m->compare_set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &dii[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i - 2];
        }
    }
    m->update_descriptor_sets(m->device, 4, writes, 0, NULL);

    m->cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipeline);
    m->cmd_bind_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 1,
                           &m->compare_set, 0, NULL);

    nr_menu_push push;
    memset(&push, 0, sizeof push);
    push.count = 0;
    push.panel_x = 0;
    push.panel_y = 0;
    push.panel_w = (float)width;
    push.panel_h = (float)height;
    push.atlas_w = m->font.width;
    push.atlas_h = m->font.height;
    push.compare_split = 0.5f;
    push.compare_swap = 0;
    push.compare_mode = 3u;      /* the ImGui overlay */
    m->cmd_push_constants(cmd, m->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    m->cmd_dispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

void nr_menu_draw_compare(nr_menu *m, uint64_t cmd_handle, uint64_t image_handle,
                          uint64_t reference_handle, uint32_t width, uint32_t height)
{
    if (!m || !m->pipeline) return;
    if (m->compare_mode == NR_MENU_COMPARE_OFF) return;
    if (!reference_handle) return;          /* nothing to compare against this frame */

    const char *why = NULL;
    if (!ensure_reference(m, width, height, &why)) {
        static int announced = 0;
        if (!announced) {
            fprintf(stderr, "[nr_layer] comparison off: %s\n", why ? why : "unknown");
            announced = 1;
        }
        m->compare_mode = NR_MENU_COMPARE_OFF;
        return;
    }

    VkCommandBuffer cmd = (VkCommandBuffer)(uintptr_t)cmd_handle;
    VkImage image = (VkImage)image_handle;
    uint32_t used = 0;

    /* The game's own frame is in the caller's staging buffer - it is what the layer copies the
     * swapchain into - and the copy into the reference image is GPU to GPU, so nothing is read
     * back. The barriers are on this command buffer, which the caller is already recording. */
    VkImageMemoryBarrier into = {0};
    into.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    into.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    into.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    into.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.image = m->reference;
    into.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    into.subresourceRange.levelCount = 1;
    into.subresourceRange.layerCount = 1;
    into.srcAccessMask = 0;
    into.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    m->cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &into);

    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = (VkExtent3D){ width, height, 1 };
    m->cmd_copy_buffer_to_image(cmd, (VkBuffer)(uintptr_t)reference_handle, m->reference,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_general = into;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    m->cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                            &to_general);

    /* One quad over the whole image, drawn from the reference, which the shader cuts at the split
     * against the image itself. The mode decides whether that is a wipe or a squeeze. */
    quad_push(m, &used, 0, 0, (float)width, (float)height, 0, 0, 1, 1,
              COLOUR(0xFF, 0xFF, 0xFF, 0xFF), NR_QUAD_REFERENCE);

    VkImageView target = image_view_for(m, image);
    if (target == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo dii[2] = {{0}, {0}};
    dii[0].imageView = target;
    dii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii[1].imageView = m->reference_view;
    dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    /* The comparison reads no quads and no glyphs, but the descriptor set is the same layout, so
     * the two buffer bindings have to be filled - an unbound binding in a set that is bound is
     * undefined behaviour even when the shader path does not reach it. */
    VkDescriptorBufferInfo buffers[2] = {{0}, {0}};
    buffers[0].buffer = m->quad_buffer;
    buffers[0].offset = 0;
    buffers[0].range = VK_WHOLE_SIZE;
    buffers[1].buffer = m->atlas_buffer;
    buffers[1].offset = 0;
    buffers[1].range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[4] = {{0}, {0}, {0}, {0}};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m->compare_set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &dii[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i - 2];
        }
    }
    m->update_descriptor_sets(m->device, 4, writes, 0, NULL);

    m->cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipeline);
    m->cmd_bind_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 1,
                           &m->compare_set, 0, NULL);

    nr_menu_push push;
    memset(&push, 0, sizeof push);
    push.count = used;
    push.panel_x = 0;
    push.panel_y = 0;
    push.panel_w = (float)width;
    push.panel_h = (float)height;
    push.atlas_w = m->font.width;
    push.atlas_h = m->font.height;
    push.compare_split = m->compare_split;
    push.compare_swap = (uint32_t)m->compare_swap;
    push.compare_mode = (uint32_t)m->compare_mode;
    m->cmd_push_constants(cmd, m->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);

    /* The whole image: (width+7)/8 by (height+7)/8 groups. At 1280x720 that is 160x90, or
     * 115 200 invocations - a few tenths of a millisecond against a frame that costs tens. */
    m->cmd_dispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}
