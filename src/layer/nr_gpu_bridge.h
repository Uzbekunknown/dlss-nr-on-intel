#ifndef NR_GPU_BRIDGE_H
#define NR_GPU_BRIDGE_H
/* Experimental same-device Win32 Vulkan transport. No pixel mappings, no
 * device-wide waits, and no mutation of an application's pNext chain. Include
 * after windows.h/vulkan.h with VK_USE_PLATFORM_WIN32_KHR enabled. */
#ifdef _WIN32
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "nr_gpu_wire.h"

#define NR_GPU_BUFFER_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT \
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
#define NR_GPU_MEMORY_TYPE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define NR_GPU_SEMAPHORE_TYPE VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define NR_GPU_WAIT_NS UINT64_C(10000000000)
typedef char nr_gpu_uuid_is_vulkans[(VK_UUID_SIZE == NR_GPU_UUID_BYTES) ? 1 : -1];

struct nr_gpu_device_patch {
    VkDeviceCreateInfo info;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda;
    const char **extensions;
    const char *reason;
    int enabled;
};

static int nr_gpu_extension_present(const char *name, uint32_t count, const char *const *names)
{
    for (uint32_t i = 0; i < count; ++i)
        if (names[i] && !strcmp(name, names[i])) return 1;
    return 0;
}

/* The caller uses patch.info for creation only when this returns 1, and frees
 * patch after vkCreateDevice returns. A patched creation may be retried with the
 * untouched original on failure. Existing disabled BDA features are intentional
 * app choices: do not replace or edit them. */
