/* Mocked Win32 bridge: no loader/device/GPU required. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <vulkan/vulkan.h>
static BOOL test_close_handle(HANDLE handle);
#define CloseHandle test_close_handle
#include "nr_gpu_bridge.h"
#undef CloseHandle

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define H(type, value) ((type)(uintptr_t)(value))
static uintptr_t serial = 100;
static uint32_t vendor = 0x8086, queue_flags = VK_QUEUE_TRANSFER_BIT;
static int supports_bda = 1, exportable = 1;
static int sem_exportable = 1, device_uuid_valid = 1, driver_uuid_valid = 1;
static unsigned sem_null_export, sem_export_attempts;
static int memory_null_export;
static int bad_buffer_handle_type, bad_sem_handle_type;
static const char *missing_proc;
static unsigned creates, destroys, allocations, frees, submits, captures, outputs;
static unsigned semaphore_creates, semaphore_destroys, pool_creates, pool_destroys, fence_creates, fence_destroys;
static unsigned buffer_acquires, buffer_releases, initial_output_releases, image_transitions;
static VkResult submit_result = VK_SUCCESS, wait_result = VK_SUCCESS;
static unsigned fail_resource_call, resource_calls, exported_handle_count;
static HANDLE exported_handles[128];
static unsigned char exported_handle_closed[128];
static VkDeviceSize current_bytes;
static VkBuffer current_dedicated;
static uint32_t seen_wait_count, seen_signal_count;
static VkPipelineStageFlags seen_wait_stage;
static VkSemaphore seen_wait, seen_signal;
static unsigned loader_callbacks, failing_loader_callback;
static VkCommandBuffer allocated_commands[2], initialized_commands[2];

static int resource_failure(void)
{
    return fail_resource_call && ++resource_calls == fail_resource_call;
}
static void save_handle(HANDLE handle)
{
    CHECK(exported_handle_count < sizeof exported_handles / sizeof exported_handles[0]);
    exported_handles[exported_handle_count] = handle;
    exported_handle_closed[exported_handle_count++] = 0;
}
static BOOL test_close_handle(HANDLE handle)
{
    for (unsigned i = 0; i < exported_handle_count; ++i) {
        if (exported_handles[i] == handle && !exported_handle_closed[i]) {
            exported_handle_closed[i] = 1;
            CHECK(CloseHandle(handle));
            return TRUE;
        }
    }
    CHECK(0 && "attempt to close unknown or already closed export handle");
    return FALSE;
}
static void check_closed_handles(void)
{
    for (unsigned i = 0; i < exported_handle_count; ++i) CHECK(exported_handle_closed[i]);
    exported_handle_count = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_enum(VkPhysicalDevice p, const char *layer,
                                            uint32_t *count, VkExtensionProperties *props)
{
    (void)p; CHECK(!layer);
    static const char *names[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    if (!props) { *count = 3; return VK_SUCCESS; }
    CHECK(*count >= 3);
    for (int i = 0; i < 3; ++i) { memset(&props[i], 0, sizeof props[i]); strcpy(props[i].extensionName, names[i]); }
    *count = 3; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_props(VkPhysicalDevice p, VkPhysicalDeviceProperties *out)
{
    (void)p; memset(out, 0, sizeof *out); out->vendorID = vendor; out->apiVersion = VK_API_VERSION_1_3;
}
static VKAPI_ATTR void VKAPI_CALL m_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *out)
{
    (void)p; VkPhysicalDeviceBufferDeviceAddressFeatures *bda = out->pNext;
    CHECK(bda->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    bda->bufferDeviceAddress = supports_bda;
}
static VKAPI_ATTR void VKAPI_CALL m_props2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out)
{
    m_props(p, &out->properties); VkPhysicalDeviceIDProperties *id = out->pNext;
    memset(id->deviceUUID, device_uuid_valid ? 12 : 0, VK_UUID_SIZE);
    memset(id->driverUUID, driver_uuid_valid ? 34 : 0, VK_UUID_SIZE);
}
static VKAPI_ATTR void VKAPI_CALL m_buffer_props(VkPhysicalDevice p, const VkPhysicalDeviceExternalBufferInfo *info,
                                                VkExternalBufferProperties *out)
{
    (void)p; CHECK(info->usage == NR_GPU_BUFFER_USAGE); CHECK(info->handleType == NR_GPU_MEMORY_TYPE);
    out->externalMemoryProperties.compatibleHandleTypes = bad_buffer_handle_type ? 0 : NR_GPU_MEMORY_TYPE;
    out->externalMemoryProperties.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
        (exportable ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : 0);
}
static VKAPI_ATTR void VKAPI_CALL m_sem_props(VkPhysicalDevice p, const VkPhysicalDeviceExternalSemaphoreInfo *info,
                                             VkExternalSemaphoreProperties *out)
{
    (void)p; CHECK(info->handleType == NR_GPU_SEMAPHORE_TYPE);
    out->compatibleHandleTypes = bad_sem_handle_type ? 0 : NR_GPU_SEMAPHORE_TYPE;
    out->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT
        | (sem_exportable ? VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT : 0);
}
static VKAPI_ATTR void VKAPI_CALL m_mem_props(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *out)
{
    (void)p; memset(out, 0, sizeof *out); out->memoryTypeCount = 2;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VKAPI_ATTR void VKAPI_CALL m_queues(VkPhysicalDevice p, uint32_t *count, VkQueueFamilyProperties *out)
{
    (void)p; *count = 1; if (out) { memset(out, 0, sizeof *out); out->queueCount = 1; out->queueFlags = queue_flags; }
}
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateBuffer(VkDevice d, const VkBufferCreateInfo *info,
                                                    const VkAllocationCallbacks *a, VkBuffer *out)
{
    (void)d; (void)a; CHECK(info->usage == NR_GPU_BUFFER_USAGE);
    CHECK(info->sharingMode == VK_SHARING_MODE_EXCLUSIVE);
    const VkExternalMemoryBufferCreateInfo *e = info->pNext;
    CHECK(e && e->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO && e->handleTypes == NR_GPU_MEMORY_TYPE);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    current_bytes = info->size; current_dedicated = *out = H(VkBuffer, ++serial); ++creates; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{ (void)d; (void)b; (void)a; ++destroys; }
static VKAPI_ATTR void VKAPI_CALL m_GetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out)
{ (void)d; (void)b; memset(out, 0, sizeof *out); out->size = current_bytes + 256; out->alignment = 256; out->memoryTypeBits = 2; }
static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *info,
                                                      const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
    (void)d; (void)a; CHECK(info->allocationSize == current_bytes + 256 && info->memoryTypeIndex == 1);
    const VkMemoryAllocateFlagsInfo *address = info->pNext;
    CHECK(address && address->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
    CHECK(address->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    const VkExportMemoryAllocateInfo *e = address->pNext;
    CHECK(e && e->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO && e->handleTypes == NR_GPU_MEMORY_TYPE);
    const VkMemoryDedicatedAllocateInfo *dedicated = e->pNext;
    CHECK(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    CHECK(dedicated->buffer == current_dedicated && !dedicated->image);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = H(VkDeviceMemory, ++serial); ++allocations; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a)
{ (void)d; (void)m; (void)a; ++frees; }
static VKAPI_ATTR VkResult VKAPI_CALL m_BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset)
{ (void)d; (void)b; (void)m; CHECK(offset == 0); return resource_failure() ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_GetMemoryWin32HandleKHR(VkDevice d, const VkMemoryGetWin32HandleInfoKHR *i, HANDLE *out)
{
    (void)d; CHECK(i->handleType == NR_GPU_MEMORY_TYPE);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (memory_null_export) { *out = NULL; return VK_SUCCESS; }
    *out = CreateEventW(NULL, FALSE, FALSE, NULL); CHECK(*out); save_handle(*out); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *info,
                                                       const VkAllocationCallbacks *a, VkSemaphore *out)
{
    (void)d; (void)a; const VkExportSemaphoreCreateInfo *e = info->pNext;
    CHECK(e && e->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO && e->handleTypes == NR_GPU_SEMAPHORE_TYPE);
    CHECK(!e->pNext);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = H(VkSemaphore, ++serial); ++semaphore_creates; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_DestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{ (void)d; (void)s; (void)a; ++semaphore_destroys; }
static VKAPI_ATTR VkResult VKAPI_CALL m_GetSemaphoreWin32HandleKHR(VkDevice d, const VkSemaphoreGetWin32HandleInfoKHR *i, HANDLE *out)
{
    (void)d; CHECK(i->handleType == NR_GPU_SEMAPHORE_TYPE);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (sem_null_export && ++sem_export_attempts == sem_null_export) { *out = NULL; return VK_SUCCESS; }
    *out = CreateEventW(NULL, FALSE, FALSE, NULL); CHECK(*out); save_handle(*out); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *i,
                                                         const VkAllocationCallbacks *a, VkCommandPool *out)
{
    (void)d; (void)a; CHECK(i->queueFamilyIndex == 0 && i->flags == VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = H(VkCommandPool, ++serial); ++pool_creates; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_DestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{ (void)d; (void)p; (void)a; ++pool_destroys; }
static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *out)
{
    (void)d; CHECK(i->commandBufferCount == 2);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    out[0] = allocated_commands[0] = H(VkCommandBuffer, ++serial);
    out[1] = allocated_commands[1] = H(VkCommandBuffer, ++serial);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_set_loader_data(VkDevice device, void *object)
{
    CHECK(device == H(VkDevice, 2) && loader_callbacks < 2);
    CHECK(object == allocated_commands[loader_callbacks]);
    initialized_commands[loader_callbacks++] = (VkCommandBuffer)object;
    return loader_callbacks == failing_loader_callback ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_ResetCommandBuffer(VkCommandBuffer b, VkCommandBufferResetFlags f)
{ (void)b; CHECK(f == 0); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_BeginCommandBuffer(VkCommandBuffer b, const VkCommandBufferBeginInfo *i)
{ (void)b; CHECK(i->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_EndCommandBuffer(VkCommandBuffer b)
{ (void)b; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL m_CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkDependencyFlags dependency, uint32_t nm, const VkMemoryBarrier *m,
    uint32_t nb, const VkBufferMemoryBarrier *buffers, uint32_t ni, const VkImageMemoryBarrier *images)
{
    (void)cmd; (void)dependency; CHECK(!nm && !m);
    for (uint32_t i = 0; i < nb; ++i) {
        CHECK(buffers[i].size == 1280u * 720u * 4u);
        if (buffers[i].srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL) {
            CHECK(buffers[i].dstQueueFamilyIndex == 0 && src == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
            ++buffer_acquires;
        } else {
            CHECK(buffers[i].srcQueueFamilyIndex == 0 && buffers[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
            CHECK(src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (!buffers[i].srcAccessMask) ++initial_output_releases;
            ++buffer_releases;
        }
    }
    for (uint32_t i = 0; i < ni; ++i) {
        CHECK(images[i].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && images[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
        CHECK(images[i].subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && images[i].subresourceRange.layerCount == 1);
        ++image_transitions;
    }
}
static VKAPI_ATTR void VKAPI_CALL m_CmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l,
                                                        VkBuffer b, uint32_t n, const VkBufferImageCopy *regions)
{ (void)c; (void)i; (void)b; CHECK(l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && n == 1); CHECK(regions->imageExtent.width == 1280 && regions->imageExtent.height == 720); CHECK(!regions->bufferRowLength); ++captures; }
static VKAPI_ATTR void VKAPI_CALL m_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l,
                                                        uint32_t n, const VkBufferImageCopy *regions)
{ (void)c; (void)i; (void)b; CHECK(l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n == 1); CHECK(regions->imageExtent.width == 1280 && regions->imageExtent.height == 720); ++outputs; }
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateFence(VkDevice d, const VkFenceCreateInfo *i,
                                                   const VkAllocationCallbacks *a, VkFence *out)
{
    (void)d; (void)a; CHECK(i->flags == 0);
    if (resource_failure()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = H(VkFence, ++serial); ++fence_creates; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL m_DestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{ (void)d; (void)f; (void)a; ++fence_destroys; }
static VKAPI_ATTR VkResult VKAPI_CALL m_ResetFences(VkDevice d, uint32_t n, const VkFence *f)
{ (void)d; (void)f; CHECK(n == 1); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{ (void)d; (void)f; CHECK(n == 1 && all && timeout == NR_GPU_WAIT_NS); return wait_result; }
static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence f)
{
    (void)q; CHECK(n == 1 && f && info->commandBufferCount == 1);
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
        CHECK(info->pWaitDstStageMask[i] == VK_PIPELINE_STAGE_TRANSFER_BIT
              || info->pWaitDstStageMask[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    seen_wait_count = info->waitSemaphoreCount; seen_signal_count = info->signalSemaphoreCount;
    seen_wait = seen_wait_count ? info->pWaitSemaphores[0] : VK_NULL_HANDLE;
    seen_wait_stage = seen_wait_count ? info->pWaitDstStageMask[0] : 0;
    seen_signal = seen_signal_count ? info->pSignalSemaphores[0] : VK_NULL_HANDLE;
    ++submits; return submit_result;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL m_get(VkDevice d, const char *name)
{
    (void)d;
    if (missing_proc && !strcmp(name, missing_proc)) return NULL;
#define PROC(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)m_##n
    PROC(CreateBuffer); PROC(DestroyBuffer); PROC(GetBufferMemoryRequirements); PROC(AllocateMemory);
    PROC(FreeMemory); PROC(BindBufferMemory); PROC(GetMemoryWin32HandleKHR);
    PROC(CreateSemaphore); PROC(DestroySemaphore); PROC(GetSemaphoreWin32HandleKHR);
    PROC(CreateCommandPool); PROC(DestroyCommandPool); PROC(AllocateCommandBuffers);
    PROC(ResetCommandBuffer); PROC(BeginCommandBuffer); PROC(EndCommandBuffer);
    PROC(CmdPipelineBarrier); PROC(CmdCopyImageToBuffer); PROC(CmdCopyBufferToImage);
    PROC(CreateFence); PROC(DestroyFence); PROC(ResetFences); PROC(WaitForFences); PROC(QueueSubmit);
#undef PROC
    fprintf(stderr, "Unexpected device function %s\n", name); return NULL;
}

static void patch_checks(void)
{
    struct nr_gpu_device_patch patch;
    const char *app_names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo app = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = app_names};
    VkDeviceCreateInfo before = app;
    _putenv_s("NR_GPU_TRANSPORT", "0");
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(patch.reason && !memcmp(&app, &before, sizeof app));
    _putenv_s("NR_GPU_TRANSPORT", "1");
    CHECK(nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(!memcmp(&app, &before, sizeof app));
    CHECK(patch.info.enabledExtensionCount == 4 && patch.info.pNext == &patch.bda && patch.bda.bufferDeviceAddress);
    CHECK(patch.info.ppEnabledExtensionNames[0] == app_names[0]);
    nr_gpu_patch_free(&patch);
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    app.pNext = &v12;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "disable") && !v12.bufferDeviceAddress);
    v12.bufferDeviceAddress = VK_TRUE;
    CHECK(nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(patch.info.pNext == &v12); nr_gpu_patch_free(&patch);
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    app.pNext = &bda;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "disabled"));
    supports_bda = 0; app.pNext = NULL;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    supports_bda = 1; vendor = 0x10de;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    vendor = 0x8086;
    const char *already[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    app.enabledExtensionCount = 4; app.ppEnabledExtensionNames = already;
    CHECK(nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(patch.info.enabledExtensionCount == 4); nr_gpu_patch_free(&patch);
    const char *legacy[] = {VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    app.enabledExtensionCount = 1; app.ppEnabledExtensionNames = legacy;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "legacy EXT BDA extension") && !patch.extensions);
    CHECK(app.ppEnabledExtensionNames == legacy && !app.pNext);
    app.enabledExtensionCount = 1; app.ppEnabledExtensionNames = NULL;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "invalid app extension"));
    app.enabledExtensionCount = 0;
    VkPhysicalDeviceTimelineSemaphoreFeatures tail = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceTimelineSemaphoreFeatures tail_before = tail;
    app.pNext = &tail;
    CHECK(nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(patch.bda.pNext == &tail && !memcmp(&tail, &tail_before, sizeof tail));
    nr_gpu_patch_free(&patch);
    VkBaseOutStructure cycle = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    cycle.pNext = &cycle; app.pNext = &cycle;
    CHECK(!nr_gpu_patch_device(&patch, H(VkPhysicalDevice, 1), &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "too long") && cycle.pNext == &cycle);
    VkPhysicalDevice physicals[] = {H(VkPhysicalDevice, 1), H(VkPhysicalDevice, 2)};
    VkDeviceGroupDeviceCreateInfo group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
        .physicalDeviceCount = 2, .pPhysicalDevices = physicals};
    app.pNext = &group;
    CHECK(!nr_gpu_patch_device(&patch, physicals[0], &app, m_enum, m_features, m_props));
    CHECK(strstr(patch.reason, "single matching") && group.physicalDeviceCount == 2);
    group.physicalDeviceCount = 1; group.pPhysicalDevices = &physicals[1];
    CHECK(!nr_gpu_patch_device(&patch, physicals[0], &app, m_enum, m_features, m_props));
    group.pPhysicalDevices = physicals;
    CHECK(nr_gpu_patch_device(&patch, physicals[0], &app, m_enum, m_features, m_props));
    CHECK(patch.bda.pNext == &group && group.pPhysicalDevices == physicals);
    nr_gpu_patch_free(&patch);
}

static VkResult init(struct nr_gpu_bridge *b)
{
    return nr_gpu_bridge_init(b, H(VkDevice, 2), H(VkPhysicalDevice, 1), 0, m_get,
        m_props2, m_buffer_props, m_sem_props, m_mem_props, m_queues);
}

static void capability_checks(void)
{
    struct nr_gpu_bridge b;
    device_uuid_valid = 0; CHECK(init(&b) == VK_ERROR_FEATURE_NOT_PRESENT); device_uuid_valid = 1;
    driver_uuid_valid = 0; CHECK(init(&b) == VK_ERROR_FEATURE_NOT_PRESENT); driver_uuid_valid = 1;
    bad_buffer_handle_type = 1; CHECK(init(&b) == VK_ERROR_FEATURE_NOT_PRESENT); bad_buffer_handle_type = 0;
    bad_sem_handle_type = 1; CHECK(init(&b) == VK_SUCCESS && b.fenced); bad_sem_handle_type = 0;
    sem_exportable = 0; CHECK(init(&b) == VK_SUCCESS && b.fenced); sem_exportable = 1;
    missing_proc = "vkGetSemaphoreWin32HandleKHR";
    CHECK(init(&b) == VK_SUCCESS && b.fenced);
    missing_proc = "vkGetMemoryWin32HandleKHR";
    CHECK(init(&b) == VK_ERROR_EXTENSION_NOT_PRESENT && !b.initialized);
    missing_proc = NULL;
    CHECK(nr_gpu_bridge_init(&b, H(VkDevice, 2), H(VkPhysicalDevice, 1), 1, m_get,
        m_props2, m_buffer_props, m_sem_props, m_mem_props, m_queues) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(init(&b) == VK_SUCCESS);
    unsigned before_creates = creates;
    CHECK(nr_gpu_bridge_configure(&b, 0, 720, VK_FORMAT_B8G8R8A8_UNORM, 1, NULL) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 0, VK_FORMAT_B8G8R8A8_UNORM, 1, NULL) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 0, NULL) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(nr_gpu_bridge_configure(&b, UINT32_MAX, UINT32_MAX, VK_FORMAT_B8G8R8A8_UNORM, 1, NULL) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(creates == before_creates);
}

static void partial_failure_checks(void)
{
    /* Every allocation/export step fails once, including the second buffer,
     * second semaphore and second fence. Successful earlier objects must all
     * be destroyed; every actual Win32 export handle must be closed. */
    for (unsigned step = 1; step <= 16; ++step) {
        struct nr_gpu_bridge b;
        CHECK(init(&b) == VK_SUCCESS);
        resource_calls = 0; fail_resource_call = step;
        CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM, 50 + step, NULL)
              == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(resource_calls == step && !b.resources && !b.input && !b.output);
        CHECK(!b.pool && !b.capture_fence && !b.output_fence && !b.session);
        CHECK(creates == destroys && allocations == frees && semaphore_creates == semaphore_destroys);
        CHECK(pool_creates == pool_destroys && fence_creates == fence_destroys);
        check_closed_handles();
        CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    }
    fail_resource_call = 0;
}

