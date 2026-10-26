/* A D3D12 client of the GPU transport's protocol 3, as the XeSS proxy will be: shared
 * committed buffers and a shared fence on its own device, frames handed over and taken back
 * on the GPU, nothing mapped but this fixture's own upload and readback buffers.
 * test_gpu_d3d12.py runs the daemon's transport against it.
 * Usage: test_gpu_d3d12 PIPE              the transport, its image pipeline a copy
 *        test_gpu_d3d12 PIPE --network    the daemon itself, with the network */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "nr_gpu_d3d12.h"
#include "nr_gpu_rpc.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (Win32 %lu)\n", __LINE__, #x, \
    (unsigned long)GetLastError()); ExitProcess(1); } } while (0)
#define HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "FAIL line %d: %s = 0x%08lx\n", \
    __LINE__, #x, (unsigned long)hr_); ExitProcess(1); } } while (0)

/* The frame the transport's test server declines (test_gpu_d3d12.py): session 2's second. */
#define DECLINED_WIDTH 96u
#define DECLINED_VALUE 3u
#define FILL 0x5a5a5a5au

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
/* One list for what goes in and one for what comes back: both are idle when a frame starts,
 * since the previous frame's readback, behind both, has been waited for. */
static ID3D12CommandAllocator *allocators[2];
static ID3D12GraphicsCommandList *lists[2];
static ID3D12Fence *own;          /* this fixture's own waits; the shared fence is the daemon's too */
static uint64_t own_value;

static uint32_t pattern(uint32_t session, uint32_t frame, uint32_t i)
{
    return 0xa5000000u ^ (session << 16) ^ (frame << 8) ^ (i * 2654435761u);
}

static ID3D12Resource *host_buffer(D3D12_HEAP_TYPE type, uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES heap = {.Type = type};
    D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes, .Height = 1,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc.Count = 1, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ID3D12Resource *resource = NULL;
    HR(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        NULL, &IID_ID3D12Resource, (void **)&resource));
    return resource;
}

static void wait_value(ID3D12Fence *fence, uint64_t value)
{
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(event);
    HR(ID3D12Fence_SetEventOnCompletion(fence, value, event));
    CHECK(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0);
    CloseHandle(event);
}

static ID3D12Device *intel_device(void)
{
    IDXGIFactory1 *factory = NULL;
    HR(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory));
    ID3D12Device *found = NULL;
    IDXGIAdapter1 *adapter = NULL;
    for (UINT i = 0; !found && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        HR(IDXGIAdapter1_GetDesc1(adapter, &desc));
        if (desc.VendorId == 0x8086u && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && SUCCEEDED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device,
                                           (void **)&found)))
            fprintf(stderr, "D3D12 on %ls, LUID %08lx:%08lx\n", desc.Description,
                    (long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart);
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return found;
}

/* One frame: the input gets this frame's words and the output FILL, the queue signals V and
 * waits for V + 1, then the output and its stamp come back to `readback`. */
static void frame(struct nr_gpu_d3d12 *s, ID3D12Resource *upload, ID3D12Resource *fill,
                  ID3D12Resource *readback, const char *pipe, uint32_t session, uint32_t index)
{
    uint32_t *words = NULL;
    D3D12_RANGE none = {0, 0};
    HR(ID3D12Resource_Map(upload, 0, &none, (void **)&words));
    for (uint32_t i = 0; i < s->bytes / 4; ++i) words[i] = pattern(session, index, i);
    ID3D12Resource_Unmap(upload, 0, NULL);
    uint64_t value = nr_gpu_d3d12_next(s);
    for (int i = 0; i < 2; ++i) {
        HR(ID3D12CommandAllocator_Reset(allocators[i]));
        HR(ID3D12GraphicsCommandList_Reset(lists[i], allocators[i], NULL));
    }
    /* COMMON buffers promote to COPY_DEST or COPY_SOURCE in a list and decay back after it. */
    ID3D12GraphicsCommandList_CopyBufferRegion(lists[0], s->input, 0, upload, 0, s->bytes);
    ID3D12GraphicsCommandList_CopyBufferRegion(lists[0], s->output, 0, fill, 0, s->bytes);
    ID3D12GraphicsCommandList_CopyBufferRegion(lists[1], readback, 0, s->output, 0, s->output_alloc);
    for (int i = 0; i < 2; ++i) HR(ID3D12GraphicsCommandList_Close(lists[i]));
    /* In the proxy's order: the first half, its signal, the wait, then what reads the answer,
     * all queued before the daemon hears of the frame. */
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&lists[0]);
    HR(ID3D12CommandQueue_Signal(queue, s->fence, value));
    HR(ID3D12CommandQueue_Wait(queue, s->fence, value + 1));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&lists[1]);
    HR(ID3D12CommandQueue_Signal(queue, own, ++own_value));
    struct nr_gpu_packet packet;
    struct nr_gpu_reply reply;
    nr_gpu_d3d12_packet(s, NR_GPU_FRAME, value, &packet);
    CHECK(nr_gpu_rpc(pipe, &packet, &reply));
    int declined = s->width == DECLINED_WIDTH && value == DECLINED_VALUE;
    CHECK(reply.status == (declined ? NR_GPU_UNSUPPORTED : NR_GPU_SUCCESS));
    wait_value(own, own_value);
    CHECK(ID3D12Fence_GetCompletedValue(s->fence) == value + 1);
    const uint32_t *back = NULL;
    D3D12_RANGE all = {0, (SIZE_T)s->output_alloc};
    HR(ID3D12Resource_Map(readback, 0, &all, (void **)&back));
    struct nr_gpu_stamp stamp;
    memcpy(&stamp, (const uint8_t *)back + s->bytes, sizeof stamp);
    CHECK(nr_gpu_d3d12_answered(&stamp, value) == !declined);
    for (uint32_t i = 0; i < s->bytes / 4; ++i) {
        uint32_t expected = declined ? FILL : pattern(session, index, i);
        if (back[i] != expected) {
            fprintf(stderr, "session %u frame %u word %u = %08x, expected %08x\n", session, index, i,
                    back[i], expected);
            ExitProcess(1);
        }
    }
    ID3D12Resource_Unmap(readback, 0, &none);
    printf("%ux%u frame %u (V=%llu): %s, fence at %llu\n", s->width, s->height, index,
           (unsigned long long)value, declined ? "declined, left as it was" : "answered and stamped",
           (unsigned long long)(value + 1));
}