static int nr_gpu_patch_device(struct nr_gpu_device_patch *patch, VkPhysicalDevice physical,
                              const VkDeviceCreateInfo *original,
                              PFN_vkEnumerateDeviceExtensionProperties enumerate,
                              PFN_vkGetPhysicalDeviceFeatures2 features,
                              PFN_vkGetPhysicalDeviceProperties properties)
{
    static const char *required[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    memset(patch, 0, sizeof *patch);
    if (!original) { patch->reason = "missing device create info"; return 0; }
    patch->info = *original;
    const char *opt = getenv("NR_GPU_TRANSPORT");
    if (!opt || strcmp(opt, "1")) { patch->reason = "GPU transport is opt-in"; return 0; }
    if (!physical || !enumerate || !features || !properties) {
        patch->reason = "physical-device query entry points unavailable"; return 0;
    }
    VkPhysicalDeviceProperties props;
    properties(physical, &props);
    if (props.vendorID != 0x8086u) { patch->reason = "GPU transport requires Intel on both sides"; return 0; }
    if (original->enabledExtensionCount && !original->ppEnabledExtensionNames) {
        patch->reason = "invalid app extension array"; return 0;
    }
    /* Enabling the KHR and EXT variants together is forbidden even when the
     * application's feature chain has no legacy EXT-BDA structure. */
    if (nr_gpu_extension_present(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                original->enabledExtensionCount, original->ppEnabledExtensionNames)) {
        patch->reason = "legacy EXT BDA extension unsupported"; return 0;
    }
    int app_bda = 0, steps = 0;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)original->pNext; p; p = p->pNext) {
        if (++steps > 128) { patch->reason = "app feature chain is too long"; return 0; }
        if (p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) {
            const VkDeviceGroupDeviceCreateInfo *group = (const VkDeviceGroupDeviceCreateInfo *)p;
            if (group->physicalDeviceCount != 1 || !group->pPhysicalDevices || group->pPhysicalDevices[0] != physical) {
                patch->reason = "GPU transport requires a single matching physical device"; return 0;
            }
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            const VkPhysicalDeviceVulkan12Features *f = (const VkPhysicalDeviceVulkan12Features *)p;
            if (!f->bufferDeviceAddress) { patch->reason = "app Vulkan12 features disable BDA"; return 0; }
            app_bda = 1;
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            const VkPhysicalDeviceBufferDeviceAddressFeatures *f = (const VkPhysicalDeviceBufferDeviceAddressFeatures *)p;
            if (!f->bufferDeviceAddress) { patch->reason = "app BDA feature is disabled"; return 0; }
            app_bda = 1;
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT) {
            patch->reason = "legacy EXT BDA feature chain unsupported"; return 0;
        }
    }
    VkPhysicalDeviceBufferDeviceAddressFeatures supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                      .pNext = &supported};
    features(physical, &query);
    if (!supported.bufferDeviceAddress) { patch->reason = "device lacks BDA"; return 0; }
    uint32_t count = 0;
    if (enumerate(physical, NULL, &count, NULL) != VK_SUCCESS || !count || count > 65536u) {
        patch->reason = "device extension enumeration failed"; return 0;
    }
    VkExtensionProperties *available = (VkExtensionProperties *)malloc((size_t)count * sizeof *available);
    if (!available) { patch->reason = "extension query allocation failed"; return 0; }
    VkResult result = enumerate(physical, NULL, &count, available);
    if (result != VK_SUCCESS) { free(available); patch->reason = "device extensions changed during query"; return 0; }
    for (size_t r = 0; r < sizeof required / sizeof required[0]; ++r) {
        int found = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (!strcmp(required[r], available[i].extensionName)) { found = 1; break; }
        if (!found) { free(available); patch->reason = "required external Win32/BDA extension unavailable"; return 0; }
    }
    int semaphore_extension = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, available[i].extensionName))
            semaphore_extension = 1;
    free(available);
    if (original->enabledExtensionCount > UINT32_MAX - 3u) {
        patch->reason = "app extension count overflows"; return 0;
    }
    patch->extensions = (const char **)malloc(((size_t)original->enabledExtensionCount + 3u) * sizeof *patch->extensions);
    if (!patch->extensions) { patch->reason = "device patch allocation failed"; return 0; }
    count = original->enabledExtensionCount;
    if (count) memcpy(patch->extensions, original->ppEnabledExtensionNames, (size_t)count * sizeof *patch->extensions);
    for (size_t r = 0; r < sizeof required / sizeof required[0]; ++r)
        if (!nr_gpu_extension_present(required[r], count, patch->extensions)) patch->extensions[count++] = required[r];
    if (semaphore_extension && !nr_gpu_extension_present(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                                                        count, patch->extensions))
        patch->extensions[count++] = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
    patch->info.ppEnabledExtensionNames = patch->extensions;
    patch->info.enabledExtensionCount = count;
    if (!app_bda) {
        patch->bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        patch->bda.bufferDeviceAddress = VK_TRUE;
        patch->bda.pNext = (void *)original->pNext;
        patch->info.pNext = &patch->bda;
    }
    patch->enabled = 1;
    patch->reason = NULL;
    return 1;
}

static void nr_gpu_patch_free(struct nr_gpu_device_patch *patch)
{
    free(patch->extensions);
    patch->extensions = NULL;
}

struct nr_gpu_bridge {
    VkDevice device;
    VkPhysicalDevice physical;
    PFN_vkSetDeviceLoaderData set_loader_data; /* private command buffers created inside a layer */
    uint32_t family;
    VkQueue bound_queue;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
    VkBuffer input, output;
    VkDeviceMemory input_memory, output_memory;
    VkDeviceSize input_alloc, output_alloc, bytes;
    uint32_t input_memory_type, output_memory_type;
    HANDLE input_handle, output_handle, ready_handle, done_handle;
    VkSemaphore ready, done;
    VkCommandPool pool;
    VkCommandBuffer capture_command, output_command;
    VkFence capture_fence, output_fence;
    uint32_t width, height;
    VkFormat format;
    uint64_t session;
    VkResult error;
    int initialized, resources, captured, capture_pending, output_pending, fenced;
    int input_external, output_external, waits_consumed;
    const char *reason;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32Handle;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkGetSemaphoreWin32HandleKHR GetSemaphoreWin32Handle;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkQueueSubmit QueueSubmit;
};