/* The copy back that signals its image's semaphore (2026-11-26): it does not wait, its fence stays
 * pending, and the next capture waits for that fence before it records or submits anything. */
static void signalled_output_checks(void)
{
    struct nr_gpu_bridge b;
    CHECK(init(&b) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 81, NULL) == VK_SUCCESS);
    VkSemaphore copied = H(VkSemaphore, 77);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    wait_result = VK_TIMEOUT;   /* a wait in the copy back would fail it */
    CHECK(nr_gpu_bridge_copy_output_signal(&b, H(VkImage, 3), H(VkQueue, 5), copied) == VK_SUCCESS);
    CHECK(seen_signal_count == 1 && seen_signal == copied && seen_wait_count == 1 && seen_wait == b.done);
    CHECK(b.output_pending && !b.captured && b.error == VK_SUCCESS);
    wait_result = VK_SUCCESS;
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    CHECK(!b.output_pending && b.captured && b.capture_pending && b.waits_consumed);
    /* the copy's fence failing at the next capture fails that capture before it submits */
    CHECK(nr_gpu_bridge_copy_output_signal(&b, H(VkImage, 3), H(VkQueue, 5), copied) == VK_SUCCESS);
    unsigned before_submits = submits;
    wait_result = VK_TIMEOUT;
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_TIMEOUT);
    CHECK(submits == before_submits && b.output_pending && b.error == VK_TIMEOUT && !b.waits_consumed);
    wait_result = VK_SUCCESS;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS && !b.output_pending && !b.capture_pending);
    check_closed_handles();
}

