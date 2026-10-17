/* Bounded native Win32 present smoke through the actual NR Vulkan layer.
 *
 * No installed proxy or game is involved. The driver supplies a temporary
 * VK_LAYER_PATH manifest, NR_* variables and a prestarted daemon. The fixture
 * clears SDR swapchain images, presents twelve frames, resizes its own hidden
 * window/swapchain and presents twelve more. Optional outage checking reads
 * one diagnostic pixel only; ordinary transport checks never map pixels.
 * This is an integration smoke; daemon/layer logs must confirm the chosen
 * transport. A successful present alone does not prove NR processed a frame.
 */
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_IMAGES 16u
#define WAIT_NS UINT64_C(10000000000)

static LONG validation_errors;
static int trace_calls;
static int use_nr_layer = 1;
static const wchar_t window_class[] = L"NR_GPU_Present_Fixture";

struct chain {
    VkSwapchainKHR handle;
    VkExtent2D extent;
    VkFormat format;
    uint32_t count;
    VkImage images[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES];
    unsigned written[MAX_IMAGES];
};

struct fixture {
    HINSTANCE module;
    HWND window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkSemaphore acquired;
    VkFence fence;
    struct chain chain;
    VkBuffer probe;
    VkDeviceMemory probe_memory;
    void *probe_mapped;
    unsigned original_frame;
    unsigned second_original_frame;
    int original_verified;
    int unsafe;
};

static int checked(struct fixture *f, VkResult r, const char *call, int line)
{
    if (r == VK_SUCCESS) return 1;
    fprintf(stderr, "FAIL line %d: %s = %d\n", line, call, (int)r);
    if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) f->unsafe = 1;
    return 0;
}
#define CHECK(f, call) do { \
    if (trace_calls) fprintf(stderr, "fixture call: %s\n", #call); \
    if (!checked((f), (call), #call, __LINE__)) return 0; \
} while (0)

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int pump(void)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return 0;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 1;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL validation(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)type; (void)user;
    /* A loader diagnostic about ANOTHER module failing to load is not a verdict on this layer:
     * the loader reports implicit-layer problems through this same messenger, and the development
     * machine has a ReShade implicit layer whose DLL will not open in this process - counting it
     * failed the smoke with the layer itself clean. Named narrowly on purpose; no Vulkan usage
     * error says this. */
    int foreign_load_failure = data && data->pMessage &&
        strstr(data->pMessage, "Failed to open dynamic library") != NULL;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && !foreign_load_failure)
        InterlockedIncrement(&validation_errors);
    fprintf(stderr, "validation %s: %s\n",
        severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning",
        data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static int instance_extension(const char *layer, const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, NULL) != VK_SUCCESS || count > 65536u)
        return 0;
    VkExtensionProperties *properties = calloc(count ? count : 1u, sizeof *properties);
    if (!properties) return 0;
    int found = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i)
            if (!strcmp(properties[i].extensionName, name)) { found = 1; break; }
    free(properties);
    return found;
}

static int validation_available(void)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count > 65536u) return 0;
    VkLayerProperties *properties = calloc(count ? count : 1u, sizeof *properties);
    if (!properties) return 0;
    int found = 0;
    if (vkEnumerateInstanceLayerProperties(&count, properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i)
            if (!strcmp(properties[i].layerName, "VK_LAYER_KHRONOS_validation")) { found = 1; break; }
    free(properties);
    return found;
}