/* A configured bridge has no queue until its first successful capture. Later
 * presentations on another queue must release/rebuild it, even if that queue
 * belongs to the same family; its completed capture fence can still be marked
 * pending until the next command begins. This predicate never submits/waits. */
static int nr_gpu_bridge_compatible(const struct nr_gpu_bridge *b, uint32_t family,
                                    VkQueue queue, uint32_t width, uint32_t height, VkFormat format)
{
    return b && queue && b->family == family && b->width == width && b->height == height
        && b->format == format && (!b->bound_queue || b->bound_queue == queue);
}

/* Entry points must come from the layer's next dispatch, never its own loader
 * exports. Family is fixed for this bridge; the caller supplies the corresponding
 * present queue and declines protected queues/swapchains before capture. */
static VkResult nr_gpu_bridge_init(struct nr_gpu_bridge *b, VkDevice device, VkPhysicalDevice physical,
                                  uint32_t family, PFN_vkGetDeviceProcAddr get,
                                  PFN_vkGetPhysicalDeviceProperties2 properties,
                                  PFN_vkGetPhysicalDeviceExternalBufferProperties buffer_properties,
                                  PFN_vkGetPhysicalDeviceExternalSemaphoreProperties semaphore_properties,
                                  PFN_vkGetPhysicalDeviceMemoryProperties memory_properties,
                                  PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties)
{
    memset(b, 0, sizeof *b);
    b->device = device; b->physical = physical; b->family = family;
    if (!device || !physical || !get || !properties || !buffer_properties
        || !memory_properties || !queue_properties) {
        b->reason = "GPU bridge query entry points unavailable";
        return b->error = VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    properties(physical, &props);
    if (props.properties.vendorID != 0x8086u) {
        b->reason = "GPU bridge requires Intel"; return b->error = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    memcpy(b->device_uuid, id.deviceUUID, VK_UUID_SIZE);
    memcpy(b->driver_uuid, id.driverUUID, VK_UUID_SIZE);
    unsigned nonzero_device = 0, nonzero_driver = 0;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) { nonzero_device |= b->device_uuid[i]; nonzero_driver |= b->driver_uuid[i]; }
    if (!nonzero_device || !nonzero_driver) {
        b->reason = "GPU bridge UUID unavailable"; return b->error = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint32_t families = 0;
    queue_properties(physical, &families, NULL);
    if (family >= families || !families || families > 65536u) {
        b->reason = "GPU bridge queue family invalid"; return b->error = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkQueueFamilyProperties *queues = (VkQueueFamilyProperties *)malloc((size_t)families * sizeof *queues);
    if (!queues) return b->error = VK_ERROR_OUT_OF_HOST_MEMORY;
    queue_properties(physical, &families, queues);
    int transfer = family < families && queues[family].queueCount
        && (queues[family].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
    free(queues);
    if (!transfer) { b->reason = "present queue family cannot transfer"; return b->error = VK_ERROR_FEATURE_NOT_PRESENT; }
    VkPhysicalDeviceExternalBufferInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = NR_GPU_BUFFER_USAGE, .handleType = NR_GPU_MEMORY_TYPE};
    VkExternalBufferProperties bp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    buffer_properties(physical, &query, &bp);
    VkExternalMemoryFeatureFlags need = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((bp.externalMemoryProperties.externalMemoryFeatures & need) != need
        || !(bp.externalMemoryProperties.compatibleHandleTypes & NR_GPU_MEMORY_TYPE)) {
        b->reason = "external BDA buffers cannot be exported/imported"; return b->error = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkPhysicalDeviceExternalSemaphoreInfo sq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = NR_GPU_SEMAPHORE_TYPE};
    VkExternalSemaphoreProperties sp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    if (semaphore_properties) semaphore_properties(physical, &sq, &sp);
    VkExternalSemaphoreFeatureFlags sn = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((sp.externalSemaphoreFeatures & sn) != sn || !(sp.compatibleHandleTypes & NR_GPU_SEMAPHORE_TYPE)) {
        b->fenced = 1;
    }
    memory_properties(physical, &b->memory_properties);
    const char *fenced = getenv("NR_GPU_TRANSPORT_FENCE");
    if (fenced && !strcmp(fenced, "1")) b->fenced = 1; /* Reproduce the fallback with valid exports. */
#define NR_GPU_LOAD(member, name) do { b->member = (PFN_vk##name)get(device, "vk" #name); \
    if (!b->member) { b->reason = "GPU bridge device entry point unavailable"; return b->error = VK_ERROR_EXTENSION_NOT_PRESENT; } } while (0)
    NR_GPU_LOAD(CreateBuffer, CreateBuffer); NR_GPU_LOAD(DestroyBuffer, DestroyBuffer);
    NR_GPU_LOAD(GetBufferMemoryRequirements, GetBufferMemoryRequirements);
    NR_GPU_LOAD(AllocateMemory, AllocateMemory); NR_GPU_LOAD(FreeMemory, FreeMemory);
    NR_GPU_LOAD(BindBufferMemory, BindBufferMemory);
    NR_GPU_LOAD(GetMemoryWin32Handle, GetMemoryWin32HandleKHR);
    NR_GPU_LOAD(CreateSemaphore, CreateSemaphore); NR_GPU_LOAD(DestroySemaphore, DestroySemaphore);
    b->GetSemaphoreWin32Handle = (PFN_vkGetSemaphoreWin32HandleKHR)get(device, "vkGetSemaphoreWin32HandleKHR");
    if (!b->GetSemaphoreWin32Handle) b->fenced = 1;
    NR_GPU_LOAD(CreateCommandPool, CreateCommandPool); NR_GPU_LOAD(DestroyCommandPool, DestroyCommandPool);
    NR_GPU_LOAD(AllocateCommandBuffers, AllocateCommandBuffers); NR_GPU_LOAD(ResetCommandBuffer, ResetCommandBuffer);
    NR_GPU_LOAD(BeginCommandBuffer, BeginCommandBuffer); NR_GPU_LOAD(EndCommandBuffer, EndCommandBuffer);
    NR_GPU_LOAD(CmdPipelineBarrier, CmdPipelineBarrier);
    NR_GPU_LOAD(CmdCopyImageToBuffer, CmdCopyImageToBuffer); NR_GPU_LOAD(CmdCopyBufferToImage, CmdCopyBufferToImage);
    NR_GPU_LOAD(CreateFence, CreateFence); NR_GPU_LOAD(DestroyFence, DestroyFence);
    NR_GPU_LOAD(ResetFences, ResetFences); NR_GPU_LOAD(WaitForFences, WaitForFences); NR_GPU_LOAD(QueueSubmit, QueueSubmit);
#undef NR_GPU_LOAD
    b->initialized = 1;
    return VK_SUCCESS;
}

static void nr_gpu_bridge_packet(const struct nr_gpu_bridge *b, uint32_t action, struct nr_gpu_packet *p)
{
    memset(p, 0, sizeof *p);
    p->action = action; p->pid = GetCurrentProcessId(); p->width = b->width; p->height = b->height;
    p->format = (uint32_t)b->format; p->version = b->fenced ? NR_GPU_FENCED_VERSION : NR_GPU_VERSION;
    p->input_memory_type = b->input_memory_type; p->output_memory_type = b->output_memory_type;
    p->session = b->session; p->input_alloc = b->input_alloc; p->output_alloc = b->output_alloc; p->bytes = b->bytes;
    p->input_memory = (uint64_t)(uintptr_t)b->input_handle; p->output_memory = (uint64_t)(uintptr_t)b->output_handle;
    p->ready_sem = (uint64_t)(uintptr_t)b->ready_handle; p->done_sem = (uint64_t)(uintptr_t)b->done_handle;
    memcpy(p->device_uuid, b->device_uuid, VK_UUID_SIZE); memcpy(p->driver_uuid, b->driver_uuid, VK_UUID_SIZE);
}

static VkResult nr_gpu_bridge_wait(struct nr_gpu_bridge *b, VkFence fence, int *pending)
{
    if (!*pending) return VK_SUCCESS;
    VkResult result = b->WaitForFences(b->device, 1, &fence, VK_TRUE, NR_GPU_WAIT_NS);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) *pending = 0;
    if (result != VK_SUCCESS) b->error = result;
    return result;
}

/* RELEASE is sent/acknowledged by the caller before close during normal resize
 * or shutdown. Export handles may be closed after the peer duplicated them;
 * retaining them for the session makes HELLO retries well-defined. A timeout
 * pins resources instead of freeing memory referenced by an unfinished submit. */
static VkResult nr_gpu_bridge_close(struct nr_gpu_bridge *b, struct nr_gpu_packet *release)
{
    if (release) nr_gpu_bridge_packet(b, NR_GPU_RELEASE, release);
    if (!b->initialized) return VK_SUCCESS;
    VkResult first = nr_gpu_bridge_wait(b, b->capture_fence, &b->capture_pending);
    if (first != VK_SUCCESS && first != VK_ERROR_DEVICE_LOST) return first;
    VkResult last = nr_gpu_bridge_wait(b, b->output_fence, &b->output_pending);
    if (last != VK_SUCCESS && last != VK_ERROR_DEVICE_LOST) return last;
    if (b->capture_fence) b->DestroyFence(b->device, b->capture_fence, NULL);
    if (b->output_fence) b->DestroyFence(b->device, b->output_fence, NULL);
    if (b->pool) b->DestroyCommandPool(b->device, b->pool, NULL);
    if (b->ready) b->DestroySemaphore(b->device, b->ready, NULL);
    if (b->done) b->DestroySemaphore(b->device, b->done, NULL);
    if (b->input) b->DestroyBuffer(b->device, b->input, NULL);
    if (b->output) b->DestroyBuffer(b->device, b->output, NULL);
    if (b->input_memory) b->FreeMemory(b->device, b->input_memory, NULL);
    if (b->output_memory) b->FreeMemory(b->device, b->output_memory, NULL);
    if (b->input_handle) CloseHandle(b->input_handle);
    if (b->output_handle) CloseHandle(b->output_handle);
    if (b->ready_handle) CloseHandle(b->ready_handle);
    if (b->done_handle) CloseHandle(b->done_handle);
    b->capture_fence = b->output_fence = VK_NULL_HANDLE;
    b->pool = VK_NULL_HANDLE; b->capture_command = b->output_command = VK_NULL_HANDLE;
    b->input = b->output = VK_NULL_HANDLE; b->input_memory = b->output_memory = VK_NULL_HANDLE;
    b->ready = b->done = VK_NULL_HANDLE;
    b->input_handle = b->output_handle = b->ready_handle = b->done_handle = NULL;
    b->resources = b->captured = b->input_external = b->output_external = b->waits_consumed = 0;
    b->bound_queue = VK_NULL_HANDLE;
    b->session = b->bytes = b->input_alloc = b->output_alloc = 0;
    b->input_memory_type = b->output_memory_type = 0;
    return first == VK_ERROR_DEVICE_LOST || last == VK_ERROR_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult nr_gpu_bridge_export_buffer(struct nr_gpu_bridge *b, VkBuffer *buffer,
                                           VkDeviceMemory *memory, VkDeviceSize *allocation,
                                           uint32_t *memory_type, HANDLE *handle)
{
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = NR_GPU_MEMORY_TYPE};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = b->bytes, .usage = NR_GPU_BUFFER_USAGE, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult r = b->CreateBuffer(b->device, &info, NULL, buffer);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    b->GetBufferMemoryRequirements(b->device, *buffer, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < b->memory_properties.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = b->memory_properties.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            && !(flags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))) {
            type = i; break;
        }
    }
    if (type == UINT32_MAX || req.size < b->bytes) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = *buffer};
    VkExportMemoryAllocateInfo exported = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .handleTypes = NR_GPU_MEMORY_TYPE};
    VkMemoryAllocateFlagsInfo address = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = &exported, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &address,
        .allocationSize = req.size, .memoryTypeIndex = type};
    r = b->AllocateMemory(b->device, &ai, NULL, memory);
    if (r != VK_SUCCESS) return r;
    *allocation = req.size; *memory_type = type;
    r = b->BindBufferMemory(b->device, *buffer, *memory, 0);
    if (r != VK_SUCCESS) return r;
    VkMemoryGetWin32HandleInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = *memory, .handleType = NR_GPU_MEMORY_TYPE};
    r = b->GetMemoryWin32Handle(b->device, &hi, handle);
    if (*handle == INVALID_HANDLE_VALUE) *handle = NULL;
    if (r == VK_SUCCESS && !*handle) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return r;
}