static void output_timeout_checks(void)
{
    struct nr_gpu_bridge b;
    CHECK(init(&b) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_SRGB, 80, NULL) == VK_SUCCESS);
    unsigned before_submits = submits;
    CHECK(nr_gpu_bridge_capture(&b, VK_NULL_HANDLE, H(VkQueue, 5), 0, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 1, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(submits == before_submits && !b.waits_consumed);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    CHECK(b.capture_pending && b.captured);
    wait_result = VK_TIMEOUT;
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_TIMEOUT);
    CHECK(b.output_pending && b.captured && b.capture_pending && b.error == VK_TIMEOUT);
    HANDLE pinned_output = b.output_handle;
    unsigned before_frees = frees;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_TIMEOUT && frees == before_frees && b.output_handle == pinned_output);
    wait_result = VK_SUCCESS;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS && !b.output_pending && !b.capture_pending);
    check_closed_handles();
    CHECK(init(&b) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_R8G8B8A8_SRGB, 81, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    wait_result = VK_ERROR_DEVICE_LOST;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_ERROR_DEVICE_LOST);
    CHECK(!b.input && !b.resources && !b.capture_pending);
    check_closed_handles(); wait_result = VK_SUCCESS;
}

static void fenced_fallback_checks(void)
{
    for (unsigned missing = 1; missing <= 2; ++missing) {
        struct nr_gpu_bridge b;
        struct nr_gpu_packet hello;
        CHECK(init(&b) == VK_SUCCESS && !b.fenced);
        sem_null_export = missing; sem_export_attempts = 0;
        CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 90 + missing, &hello) == VK_SUCCESS);
        CHECK(b.fenced && hello.version == NR_GPU_FENCED_VERSION && !hello.ready_sem && !hello.done_sem);
        CHECK(!b.ready && !b.done);
        VkSemaphore app_wait = H(VkSemaphore, 4);
        CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 1, &app_wait) == VK_SUCCESS);
        CHECK(b.waits_consumed && b.captured && !b.capture_pending);
        CHECK(seen_wait_count == 1 && seen_wait == app_wait && !seen_signal_count);
        CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
        CHECK(!seen_wait_count && !seen_signal_count && !b.output_pending && !b.captured);
        CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
        check_closed_handles();
    }
    sem_null_export = sem_export_attempts = 0;
    struct nr_gpu_bridge b;
    CHECK(init(&b) == VK_SUCCESS);
    memory_null_export = 1;
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 95, NULL) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    CHECK(!b.resources && !b.input && !b.input_memory && !b.input_handle);
    memory_null_export = 0;
    check_closed_handles();
    sem_exportable = 0; CHECK(init(&b) == VK_SUCCESS && b.fenced); sem_exportable = 1;
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 96, NULL) == VK_SUCCESS);
    wait_result = VK_TIMEOUT;
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_TIMEOUT);
    CHECK(b.capture_pending && b.captured && b.waits_consumed && b.error == VK_TIMEOUT);
    unsigned before_frees = frees;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_TIMEOUT && frees == before_frees);
    wait_result = VK_SUCCESS;
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
}