static int initialize_instance(struct fixture *f, int validate, int require_sync, uint32_t api)
{
    const char *layers[2];
    uint32_t layer_count = 0;
    if (use_nr_layer) layers[layer_count++] = "VK_LAYER_dlssnr_intel";
    if (validate) layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
    const char *extensions[4] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    uint32_t extension_count = 2;
    int synchronization_validation = 0;
    if (validate) {
        if (!validation_available() || !instance_extension(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            fprintf(stderr, "requested Vulkan validation layer/debug-utils extension is unavailable\n");
            return 0;
        }
        extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        synchronization_validation = instance_extension("VK_LAYER_KHRONOS_validation",
                                                        VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        if (require_sync && !synchronization_validation) {
            fprintf(stderr, "requested synchronization validation extension is unavailable\n");
            return 0;
        }
        if (synchronization_validation) extensions[extension_count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
    }
    VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = synchronization_validation ? &features : NULL,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NR GPU present fixture", .apiVersion = api};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validate ? &debug : NULL, .pApplicationInfo = &app,
        .enabledLayerCount = layer_count, .ppEnabledLayerNames = layers,
        .enabledExtensionCount = extension_count, .ppEnabledExtensionNames = extensions};
    CHECK(f, vkCreateInstance(&info, NULL, &f->instance));
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(f->instance, "vkCreateDebugUtilsMessengerEXT");
        if (!create) { fprintf(stderr, "debug-utils entry point unavailable\n"); return 0; }
        debug.pNext = NULL;
        CHECK(f, create(f->instance, &debug, NULL, &f->debug));
    }
    printf("instance_api=1.%u nr_layer=%s validation=%s synchronization_validation=%s\n",
        VK_API_VERSION_MINOR(api), use_nr_layer ? "on" : "off", validate ? "on" : "off",
        synchronization_validation ? "on" : "off");
    return 1;
}

static int device_extensions(VkPhysicalDevice physical)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS || count > 65536u)
        return 0;
    VkExtensionProperties *properties = calloc(count ? count : 1u, sizeof *properties);
    if (!properties) return 0;
    int swapchain = 0, bda = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i) {
            swapchain |= !strcmp(properties[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            bda |= !strcmp(properties[i].extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        }
    free(properties);
    return swapchain && bda;
}

static int initialize_device(struct fixture *f)
{
    uint32_t count = 0;
    CHECK(f, vkEnumeratePhysicalDevices(f->instance, &count, NULL));
    if (!count || count > 256u) { fprintf(stderr, "no usable physical device\n"); return 0; }
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices) return 0;
    VkResult result = vkEnumeratePhysicalDevices(f->instance, &count, devices);
    if (result != VK_SUCCESS) { free(devices); return checked(f, result, "enumerate physical devices", __LINE__); }
    for (uint32_t n = 0; n < count && !f->physical; ++n) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[n], &properties);
        if (properties.vendorID != 0x8086u || properties.apiVersion < VK_API_VERSION_1_1 || !device_extensions(devices[n]))
            continue;
        VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &bda};
        vkGetPhysicalDeviceFeatures2(devices[n], &features);
        if (!bda.bufferDeviceAddress) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[n], &families, NULL);
        if (!families || families > 256u) continue;
        VkQueueFamilyProperties *queues = calloc(families, sizeof *queues);
        if (!queues) { free(devices); return 0; }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[n], &families, queues);
        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 present = VK_FALSE;
            VkResult supported = vkGetPhysicalDeviceSurfaceSupportKHR(devices[n], i, f->surface, &present);
            if (supported == VK_SUCCESS && present && queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                f->physical = devices[n]; f->family = i;
                printf("device=%s family=%u device_api=1.%u\n", properties.deviceName, i,
                    VK_API_VERSION_MINOR(properties.apiVersion));
                if (properties.apiVersion >= VK_API_VERSION_1_2) {
                    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                        .pNext = &driver};
                    vkGetPhysicalDeviceProperties2(devices[n], &properties2);
                    printf("driver=%s info=%s version_raw=%u\n", driver.driverName, driver.driverInfo,
                        properties.driverVersion);
                }
                break;
            }
        }
        free(queues);
    }
    free(devices);
    if (!f->physical) { fprintf(stderr, "no Intel device with graphics/present/BDA support\n"); return 0; }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = f->family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .bufferDeviceAddress = VK_TRUE};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &bda,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    CHECK(f, vkCreateDevice(f->physical, &info, NULL, &f->device));
    vkGetDeviceQueue(f->device, f->family, 0, &f->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = f->family};
    CHECK(f, vkCreateCommandPool(f->device, &pool, NULL, &f->pool));
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = f->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(f, vkAllocateCommandBuffers(f->device, &command, &f->command));
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(f, vkCreateSemaphore(f->device, &semaphore, NULL, &f->acquired));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    CHECK(f, vkCreateFence(f->device, &fence, NULL, &f->fence));
    if (f->original_frame) {
        VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(f, vkCreateBuffer(f->device, &buffer, NULL, &f->probe));
        VkMemoryRequirements need;
        VkPhysicalDeviceMemoryProperties memory;
        vkGetBufferMemoryRequirements(f->device, f->probe, &need);
        vkGetPhysicalDeviceMemoryProperties(f->physical, &memory);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((need.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i; break;
            }
        if (type == UINT32_MAX) { fprintf(stderr, "outage pixel probe requires coherent host-visible memory\n"); return 0; }
        VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = need.size, .memoryTypeIndex = type};
        CHECK(f, vkAllocateMemory(f->device, &allocate, NULL, &f->probe_memory));
        CHECK(f, vkBindBufferMemory(f->device, f->probe, f->probe_memory, 0));
        CHECK(f, vkMapMemory(f->device, f->probe_memory, 0, 4, 0, &f->probe_mapped));
    }
    return 1;
}