static VkResult nr_gpu_bridge_export_semaphore(struct nr_gpu_bridge *b, VkSemaphore *semaphore, HANDLE *handle)
{
    VkExportSemaphoreCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = NR_GPU_SEMAPHORE_TYPE};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &external};
    VkResult r = b->CreateSemaphore(b->device, &info, NULL, semaphore);
    if (r != VK_SUCCESS) return r;
    VkSemaphoreGetWin32HandleInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
        .semaphore = *semaphore, .handleType = NR_GPU_SEMAPHORE_TYPE};
    r = b->GetSemaphoreWin32Handle(b->device, &hi, handle);
    if (*handle == INVALID_HANDLE_VALUE) *handle = NULL;
    if (r == VK_SUCCESS && !*handle) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return r;
}

/* Configure a fresh bridge, or an idle/closed bridge after the peer's RELEASE.
 * width/height are the swapchain extent; session must change on every rebuild. */
static VkResult nr_gpu_bridge_configure(struct nr_gpu_bridge *b, uint32_t width, uint32_t height,
                                       VkFormat format, uint64_t session, struct nr_gpu_packet *hello)
{
    if (!b->initialized || b->resources || b->capture_pending || b->output_pending || b->input || b->output)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!width || !height || !session || (uint64_t)width * height > UINT64_MAX / 4u
        || (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB
            && format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)) {
        b->reason = "GPU bridge supports nonempty SDR RGBA8/BGRA8 only"; return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    b->width = width; b->height = height; b->format = format; b->session = session;
    b->bytes = (VkDeviceSize)width * height * 4u;
    VkResult r = nr_gpu_bridge_export_buffer(b, &b->input, &b->input_memory, &b->input_alloc,
                                              &b->input_memory_type, &b->input_handle);
    if (r == VK_SUCCESS) r = nr_gpu_bridge_export_buffer(b, &b->output, &b->output_memory, &b->output_alloc,
                                                        &b->output_memory_type, &b->output_handle);
    if (r == VK_SUCCESS && !b->fenced) {
        r = nr_gpu_bridge_export_semaphore(b, &b->ready, &b->ready_handle);
        if (r == VK_SUCCESS) r = nr_gpu_bridge_export_semaphore(b, &b->done, &b->done_handle);
        if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE || r == VK_ERROR_FEATURE_NOT_PRESENT
            || r == VK_ERROR_EXTENSION_NOT_PRESENT) {
            /* Export was observed returning SUCCESS+NULL despite advertised
             * support. Do not submit or import a missing external payload.
             * Private fences + control replies order external ownership instead. */
            if (b->ready_handle && b->ready_handle != INVALID_HANDLE_VALUE) CloseHandle(b->ready_handle);
            if (b->done_handle && b->done_handle != INVALID_HANDLE_VALUE) CloseHandle(b->done_handle);
            if (b->ready) b->DestroySemaphore(b->device, b->ready, NULL);
            if (b->done) b->DestroySemaphore(b->device, b->done, NULL);
            b->ready = b->done = VK_NULL_HANDLE;
            b->ready_handle = b->done_handle = NULL;
            b->fenced = 1;
            r = VK_SUCCESS;
        }
    }
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = b->family};
    if (r == VK_SUCCESS) r = b->CreateCommandPool(b->device, &pi, NULL, &b->pool);
    VkCommandBuffer commands[2];
    VkCommandBufferAllocateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = b->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    if (r == VK_SUCCESS) {
        r = b->AllocateCommandBuffers(b->device, &ci, commands);
        if (r == VK_SUCCESS) { b->capture_command = commands[0]; b->output_command = commands[1]; }
        if (r == VK_SUCCESS && b->set_loader_data)
            for (unsigned i = 0; i < 2 && r == VK_SUCCESS; ++i)
                r = b->set_loader_data(b->device, commands[i]);
    }
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (r == VK_SUCCESS) r = b->CreateFence(b->device, &fi, NULL, &b->capture_fence);
    if (r == VK_SUCCESS) r = b->CreateFence(b->device, &fi, NULL, &b->output_fence);
    if (r != VK_SUCCESS) {
        b->error = r; b->reason = "GPU bridge export allocation failed";
        nr_gpu_bridge_close(b, NULL);
        return r;
    }
    b->resources = 1; b->error = VK_SUCCESS;
    if (hello) nr_gpu_bridge_packet(b, NR_GPU_HELLO, hello);
    return VK_SUCCESS;
}

