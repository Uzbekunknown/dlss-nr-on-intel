#ifndef NR_GPU_D3D12_H
#define NR_GPU_D3D12_H
/* The D3D12 client's half of the GPU transport's protocol 3 (nr_gpu_wire.h): two shared
 * committed buffers and a shared fence on the game's own device, and the packets that name
 * them to the daemon. For the XeSS proxy (src/xess) and its tests; nr_gpu_rpc.h sends them.
 * C, with COBJMACROS defined before <d3d12.h>. No Vulkan header is needed. */
#ifdef _WIN32
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>
#include "nr_gpu_wire.h"

struct nr_gpu_d3d12 {
    ID3D12Device *device;
    /* DEFAULT heap, shared, UAV-capable, in the COMMON state between uses. The output holds
     * `bytes` of answer and then the daemon's struct nr_gpu_stamp. */
    ID3D12Resource *input, *output;
    ID3D12Fence *fence;
    HANDLE input_handle, output_handle, fence_handle;   /* the daemon duplicates these */
    uint32_t width, height, format;
    uint64_t bytes, input_alloc, output_alloc, session;
    uint64_t value;   /* the fence's last value this client handed out (nr_gpu_d3d12_next) */
    LUID luid;
};

/* The adapter's LUID. Called through the vtable with the hidden return pointer spelled out,
 * which is how both MinGW's d3d12.h and the Windows SDK's declare it for C. */
static inline LUID nr_gpu_d3d12_luid(ID3D12Device *device)
{
    typedef LUID *(STDMETHODCALLTYPE *luid_fn)(ID3D12Device *, LUID *);
    LUID luid;
    memset(&luid, 0, sizeof luid);
    ((luid_fn)(void *)device->lpVtbl->GetAdapterLuid)(device, &luid);
    return luid;
}

static inline HRESULT nr_gpu_d3d12_buffer(ID3D12Device *device, uint64_t bytes, ID3D12Resource **resource,
                                   HANDLE *handle)
{
    D3D12_HEAP_PROPERTIES heap;
    memset(&heap, 0, sizeof heap);
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc;
    memset(&desc, 0, sizeof desc);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)resource);
    if (FAILED(hr)) return hr;
    hr = ID3D12Device_CreateSharedHandle(device, (ID3D12DeviceChild *)*resource, NULL, GENERIC_ALL, NULL, handle);
    if (FAILED(hr)) { ID3D12Resource_Release(*resource); *resource = NULL; }
    return hr;
}

static inline void nr_gpu_d3d12_close(struct nr_gpu_d3d12 *s)
{
    if (s->input_handle) CloseHandle(s->input_handle);
    if (s->output_handle) CloseHandle(s->output_handle);
    if (s->fence_handle) CloseHandle(s->fence_handle);
    if (s->input) ID3D12Resource_Release(s->input);
    if (s->output) ID3D12Resource_Release(s->output);
    if (s->fence) ID3D12Fence_Release(s->fence);
    memset(s, 0, sizeof *s);
}

/* A session for frames of width x height in `format` (a VkFormat number, as the daemon reads
 * the bytes: 37 for R8G8B8A8_UNORM). The device is the game's; nothing is kept on it. */
static inline HRESULT nr_gpu_d3d12_open(struct nr_gpu_d3d12 *s, ID3D12Device *device, uint32_t width,
                                 uint32_t height, uint32_t format, uint64_t session)
{
    memset(s, 0, sizeof *s);
    if (!device || !width || !height || !session || (uint64_t)width * height > (1u << 28))
        return E_INVALIDARG;
    s->device = device;
    s->width = width; s->height = height; s->format = format; s->session = session;
    s->bytes = (uint64_t)width * height * 4;
    s->input_alloc = s->bytes;
    s->output_alloc = s->bytes + sizeof(struct nr_gpu_stamp);
    s->luid = nr_gpu_d3d12_luid(device);
    HRESULT hr = nr_gpu_d3d12_buffer(device, s->input_alloc, &s->input, &s->input_handle);
    if (SUCCEEDED(hr)) hr = nr_gpu_d3d12_buffer(device, s->output_alloc, &s->output, &s->output_handle);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence,
                                                    (void **)&s->fence);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateSharedHandle(device, (ID3D12DeviceChild *)s->fence, NULL,
                                                           GENERIC_ALL, NULL, &s->fence_handle);
    if (FAILED(hr)) nr_gpu_d3d12_close(s);
    return hr;
}

/* The next frame's V: the client's queue signals it once the input is written and then waits
 * for V + 1, which only the daemon signals. */
static inline uint64_t nr_gpu_d3d12_next(struct nr_gpu_d3d12 *s)
{
    s->value += 2;
    return s->value - 1;
}

/* HELLO and RELEASE carry `value` 0, a FRAME its V. */
static inline void nr_gpu_d3d12_packet(const struct nr_gpu_d3d12 *s, uint32_t action, uint64_t value,
                                struct nr_gpu_packet *p)
{
    memset(p, 0, sizeof *p);
    p->action = action; p->pid = GetCurrentProcessId();
    p->width = s->width; p->height = s->height; p->format = s->format;
    p->version = NR_GPU_D3D12_VERSION;
    p->session = s->session;
    p->input_alloc = s->input_alloc; p->output_alloc = s->output_alloc; p->bytes = s->bytes;
    p->input_memory = (uint64_t)(uintptr_t)s->input_handle;
    p->output_memory = (uint64_t)(uintptr_t)s->output_handle;
    p->ready_sem = (uint64_t)(uintptr_t)s->fence_handle;
    p->done_sem = action == NR_GPU_FRAME ? value : 0;
    memcpy(p->device_uuid, &s->luid, sizeof s->luid);
}

/* What the daemon left in a stamp the client copied out of the output: 1 when it names V. */
static inline int nr_gpu_d3d12_answered(const struct nr_gpu_stamp *stamp, uint64_t value)
{
    return stamp->magic == NR_GPU_STAMP_MAGIC && stamp->frame_low == (uint32_t)value
        && stamp->frame_high == (uint32_t)(value >> 32);
}
#endif /* _WIN32 */
#endif /* NR_GPU_D3D12_H */