/* A picture with something in it for the network: a gradient under a hashed texture. */
static uint32_t picture(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t frame)
{
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (frame * 83492791u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    uint32_t r = x * 191u / width + (h & 63u), g = y * 191u / height + (h >> 8 & 63u);
    uint32_t b = (x + y) * 95u / (width + height) + 64u + (h >> 16 & 63u);
    return r | g << 8 | b << 16 | 0xffu << 24;
}

/* The daemon's own path: what comes back is NR's answer to each frame, stamped with its V. */
static int network(const char *pipe)
{
    const uint32_t width = 640, height = 360, frames = 6;
    struct nr_gpu_d3d12 s;
    HR(nr_gpu_d3d12_open(&s, device, width, height, 37, UINT64_C(0x13579bdf02468ace)));
    ID3D12Resource *upload = host_buffer(D3D12_HEAP_TYPE_UPLOAD, s.bytes);
    ID3D12Resource *fill = host_buffer(D3D12_HEAP_TYPE_UPLOAD, s.bytes);
    ID3D12Resource *readback = host_buffer(D3D12_HEAP_TYPE_READBACK, s.output_alloc);
    uint32_t *words = NULL;
    D3D12_RANGE none = {0, 0};
    HR(ID3D12Resource_Map(fill, 0, &none, (void **)&words));
    for (uint32_t i = 0; i < s.bytes / 4; ++i) words[i] = FILL;
    ID3D12Resource_Unmap(fill, 0, NULL);
    struct nr_gpu_packet packet;
    struct nr_gpu_reply reply;
    DWORD peer = 0;
    nr_gpu_d3d12_packet(&s, NR_GPU_HELLO, 0, &packet);
    CHECK(nr_gpu_rpc_peer(pipe, &packet, &reply, &peer) && reply.status == NR_GPU_SUCCESS);
    for (uint32_t index = 0; index < frames; ++index) {
        HR(ID3D12Resource_Map(upload, 0, &none, (void **)&words));
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) words[y * width + x] = picture(x, y, width, height, index);
        ID3D12Resource_Unmap(upload, 0, NULL);
        uint64_t value = nr_gpu_d3d12_next(&s);
        for (int i = 0; i < 2; ++i) {
            HR(ID3D12CommandAllocator_Reset(allocators[i]));
            HR(ID3D12GraphicsCommandList_Reset(lists[i], allocators[i], NULL));
        }
        ID3D12GraphicsCommandList_CopyBufferRegion(lists[0], s.input, 0, upload, 0, s.bytes);
        ID3D12GraphicsCommandList_CopyBufferRegion(lists[0], s.output, 0, fill, 0, s.bytes);
        ID3D12GraphicsCommandList_CopyBufferRegion(lists[1], readback, 0, s.output, 0, s.output_alloc);
        for (int i = 0; i < 2; ++i) HR(ID3D12GraphicsCommandList_Close(lists[i]));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&lists[0]);
        HR(ID3D12CommandQueue_Signal(queue, s.fence, value));
        HR(ID3D12CommandQueue_Wait(queue, s.fence, value + 1));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&lists[1]);
        HR(ID3D12CommandQueue_Signal(queue, own, ++own_value));
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        nr_gpu_d3d12_packet(&s, NR_GPU_FRAME, value, &packet);
        CHECK(nr_gpu_rpc(pipe, &packet, &reply) && reply.status == NR_GPU_SUCCESS);
        wait_value(own, own_value);
        QueryPerformanceCounter(&t1);
        CHECK(ID3D12Fence_GetCompletedValue(s.fence) == value + 1);
        const uint8_t *back = NULL;
        D3D12_RANGE all = {0, (SIZE_T)s.output_alloc};
        HR(ID3D12Resource_Map(readback, 0, &all, (void **)&back));
        struct nr_gpu_stamp stamp;
        memcpy(&stamp, back + s.bytes, sizeof stamp);
        CHECK(nr_gpu_d3d12_answered(&stamp, value));
        uint64_t changed = 0, filled = 0, total = 0;
        for (uint32_t i = 0; i < width * height; ++i) {
            uint32_t given = picture(i % width, i / width, width, height, index), got;
            memcpy(&got, back + 4 * (size_t)i, 4);
            filled += got == FILL;
            for (int c = 0; c < 3; ++c) {
                int d = (int)(got >> 8 * c & 255u) - (int)(given >> 8 * c & 255u);
                total += (uint64_t)(d < 0 ? -d : d);
                changed += d != 0;
            }
        }
        ID3D12Resource_Unmap(readback, 0, &none);
        double share = (double)changed / (3.0 * width * height), mean = (double)total / (3.0 * width * height);
        printf("%ux%u frame %u (V=%llu): answered and stamped, %.1f%% of channels changed, mean %.2f levels, "
               "%.1f ms\n", width, height, index, (unsigned long long)value, 100 * share, mean,
               1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
        /* NR's answer, not the frame as it came, not the fill left over, and not noise. */
        CHECK(!filled && share > 0.05 && mean < 32.0);
    }
    nr_gpu_d3d12_packet(&s, NR_GPU_RELEASE, 0, &packet);
    CHECK(nr_gpu_rpc(pipe, &packet, &reply) && reply.status == NR_GPU_SUCCESS);
    ID3D12Resource_Release(upload);
    ID3D12Resource_Release(fill);
    ID3D12Resource_Release(readback);
    nr_gpu_d3d12_close(&s);
    printf("GPU D3D12 frames through the daemon's network: %u answered, peer %lu\n", frames, (unsigned long)peer);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "--network")));
    device = intel_device();
    CHECK(device);
    D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    HR(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    for (int i = 0; i < 2; ++i) {
        HR(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               &IID_ID3D12CommandAllocator, (void **)&allocators[i]));
        HR(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i], NULL,
                                          &IID_ID3D12GraphicsCommandList, (void **)&lists[i]));
        HR(ID3D12GraphicsCommandList_Close(lists[i]));
    }
    HR(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&own));
    if (argc == 3) return network(argv[1]);
    for (uint32_t session = 1; session <= 2; ++session) {
        uint32_t width = session == 1 ? 64 : DECLINED_WIDTH, height = session == 1 ? 64 : 80;
        struct nr_gpu_d3d12 s;
        HR(nr_gpu_d3d12_open(&s, device, width, height, 37, UINT64_C(0x2468ace013579bd0) + session));
        ID3D12Resource *upload = host_buffer(D3D12_HEAP_TYPE_UPLOAD, s.bytes);
        ID3D12Resource *fill = host_buffer(D3D12_HEAP_TYPE_UPLOAD, s.bytes);
        ID3D12Resource *readback = host_buffer(D3D12_HEAP_TYPE_READBACK, s.output_alloc);
        uint32_t *words = NULL;
        D3D12_RANGE none = {0, 0};
        HR(ID3D12Resource_Map(fill, 0, &none, (void **)&words));
        for (uint32_t i = 0; i < s.bytes / 4; ++i) words[i] = FILL;
        ID3D12Resource_Unmap(fill, 0, NULL);
        struct nr_gpu_packet packet;
        struct nr_gpu_reply reply;
        DWORD peer = 0;
        nr_gpu_d3d12_packet(&s, NR_GPU_HELLO, 0, &packet);
        CHECK(nr_gpu_rpc_peer(argv[1], &packet, &reply, &peer) && reply.status == NR_GPU_SUCCESS);
        CHECK(peer && reply.detail == peer);
        for (uint32_t index = 0; index < 4; ++index) frame(&s, upload, fill, readback, argv[1], session, index);
        nr_gpu_d3d12_packet(&s, NR_GPU_RELEASE, 0, &packet);
        CHECK(nr_gpu_rpc(argv[1], &packet, &reply) && reply.status == NR_GPU_SUCCESS);
        ID3D12Resource_Release(upload);
        ID3D12Resource_Release(fill);
        ID3D12Resource_Release(readback);
        nr_gpu_d3d12_close(&s);
        printf("session %u, %ux%u: HELLO by LUID, four frames, RELEASE acknowledged, peer %lu\n",
               session, width, height, (unsigned long)peer);
    }
    nr_gpu_rpc_reap();
    ID3D12Fence_Release(own);
    for (int i = 0; i < 2; ++i) {
        ID3D12GraphicsCommandList_Release(lists[i]);
        ID3D12CommandAllocator_Release(allocators[i]);
    }
    ID3D12CommandQueue_Release(queue);
    ID3D12Device_Release(device);
    puts("GPU D3D12 interop: shared buffers and fence, frames answered and declined on the GPU, two sessions");
    return 0;
}