static VkBufferMemoryBarrier nr_gpu_bridge_buffer_barrier(VkBuffer buffer, VkDeviceSize bytes,
                                                         uint32_t from, uint32_t to,
                                                         VkAccessFlags source, VkAccessFlags dest)
{
    VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = source, .dstAccessMask = dest, .srcQueueFamilyIndex = from,
        .dstQueueFamilyIndex = to, .buffer = buffer, .offset = 0, .size = bytes};
    return barrier;
}

static VkImageMemoryBarrier nr_gpu_bridge_image_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                                       VkAccessFlags source, VkAccessFlags dest)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source, .dstAccessMask = dest, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return barrier;
}

static VkResult nr_gpu_bridge_begin(struct nr_gpu_bridge *b, VkCommandBuffer command, VkFence fence, int *pending)
{
    VkResult r = nr_gpu_bridge_wait(b, fence, pending);
    if (r != VK_SUCCESS) return r;
    r = b->ResetCommandBuffer(command, 0);
    if (r != VK_SUCCESS) return r;
    r = b->ResetFences(b->device, 1, &fence);
    if (r != VK_SUCCESS) return r;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    return b->BeginCommandBuffer(command, &bi);
}

/* Signal ready only after pixels and external ownership are published. Waits are
 * consumed exactly once on successful submit, even if the later daemon fails.
 * The caller must present with waits removed whenever b->waits_consumed is 1.
 * image belongs to this family (or a concurrent swapchain) and is PRESENT_SRC. */