static void close_chain(struct fixture *f, struct chain *chain)
{
    for (uint32_t i = 0; i < chain->count; ++i)
        if (chain->rendered[i]) vkDestroySemaphore(f->device, chain->rendered[i], NULL);
    if (chain->handle) vkDestroySwapchainKHR(f->device, chain->handle, NULL);
    memset(chain, 0, sizeof *chain);
}

static int create_chain(struct fixture *f, unsigned width, unsigned height)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(f, vkGetPhysicalDeviceSurfaceCapabilitiesKHR(f->physical, f->surface, &caps));
    uint32_t count = 0;
    CHECK(f, vkGetPhysicalDeviceSurfaceFormatsKHR(f->physical, f->surface, &count, NULL));
    if (!count || count > 256u) { fprintf(stderr, "no usable surface formats\n"); return 0; }
    VkSurfaceFormatKHR formats[256];
    CHECK(f, vkGetPhysicalDeviceSurfaceFormatsKHR(f->physical, f->surface, &count, formats));
    VkSurfaceFormatKHR format = {.format = VK_FORMAT_UNDEFINED};
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)) {
            format = formats[i]; break;
        }
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        format = (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_UNORM, formats[0].colorSpace};
    if (format.format == VK_FORMAT_UNDEFINED) { fprintf(stderr, "SDR UNORM surface format unavailable\n"); return 0; }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & usage) != usage) { fprintf(stderr, "surface lacks transfer/color usage\n"); return 0; }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width < caps.minImageExtent.width ? caps.minImageExtent.width : width;
        extent.height = height < caps.minImageExtent.height ? caps.minImageExtent.height : height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    uint32_t images = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        VkCompositeAlphaFlagsKHR bits = caps.supportedCompositeAlpha;
        alpha = (VkCompositeAlphaFlagBitsKHR)(bits & (~bits + 1u));
    }
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = f->surface, .minImageCount = images, .imageFormat = format.format, .imageColorSpace = format.colorSpace,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
        .oldSwapchain = f->chain.handle};
    struct chain next = {.extent = extent, .format = format.format};
    if (trace_calls) fprintf(stderr, "fixture call: vkCreateSwapchainKHR extent=%ux%u images=%u format=%d usage=0x%x alpha=0x%x surface=%llx\n",
        extent.width, extent.height, images, (int)format.format, (unsigned)usage, (unsigned)alpha,
        (unsigned long long)(uintptr_t)f->surface);
    VkResult result = vkCreateSwapchainKHR(f->device, &info, NULL, &next.handle);
    if (trace_calls) fprintf(stderr, "fixture result: vkCreateSwapchainKHR=%d handle=%llx\n",
        (int)result, (unsigned long long)(uintptr_t)next.handle);
    if (result != VK_SUCCESS) return checked(f, result, "vkCreateSwapchainKHR", __LINE__);
    if (trace_calls) fputs("fixture call: vkGetSwapchainImagesKHR count\n", stderr);
    result = vkGetSwapchainImagesKHR(f->device, next.handle, &next.count, NULL);
    if (result != VK_SUCCESS || !next.count || next.count > MAX_IMAGES) goto failed;
    if (trace_calls) fprintf(stderr, "fixture call: vkGetSwapchainImagesKHR images count=%u\n", next.count);
    result = vkGetSwapchainImagesKHR(f->device, next.handle, &next.count, next.images);
    if (result != VK_SUCCESS) goto failed;
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < next.count; ++i) {
        result = vkCreateSemaphore(f->device, &semaphore, NULL, &next.rendered[i]);
        if (result != VK_SUCCESS) goto failed;
    }
    close_chain(f, &f->chain);
    f->chain = next;
    printf("swapchain=%ux%u images=%u format=%d\n", extent.width, extent.height, next.count, (int)format.format);
    fflush(stdout);
    return 1;