static void loader_callback_checks(void)
{
    struct nr_gpu_bridge b;
    CHECK(init(&b) == VK_SUCCESS);
    b.set_loader_data = m_set_loader_data;
    loader_callbacks = failing_loader_callback = 0;
    memset(initialized_commands, 0, sizeof initialized_commands);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 100, NULL) == VK_SUCCESS);
    CHECK(loader_callbacks == 2 && initialized_commands[0] == b.capture_command
          && initialized_commands[1] == b.output_command);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
    CHECK(loader_callbacks == 2); /* Replay retains initialized dispatch data. */
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
    for (unsigned failed = 1; failed <= 2; ++failed) {
        CHECK(init(&b) == VK_SUCCESS);
        b.set_loader_data = m_set_loader_data;
        loader_callbacks = 0; failing_loader_callback = failed;
        unsigned before_submits = submits, before_fences = fence_creates;
        CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 100 + failed, NULL)
              == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(loader_callbacks == failed && submits == before_submits && fence_creates == before_fences);
        CHECK(!b.resources && !b.pool && !b.capture_command && !b.output_command);
        CHECK(!b.input && !b.output && !b.input_memory && !b.output_memory);
        CHECK(!b.capture_pending && !b.output_pending && !b.waits_consumed);
        CHECK(creates == destroys && allocations == frees && semaphore_creates == semaphore_destroys
              && pool_creates == pool_destroys && fence_creates == fence_destroys);
        check_closed_handles();
        CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    }
    failing_loader_callback = 0;
}