static VkResult nr_gpu_bridge_capture(struct nr_gpu_bridge *b, VkImage image, VkQueue queue,
                                     uint32_t wait_count, const VkSemaphore *waits)
{
    b->waits_consumed = 0;
    if (!b->resources || b->captured || b->output_pending || b->error != VK_SUCCESS
        || !image || !queue || (b->bound_queue && b->bound_queue != queue)
        || (wait_count && !waits)) return VK_ERROR_INITIALIZATION_FAILED;
    if ((uint64_t)wait_count * sizeof(VkPipelineStageFlags) > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkPipelineStageFlags *stages = wait_count ? (VkPipelineStageFlags *)malloc((size_t)wait_count * sizeof *stages) : NULL;
    if (wait_count && !stages) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < wait_count; ++i) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkResult r = nr_gpu_bridge_begin(b, b->capture_command, b->capture_fence, &b->capture_pending);
    if (r != VK_SUCCESS) { free(stages); b->error = r; return r; }
    if (b->input_external) {
        VkBufferMemoryBarrier acquire = nr_gpu_bridge_buffer_barrier(b->input, b->bytes,
            VK_QUEUE_FAMILY_EXTERNAL, b->family, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        b->CmdPipelineBarrier(b->capture_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &acquire, 0, NULL);
    }
    VkImageMemoryBarrier before = nr_gpu_bridge_image_barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    b->CmdPipelineBarrier(b->capture_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &before);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {b->width, b->height, 1}};
    b->CmdCopyImageToBuffer(b->capture_command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->input, 1, &region);
    VkBufferMemoryBarrier release[2];
    release[0] = nr_gpu_bridge_buffer_barrier(b->input, b->bytes, b->family,
        VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    uint32_t release_count = 1;
    if (!b->output_external) release[release_count++] = nr_gpu_bridge_buffer_barrier(b->output, b->bytes,
        b->family, VK_QUEUE_FAMILY_EXTERNAL, 0, 0);
    b->CmdPipelineBarrier(b->capture_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, release_count, release, 0, NULL);
    VkImageMemoryBarrier after = nr_gpu_bridge_image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    b->CmdPipelineBarrier(b->capture_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &after);
    r = b->EndCommandBuffer(b->capture_command);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait_count,
        .pWaitSemaphores = waits, .pWaitDstStageMask = stages, .commandBufferCount = 1,
        .pCommandBuffers = &b->capture_command, .signalSemaphoreCount = b->fenced ? 0 : 1,
        .pSignalSemaphores = b->fenced ? NULL : &b->ready};
    if (r == VK_SUCCESS) r = b->QueueSubmit(queue, 1, &si, b->capture_fence);
    free(stages);
    if (r == VK_SUCCESS) {
        b->capture_pending = b->captured = b->input_external = b->output_external = 1;
        b->bound_queue = queue;
        b->waits_consumed = 1;
        /* Protocol 2 cannot send FRAME until this release has finished. CPU
         * participates only in synchronization; no pixel data is mapped. */
        if (b->fenced) r = nr_gpu_bridge_wait(b, b->capture_fence, &b->capture_pending);
    } else b->error = r;
    return r;
}