failed:
    /* A driver reporting an unexpectedly large image count cannot index arrays. */
    if (next.count > MAX_IMAGES) next.count = 0;
    close_chain(f, &next);
    if (result == VK_SUCCESS) fprintf(stderr, "swapchain image count unsupported\n");
    else checked(f, result, "initialize swapchain images/semaphores", __LINE__);
    return 0;
}

static int draw(struct fixture *f, unsigned phase, unsigned frame)
{
    if (!pump()) { fprintf(stderr, "fixture window closed early\n"); return 0; }
    CHECK(f, vkWaitForFences(f->device, 1, &f->fence, VK_TRUE, WAIT_NS));
    uint32_t image = 0;
    VkResult result = vkAcquireNextImageKHR(f->device, f->chain.handle, WAIT_NS, f->acquired, VK_NULL_HANDLE, &image);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return checked(f, result, "vkAcquireNextImageKHR", __LINE__);
    if (image >= f->chain.count) { fprintf(stderr, "invalid acquired image index\n"); return 0; }
    CHECK(f, vkResetCommandBuffer(f->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(f, vkBeginCommandBuffer(f->command, &begin));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier into = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = f->chain.images[image], .subresourceRange = range};
    int probe = phase == 1 && f->original_frame && !f->original_verified
             && f->chain.written[image] == f->original_frame;
    if (probe) {
        VkImageMemoryBarrier sample = into;
        sample.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sample.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        sample.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &sample);
        VkBufferImageCopy pixel = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {(int32_t)(f->chain.extent.width / 2u), (int32_t)(f->chain.extent.height / 2u), 0},
            .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(f->command, f->chain.images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            f->probe, 1, &pixel);
        VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = f->probe, .size = 4};
        vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &host, 0, NULL);
        into.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        into.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    /* TRANSFER matches the acquire-semaphore wait stage, including the layout transition. */
    vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &into);
    VkClearColorValue color = {.float32 = {0.20f + (float)(frame % 8u) * 0.01f,
        0.40f + (float)phase * 0.02f, 0.60f, 1.0f}};
    vkCmdClearColorImage(f->command, f->chain.images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier out = into;
    out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; out.dstAccessMask = 0;
    out.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; out.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &out);
    CHECK(f, vkEndCommandBuffer(f->command));
    CHECK(f, vkResetFences(f->device, 1, &f->fence));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->acquired, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
        .pCommandBuffers = &f->command, .signalSemaphoreCount = 1, .pSignalSemaphores = &f->chain.rendered[image]};
    CHECK(f, vkQueueSubmit(f->queue, 1, &submit, f->fence));
    if (probe) {
        CHECK(f, vkWaitForFences(f->device, 1, &f->fence, VK_TRUE, WAIT_NS));
        unsigned char pixel[4];
        memcpy(pixel, f->probe_mapped, sizeof pixel);
        float red = 0.20f + (float)(f->original_frame % 8u) * 0.01f;
        unsigned char expected[4] = {(unsigned char)(255.0f * red + 0.5f), 107, 153, 255};
        if (f->chain.format == VK_FORMAT_B8G8R8A8_UNORM) {
            unsigned char swap = expected[0]; expected[0] = expected[2]; expected[2] = swap;
        }
        for (unsigned i = 0; i < 4; ++i)
            if (pixel[i] != expected[i]) {
                fprintf(stderr, "outage image pixel changed: channel=%u actual=%u expected=%u\n",
                    i, (unsigned)pixel[i], (unsigned)expected[i]); return 0;
            }
        unsigned sampled_original_frame = f->original_frame;
        f->original_verified = 1;
        if (f->second_original_frame) {
            f->original_frame = f->second_original_frame;
            f->second_original_frame = 0;
            f->original_verified = 0;
        }
        printf("original_pixel frame=%u value=%u,%u,%u,%u status=retained\n", sampled_original_frame,
            (unsigned)pixel[0], (unsigned)pixel[1], (unsigned)pixel[2], (unsigned)pixel[3]);
    }
    /* A render-finished semaphore per image is reusable only after reacquiring that image. */
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->chain.rendered[image], .swapchainCount = 1,
        .pSwapchains = &f->chain.handle, .pImageIndices = &image};
    LARGE_INTEGER present_start, present_end, present_frequency;
    QueryPerformanceFrequency(&present_frequency);
    QueryPerformanceCounter(&present_start);
    result = vkQueuePresentKHR(f->queue, &present);
    QueryPerformanceCounter(&present_end);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return checked(f, result, "vkQueuePresentKHR", __LINE__);
    f->chain.written[image] = frame;
    printf("present phase=%u frame=%u extent=%ux%u image=%u result=%d present_ms=%.6f\n",
        phase, frame, f->chain.extent.width, f->chain.extent.height, image, (int)result,
        1000.0 * (double)(present_end.QuadPart - present_start.QuadPart) / (double)present_frequency.QuadPart);
    fflush(stdout);
    return 1;
}