static void fenced_override_checks(void)
{
    struct nr_gpu_bridge b;
    struct nr_gpu_packet hello;
    /* The default and explicit zero leave usable semaphore exports enabled. */
    _putenv_s("NR_GPU_TRANSPORT_FENCE", "");
    CHECK(init(&b) == VK_SUCCESS && !b.fenced);
    _putenv_s("NR_GPU_TRANSPORT_FENCE", "0");
    CHECK(init(&b) == VK_SUCCESS && !b.fenced);
    _putenv_s("NR_GPU_TRANSPORT_FENCE", "1");
    CHECK(init(&b) == VK_SUCCESS && b.fenced);
    unsigned before_semaphores = semaphore_creates;
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 110, &hello) == VK_SUCCESS);
    CHECK(semaphore_creates == before_semaphores && !b.ready && !b.done);
    CHECK(hello.version == NR_GPU_FENCED_VERSION && !hello.ready_sem && !hello.done_sem);
    VkSemaphore app_wait = H(VkSemaphore, 4);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 1, &app_wait) == VK_SUCCESS);
    CHECK(b.waits_consumed && !b.capture_pending && !seen_signal_count);
    CHECK(seen_wait_count == 1 && seen_wait == app_wait);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
    CHECK(!seen_wait_count && !seen_signal_count && !b.output_pending);
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
    _putenv_s("NR_GPU_TRANSPORT_FENCE", "");
    CHECK(init(&b) == VK_SUCCESS && !b.fenced);
}