/* Call only after a matching successful FRAME reply. The daemon must send that
 * reply after its done signal submit/fence completed, so a failure cannot strand
 * the game's queue in an unfulfilled binary semaphore wait. */
static VkResult nr_gpu_bridge_copy_output(struct nr_gpu_bridge *b, VkImage image, VkQueue queue)
{
    if (!b->resources || !b->captured || b->error != VK_SUCCESS || !image || !queue
        || b->bound_queue != queue)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = nr_gpu_bridge_begin(b, b->output_command, b->output_fence, &b->output_pending);
    if (r != VK_SUCCESS) { b->error = r; return r; }
    VkBufferMemoryBarrier acquire = nr_gpu_bridge_buffer_barrier(b->output, b->bytes,
        VK_QUEUE_FAMILY_EXTERNAL, b->family, 0, VK_ACCESS_TRANSFER_READ_BIT);
    b->CmdPipelineBarrier(b->output_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &acquire, 0, NULL);
    VkImageMemoryBarrier before = nr_gpu_bridge_image_barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    b->CmdPipelineBarrier(b->output_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &before);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {b->width, b->height, 1}};
    b->CmdCopyBufferToImage(b->output_command, b->output, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkBufferMemoryBarrier release = nr_gpu_bridge_buffer_barrier(b->output, b->bytes, b->family,
        VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
    b->CmdPipelineBarrier(b->output_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &release, 0, NULL);
    VkImageMemoryBarrier after = nr_gpu_bridge_image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    b->CmdPipelineBarrier(b->output_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &after);
    r = b->EndCommandBuffer(b->output_command);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = b->fenced ? 0 : 1,
        .pWaitSemaphores = b->fenced ? NULL : &b->done, .pWaitDstStageMask = b->fenced ? NULL : &stage, .commandBufferCount = 1,
        .pCommandBuffers = &b->output_command};
    if (r == VK_SUCCESS) r = b->QueueSubmit(queue, 1, &si, b->output_fence);
    if (r != VK_SUCCESS) { b->error = r; return r; }
    b->output_pending = 1;
    r = nr_gpu_bridge_wait(b, b->output_fence, &b->output_pending);
    if (r == VK_SUCCESS) b->captured = 0;
    return r;
}
#endif /* _WIN32 */
#endif /* NR_GPU_BRIDGE_H */