static void cleanup(struct fixture *f)
{
    if (!f->unsafe) {
        if (f->device) {
            VkResult idle = vkDeviceWaitIdle(f->device);
            if (idle == VK_SUCCESS) {
                close_chain(f, &f->chain);
                if (f->fence) vkDestroyFence(f->device, f->fence, NULL);
                if (f->acquired) vkDestroySemaphore(f->device, f->acquired, NULL);
                if (f->pool) vkDestroyCommandPool(f->device, f->pool, NULL);
                if (f->probe_mapped) vkUnmapMemory(f->device, f->probe_memory);
                if (f->probe) vkDestroyBuffer(f->device, f->probe, NULL);
                if (f->probe_memory) vkFreeMemory(f->device, f->probe_memory, NULL);
                vkDestroyDevice(f->device, NULL);
            } else f->unsafe = 1;
        }
        if (!f->unsafe && f->instance) {
            if (f->surface) vkDestroySurfaceKHR(f->instance, f->surface, NULL);
            if (f->debug) {
                PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
                    vkGetInstanceProcAddr(f->instance, "vkDestroyDebugUtilsMessengerEXT");
                if (destroy) destroy(f->instance, f->debug, NULL);
            }
            vkDestroyInstance(f->instance, NULL);
        }
    }
    if (f->unsafe) fprintf(stderr, "GPU failure: resources retained until this fixture process exits\n");
    if (f->window && IsWindow(f->window)) DestroyWindow(f->window);
    if (f->module) UnregisterClassW(window_class, f->module);
}