static void compatibility_checks(void)
{
    struct nr_gpu_bridge b;
    VkQueue first = H(VkQueue, 5), second = H(VkQueue, 6);
    CHECK(init(&b) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 120, NULL) == VK_SUCCESS);
    CHECK(!b.bound_queue);
    unsigned before_submits = submits;
    CHECK(nr_gpu_bridge_compatible(&b, 0, first, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(nr_gpu_bridge_compatible(&b, 0, second, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(NULL, 0, first, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 0, VK_NULL_HANDLE, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 1, first, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 0, first, 1920, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 0, first, 1280, 1080, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 0, first, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(submits == before_submits && !b.bound_queue);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), first, 0, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), first) == VK_SUCCESS);
    /* Output completion also implies capture completion, but the bookkeeping
     * flag remains set until begin/close. Queue mismatch must rebuild before
     * capture can interpret that old flag as current in-flight work. */
    CHECK(b.capture_pending && !b.output_pending && b.bound_queue == first);
    CHECK(nr_gpu_bridge_compatible(&b, 0, first, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(!nr_gpu_bridge_compatible(&b, 0, second, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 121, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_compatible(&b, 0, second, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), second, 0, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), second) == VK_SUCCESS);
    CHECK(b.bound_queue == second && b.error == VK_SUCCESS);
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
}

int main(void)
{
    const char *caller_fence = getenv("NR_GPU_TRANSPORT_FENCE");
    char *saved_fence = caller_fence ? _strdup(caller_fence) : NULL;
    CHECK(!caller_fence || saved_fence);
    _putenv_s("NR_GPU_TRANSPORT_FENCE", "");
    patch_checks();
    struct nr_gpu_bridge b;
    exportable = 0; CHECK(init(&b) == VK_ERROR_FEATURE_NOT_PRESENT); exportable = 1;
    queue_flags = 0; CHECK(init(&b) == VK_ERROR_FEATURE_NOT_PRESENT); queue_flags = VK_QUEUE_TRANSFER_BIT;
    CHECK(init(&b) == VK_SUCCESS);
    struct nr_gpu_packet hello, release;
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 12, &hello) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM, 12, &hello) == VK_SUCCESS);
    CHECK(hello.action == NR_GPU_HELLO && hello.version == 1 && hello.pid == GetCurrentProcessId());
    CHECK(hello.session == 12 && hello.bytes == 1280u * 720u * 4u);
    CHECK(hello.input_alloc == hello.bytes + 256 && hello.output_alloc == hello.input_alloc);
    CHECK(hello.input_memory_type == 1 && hello.output_memory_type == 1);
    CHECK(hello.input_memory && hello.output_memory && hello.ready_sem && hello.done_sem);
    CHECK(hello.device_uuid[0] == 12 && hello.driver_uuid[0] == 34);
    VkSemaphore app_wait = H(VkSemaphore, 4);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 1, &app_wait) == VK_SUCCESS);
    CHECK(b.waits_consumed && seen_wait_count == 1 && seen_wait == app_wait && seen_signal == b.ready);
    CHECK(b.capture_pending && b.captured && buffer_releases == 2 && initial_output_releases == 1);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 999)) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
    CHECK(!b.captured && !b.output_pending && seen_wait_count == 1 && seen_wait == b.done && !seen_signal_count);
    CHECK(seen_wait_stage == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    CHECK(nr_gpu_bridge_copy_output(&b, H(VkImage, 3), H(VkQueue, 5)) == VK_SUCCESS);
    CHECK(creates == 2 && allocations == 2 && captures == 2 && outputs == 2 && submits == 4);
    CHECK(buffer_acquires == 3 && buffer_releases == 5 && initial_output_releases == 1 && image_transitions == 8);
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 0, NULL) == VK_SUCCESS);
    HANDLE retained_handle = b.input_handle;
    wait_result = VK_TIMEOUT;
    CHECK(nr_gpu_bridge_close(&b, &release) == VK_TIMEOUT);
    CHECK(!destroys && !frees && b.input_handle == retained_handle && b.capture_pending);
    wait_result = VK_SUCCESS;
    CHECK(nr_gpu_bridge_close(&b, &release) == VK_SUCCESS);
    CHECK(release.action == NR_GPU_RELEASE && release.session == 12);
    CHECK(destroys == creates && frees == allocations && !b.input && !b.session);
    CHECK(!b.input_handle && !b.output_handle && !b.ready_handle && !b.done_handle);
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    check_closed_handles();
    CHECK(nr_gpu_bridge_configure(&b, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM, 13, NULL) == VK_SUCCESS);
    submit_result = VK_ERROR_DEVICE_LOST;
    CHECK(nr_gpu_bridge_capture(&b, H(VkImage, 3), H(VkQueue, 5), 1, &app_wait) == VK_ERROR_DEVICE_LOST);
    CHECK(!b.waits_consumed && !b.capture_pending);
    CHECK(nr_gpu_bridge_close(&b, NULL) == VK_SUCCESS);
    CHECK(destroys == creates && frees == allocations);
    check_closed_handles(); submit_result = VK_SUCCESS;
    capability_checks(); partial_failure_checks(); output_timeout_checks(); signalled_output_checks();
    fenced_fallback_checks();
    loader_callback_checks();
    fenced_override_checks();
    compatibility_checks();
    CHECK(destroys == creates && frees == allocations && semaphore_creates == semaphore_destroys);
    CHECK(pool_creates == pool_destroys && fence_creates == fence_destroys);
    _putenv_s("NR_GPU_TRANSPORT_FENCE", saved_fence ? saved_fence : "");
    free(saved_fence);
    puts("GPU bridge mock: app feature preservation, legacy EXT guard, device/driver identity, capability checks, dedicated BDA exports, ownership, all 16 partial failures, output timeout pinning and failed-submit waits OK");
    return 0;
}