static int parse_extent(const char *value, VkExtent2D *extent)
{
    unsigned long width, height;
    char extra;
    if (sscanf(value, "%lux%lu%c", &width, &height, &extra) != 2 ||
        width < 128 || height < 64 || width > 4096 || height > 4096) return 0;
    extent->width = (uint32_t)width;
    extent->height = (uint32_t)height;
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned frames = 12, pause_ms = 0, original_frame = 0, second_original_frame = 0;
    int stepped = 0;
    int validate = 0, require_sync = 0;
    uint32_t api = VK_API_VERSION_1_3;
    VkExtent2D initial_extent = {128, 64}, resized_extent = {160, 96};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("test_gpu_present [--validation] [--sync-validation] [--api11] [--trace] [--no-layer] [--step] [--check-original-frame N] [--frames 1..120] [--pause-ms 0..1000] [--extent WxH] [--resize-extent WxH]\n"
                 "Uses temporary loader manifest/environment and prestarted daemon supplied by the driver.\n"
                 "Two bounded hidden SDR Win32 swapchains (default 128x64 -> 160x96); exits after both phases. Optional original check samples one pixel.");
            return 0;
        } else if (!strcmp(argv[i], "--validation")) validate = 1;
        else if (!strcmp(argv[i], "--sync-validation")) { validate = 1; require_sync = 1; }
        else if (!strcmp(argv[i], "--api11")) api = VK_API_VERSION_1_1;
        else if (!strcmp(argv[i], "--trace")) trace_calls = 1;
        else if (!strcmp(argv[i], "--no-layer")) use_nr_layer = 0;
        else if (!strcmp(argv[i], "--step")) stepped = 1;
        else if ((!strcmp(argv[i], "--extent") || !strcmp(argv[i], "--resize-extent")) && i + 1 < argc) {
            int resized = !strcmp(argv[i], "--resize-extent");
            if (!parse_extent(argv[++i], resized ? &resized_extent : &initial_extent)) {
                fprintf(stderr, "invalid extent (width 128..4096, height 64..4096)\n"); return 2;
            }
        }
        else if ((!strcmp(argv[i], "--frames") || !strcmp(argv[i], "--pause-ms") ||
                  !strcmp(argv[i], "--check-original-frame") || !strcmp(argv[i], "--check-second-original-frame")) && i + 1 < argc) {
            int frame_option = !strcmp(argv[i], "--frames");
            int original_option = !strcmp(argv[i], "--check-original-frame");
            int second_original_option = !strcmp(argv[i], "--check-second-original-frame");
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!end || *end || (frame_option || original_option || second_original_option ? value < 1 || value > 120 : value > 1000)) {
                fprintf(stderr, "invalid bounded frame/pause option\n"); return 2;
            }
            if (frame_option) frames = (unsigned)value;
            else if (original_option) original_frame = (unsigned)value;
            else if (second_original_option) second_original_frame = (unsigned)value;
            else pause_ms = (unsigned)value;
        } else { fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]); return 2; }
    }
    struct fixture f = {0};
    f.original_frame = original_frame;
    f.second_original_frame = second_original_frame;
    if (second_original_frame && (!original_frame || second_original_frame <= original_frame || second_original_frame >= frames)) {
        fprintf(stderr, "second original check must follow the first and precede the last frame\n"); return 2;
    }
    if (!use_nr_layer) {
        SetEnvironmentVariableA("VK_INSTANCE_LAYERS", NULL);
        SetEnvironmentVariableA("ENABLE_NR_LAYER", NULL);
        SetEnvironmentVariableA("DISABLE_NR_LAYER", "1");
    }
    int passed = 0;
    f.module = GetModuleHandleW(NULL);
    WNDCLASSW klass = {.lpfnWndProc = window_proc, .hInstance = f.module, .lpszClassName = window_class};
    if (!RegisterClassW(&klass)) { fprintf(stderr, "RegisterClass: %lu\n", (unsigned long)GetLastError()); goto done; }
    f.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class, L"NR GPU present fixture",
        WS_POPUP, 0, 0, (int)initial_extent.width, (int)initial_extent.height, NULL, NULL, f.module, NULL);
    if (!f.window) { fprintf(stderr, "CreateWindow: %lu\n", (unsigned long)GetLastError()); goto done; }
    /* No WS_VISIBLE or ShowWindow: this owned smoke surface never takes focus. */
    if (!initialize_instance(&f, validate, require_sync, api)) goto done;
    if (trace_calls) fputs("fixture call: vkCreateWin32SurfaceKHR\n", stderr);
    VkWin32SurfaceCreateInfoKHR surface = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = f.module, .hwnd = f.window};
    if (!checked(&f, vkCreateWin32SurfaceKHR(f.instance, &surface, NULL, &f.surface), "vkCreateWin32SurfaceKHR", __LINE__)) goto done;
    if (!initialize_device(&f) || !create_chain(&f, initial_extent.width, initial_extent.height)) goto done;
    for (unsigned phase = 1; phase <= 2; ++phase) {
        printf("phase_begin=%u pid=%lu frames=%u\n", phase, (unsigned long)GetCurrentProcessId(), frames);
        fflush(stdout);
        for (unsigned frame = 1; frame <= frames; ++frame) {
            if (!draw(&f, phase, frame)) goto done;
            if (stepped) {
                char command[16];
                if (!fgets(command, sizeof command, stdin) || strcmp(command, "go\n")) {
                    fprintf(stderr, "fixture step acknowledgement missing or invalid\n"); goto done;
                }
            }
            if (pause_ms) Sleep(pause_ms);
        }
        if (!checked(&f, vkDeviceWaitIdle(f.device), "phase vkDeviceWaitIdle", __LINE__)) goto done;
        if (phase == 1) {
            if (f.original_frame && !f.original_verified) {
                fprintf(stderr, "outage image was not reacquired for its original-pixel check\n"); goto done;
            }
            if (!SetWindowPos(f.window, NULL, 0, 0, (int)resized_extent.width, (int)resized_extent.height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE)) {
                fprintf(stderr, "SetWindowPos: %lu\n", (unsigned long)GetLastError()); goto done;
            }
            if (!pump() || !create_chain(&f, resized_extent.width, resized_extent.height)) goto done;
        }
    }
    passed = 1;
done:
    cleanup(&f);
    if (validation_errors) { fprintf(stderr, "validation_errors=%ld\n", validation_errors); passed = 0; }
    if (f.unsafe) passed = 0;
    if (passed) puts("Win32 present smoke completed both phases; inspect NR logs to confirm GPU processing.");
    return passed ? 0 : 1;
}
