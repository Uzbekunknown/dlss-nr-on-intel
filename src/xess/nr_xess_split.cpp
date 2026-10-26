/* nr_xess_split.cpp — the game's command list cut in two at xessD3D12Execute, so that the
 * daemon's network runs on the frame between the halves (nr_xess.c, mode=cut).
 *
 * Adapted from DLSSNR-AMD's windows/src/pe/nr_pe_d3d12split.cpp at commit c71b0f4,
 * https://github.com/mochizuki0323/DLSSNR-AMD, Copyright (c) 2026 mochizuki0323, under the
 * MIT License, which licenses/dlssnr-amd-MIT.txt keeps. Changed here: the namespace and the
 * log, the interface for C at the end (nr_xess_split.h), and the queue it remembered for its
 * own menu, which this tree has no use for. Its account of the design, from its header:
 *
 * An upscaler's evaluate is recorded into the game's command list, between the game's work
 * that made its inputs and the work that reads its output. Nothing can be put in the middle
 * of a D3D12 command list that waits for another API: fences are queue operations. So the
 * list is cut at the evaluate:
 *   - everything the list holds so far stays in it, and it is closed there;
 *   - every call the game (and the upscaler after us) makes on the list from then on is
 *     redirected to a continuation list of the same class, created on the list's own device,
 *     which starts with the list's state as it was when the evaluate began;
 *   - when the game executes the list, the call is expanded on the same queue into
 *         list -> signal -> [our work, waiting on the signal] -> wait -> continuation -> rest.
 * The redirection is a patch of the list class's vtable: every recording method goes through
 * a small stub that swaps `this` for the continuation while a cut is active, and the
 * state-setting methods go through hooks that also remember the list's state since its last
 * Reset (descriptor heaps, root signatures and arguments, pipeline, render targets,
 * viewports, input assembly, ...), which is what the continuation is given.
 * ExecuteCommandLists and Reset are hooked the same way. x86-64 only.
 */
#include "nr_xess_split.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nr_xess_split {

// The proxy's log (nr_split_set_log), a line at a time.
static void (*g_say)(const char* line) = nullptr;
static void note(const char* fmt, ...) {
    if (!g_say) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_say(line);
}

// What runs between the halves, called inside the queue's ExecuteCommandLists after the first
// half has been handed to the queue. It must leave the queue waiting (on the GPU) for its
// work, or do nothing at all. `discard` is called instead when the list is reset or the cut
// abandoned without the list ever being executed.
struct Job {
    std::function<void(ID3D12CommandQueue* queue)> run;
    std::function<void()> discard;
};
struct Snapshot;

#if !defined(_M_X64) && !defined(__x86_64__)
bool prepare(ID3D12GraphicsCommandList*, std::string* why) {
    *why = "the command-list cut is x86-64 only";
    return false;
}
struct Snapshot {};
std::shared_ptr<Snapshot> snapshot(ID3D12GraphicsCommandList*) { return nullptr; }
void restore(ID3D12GraphicsCommandList*, const std::shared_ptr<Snapshot>&) {}
bool cut(ID3D12GraphicsCommandList*, Job, const std::shared_ptr<Snapshot>&, std::string* why) {
    *why = "the command-list cut is x86-64 only";
    return false;
}
#else

namespace {

// ---- vtable layout ------------------------------------------------------------------------------
// ID3D12GraphicsCommandList .. 10, as declared in d3d12.h (mingw's for .. 7, the Agility SDK's IDL for
// 8 .. 10). A class implements a prefix of this; how much is found by QueryInterface (slot_count).
enum Slot : unsigned {
    kRelease = 2, kClose = 9, kReset = 10, kClearState = 11,
    kIASetPrimitiveTopology = 20, kRSSetViewports = 21, kRSSetScissorRects = 22, kOMSetBlendFactor = 23,
    kOMSetStencilRef = 24, kSetPipelineState = 25, kExecuteBundle = 27, kSetDescriptorHeaps = 28,
    kSetComputeRootSignature = 29, kSetGraphicsRootSignature = 30,
    kSetComputeRootDescriptorTable = 31, kSetGraphicsRootDescriptorTable = 32,
    kSetComputeRoot32BitConstant = 33, kSetGraphicsRoot32BitConstant = 34,
    kSetComputeRoot32BitConstants = 35, kSetGraphicsRoot32BitConstants = 36,
    kSetComputeRootConstantBufferView = 37, kSetGraphicsRootConstantBufferView = 38,
    kSetComputeRootShaderResourceView = 39, kSetGraphicsRootShaderResourceView = 40,
    kSetComputeRootUnorderedAccessView = 41, kSetGraphicsRootUnorderedAccessView = 42,
    kIASetIndexBuffer = 43, kIASetVertexBuffers = 44, kSOSetTargets = 45, kOMSetRenderTargets = 46,
    kBeginQuery = 52, kEndQuery = 53, kSetPredication = 55,
    kOMSetDepthBounds = 62, kSetSamplePositions = 63, kSetViewInstanceMask = 65,   // List1
    kBeginRenderPass = 68, kEndRenderPass = 69, kSetPipelineState1 = 75,          // List4
    kRSSetShadingRate = 77, kRSSetShadingRateImage = 78,                           // List5
    kOMSetFrontAndBackStencilRef = 81,                                             // List8
    kRSSetDepthBias = 82, kIASetIndexBufferStripCutValue = 83,                     // List9
    kMaxSlots = 86,                                                                // List10
    kQueueExecute = 10,   // ID3D12CommandQueue::ExecuteCommandLists
};

// All by value: the build's d3d12.h (mingw) may stop at ID3D12GraphicsCommandList2.
const GUID kIIDList1 = {0x553103fb, 0x1fe7, 0x4557, {0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7}};
const GUID kIIDList2 = {0x38c3e585, 0xff17, 0x412c, {0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28}};
const GUID kIIDList3 = {0x6fda83a7, 0xb84c, 0x4e38, {0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d}};
const GUID kIIDList4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
const GUID kIIDList5 = {0x55050859, 0x4024, 0x474c, {0x87, 0xf5, 0x64, 0x72, 0xea, 0xee, 0x44, 0xea}};
const GUID kIIDList6 = {0xc3827890, 0xe548, 0x4cfa, {0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80}};
const GUID kIIDList7 = {0xdd171223, 0x8b61, 0x4769, {0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1}};
const GUID kIIDList8 = {0xee936ef9, 0x599d, 0x4d28, {0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51}};
const GUID kIIDList9 = {0x34ed2808, 0xffe6, 0x4c2b, {0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1}};
const GUID kIIDList10 = {0x7013c015, 0xd161, 0x4b63, {0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a, 0xcc}};

// Types of the newer methods, as plain values (the enums are UINT-sized, the state object an
// interface pointer passed through untouched).
using StateObject = IUnknown;
using ShadingRate = UINT;
using ShadingRateCombiner = UINT;
using StripCut = UINT;

unsigned slot_count(ID3D12GraphicsCommandList* list) {
    struct { const GUID* iid; unsigned slots; } versions[] = {
        {&kIIDList10, 86}, {&kIIDList9, 84}, {&kIIDList8, 82}, {&kIIDList7, 81}, {&kIIDList6, 80},
        {&kIIDList5, 79}, {&kIIDList4, 77}, {&kIIDList3, 68}, {&kIIDList2, 67}, {&kIIDList1, 66},
    };
    for (const auto& v : versions) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(list->QueryInterface(*v.iid, reinterpret_cast<void**>(&p))) && p) {
            p->Release();
            return v.slots;
        }
    }
    return 60;
}

// ---- the redirect table the stubs read ----------------------------------------------------------
// `active` is the number of live pairs; the stubs scan only while it is non-zero. A pair is written
// `to` first and cleared `from` first, so a reader never pairs a list with a stale continuation.
// Bit 0 of `to` marks a continuation of another class than the list (kForeign): it is called through
// its own vtable, never through the list's originals.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)   // padded to its alignment, which the stubs' offsets assume
#endif
struct alignas(16) RedirectTable {
    volatile int32_t active;
    int32_t pad;
    struct Pair { void* volatile from; void* volatile to; } pairs[16];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
RedirectTable g_rt{};
constexpr uintptr_t kForeign = 1;

void* raw_target_of(void* self) {
    if (!g_rt.active) return self;
    for (auto& p : g_rt.pairs)
        if (p.from == self) return p.to;
    return self;
}
void* target_of(void* self) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(raw_target_of(self)) & ~kForeign);
}

// Per-slot stub: if `this` (rcx) is a list with an active cut, replace it with the continuation;
// then jump to the slot's original, or for a continuation of another class to the slot of its own
// vtable. Touches only rax, r10, r11 and flags (volatile, not argument registers in the Windows x64
// convention) and never the stack.
//
//   0  48 B8 <imm64 &g_rt>        mov  rax, table
//  10  83 38 00                   cmp  dword [rax], 0
//  13  74 2E                      je   orig
//  15  4C 8D 50 08                lea  r10, [rax+8]
//  19  41 BB 10 00 00 00          mov  r11d, 16
//  25  49 3B 0A          scan:    cmp  rcx, [r10]
//  28  74 0B                      je   hit
//  30  49 83 C2 10                add  r10, 16
//  34  41 FF CB                   dec  r11d
//  37  75 F2                      jne  scan
//  39  EB 14                      jmp  orig
//  41  49 8B 4A 08       hit:     mov  rcx, [r10+8]
//  45  48 0F BA F1 00             btr  rcx, 0
//  50  73 09                      jnc  orig
//  52  48 8B 01                   mov  rax, [rcx]
//  55  FF A0 <disp32 slot*8>      jmp  [rax+slot*8]
//  61  FF 25 00 00 00 00 orig:    jmp  [rip+0]
//  67  <imm64 original>
constexpr size_t kStubSize = 80;
void write_stub(uint8_t* p, const void* table, const void* original, unsigned slot) {
    static const uint8_t code[] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x83, 0x38, 0x00,
        0x74, 0x2E,
        0x4C, 0x8D, 0x50, 0x08,
        0x41, 0xBB, 0x10, 0x00, 0x00, 0x00,
        0x49, 0x3B, 0x0A,
        0x74, 0x0B,
        0x49, 0x83, 0xC2, 0x10,
        0x41, 0xFF, 0xCB,
        0x75, 0xF2,
        0xEB, 0x14,
        0x49, 0x8B, 0x4A, 0x08,
        0x48, 0x0F, 0xBA, 0xF1, 0x00,
        0x73, 0x09,
        0x48, 0x8B, 0x01,
        0xFF, 0xA0, 0, 0, 0, 0,
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    static_assert(sizeof code == 75, "stub layout");
    std::memset(p, 0xCC, kStubSize);
    std::memcpy(p, code, sizeof code);
    const uint64_t t = reinterpret_cast<uint64_t>(table), o = reinterpret_cast<uint64_t>(original);
    const uint32_t d = slot * 8;
    std::memcpy(p + 2, &t, 8);
    std::memcpy(p + 57, &d, 4);
    std::memcpy(p + 67, &o, 8);
}

// A class is a set of methods: the originals of one vtable. A process can hold many vtables with the
// same methods - something that hooks per object gives every list its own copy of the vtable (seen in
// a D3D12 game with a heap vtable per command list) - and those share one class, its hooks and stubs.
struct ListClass {
    void** vtable{};   // the first vtable seen with these methods
    unsigned slots{};
    void* orig[kMaxSlots]{};
    void* entry[kMaxSlots]{};   // what a hooked vtable holds: hooks and stubs
    uint8_t* stubs{};
};
ListClass* g_classes[32]{};
std::atomic<int> g_class_count{0};

// Every hooked vtable -> its class. Open addressing, entries never removed; written under g_mutex,
// read without a lock (the class first, then the key with release).
constexpr size_t kVtables = 8192;
struct VtEntry {
    std::atomic<void**> vt{nullptr};
    ListClass* cls{};
};
VtEntry g_vtables[kVtables];
std::atomic<int> g_vtable_count{0};

size_t vt_hash(void** vt) {
    return size_t((reinterpret_cast<uintptr_t>(vt) >> 3) * 0x9E3779B97F4A7C15ull >> 51) & (kVtables - 1);
}
ListClass* class_by_vtable(void** vt) {
    for (size_t i = vt_hash(vt), n = 0; n < kVtables; i = (i + 1) & (kVtables - 1), ++n) {
        void** k = g_vtables[i].vt.load(std::memory_order_acquire);
        if (k == vt) return g_vtables[i].cls;
        if (!k) return nullptr;
    }
    return nullptr;
}
bool register_vtable(void** vt, ListClass* cls) {
    if (g_vtable_count.load() >= int(kVtables * 3 / 4)) return false;
    for (size_t i = vt_hash(vt);; i = (i + 1) & (kVtables - 1)) {
        void** k = g_vtables[i].vt.load(std::memory_order_relaxed);
        if (k == vt) { g_vtables[i].cls = cls; return true; }
        if (k) continue;
        g_vtables[i].cls = cls;
        g_vtables[i].vt.store(vt, std::memory_order_release);
        ++g_vtable_count;
        return true;
    }
}

ListClass* class_of(void* self) {
    if (ListClass* cls = class_by_vtable(*static_cast<void***>(self))) return cls;
    // Called through a vtable that copied our hook from a patched one: the newest class is the only
    // answer that can be right.
    const int n = g_class_count.load(std::memory_order_acquire);
    return n ? g_classes[n - 1] : nullptr;
}

template <class F> F orig_fn(void* self, unsigned slot) {
    return reinterpret_cast<F>(class_of(self)->orig[slot]);
}

// A method of a list that is not the game's: a continuation. Through our record of its class when its
// vtable is one we hooked, else through its own vtable.
void* own_fn(void* list, unsigned slot) {
    void** vt = *static_cast<void***>(list);
    if (ListClass* cls = class_by_vtable(vt)) return cls->orig[slot];
    return vt[slot];
}
unsigned cont_slots(ID3D12GraphicsCommandList* list) {
    if (ListClass* cls = class_by_vtable(*reinterpret_cast<void***>(list))) return cls->slots;
    return slot_count(list);
}

// For a hook forwarding a call to where it belongs: the list itself or its continuation. Picks the
// method and leaves the object in tl_forward for NR_TARGET in the same call: C++17 evaluates the
// called function before its arguments.
thread_local void* tl_forward = nullptr;
template <class F> F fwd_fn(void* self, unsigned slot) {
    const uintptr_t to = reinterpret_cast<uintptr_t>(raw_target_of(self));
    tl_forward = reinterpret_cast<void*>(to & ~kForeign);
    if (to & kForeign) return reinterpret_cast<F>((*static_cast<void***>(tl_forward))[slot]);
    return reinterpret_cast<F>(class_of(self)->orig[slot]);
}

// ---- the state a list has been given since its last Reset ---------------------------------------
struct RootArgs {
    ID3D12RootSignature* rs{};
    bool rs_set{};
    uint64_t mask{};
    uint8_t kind[64]{};   // 1 table, 2 CBV, 3 SRV, 4 UAV
    uint64_t value[64]{};
    std::vector<std::pair<uint32_t, uint32_t>> constants;   // (parameter << 16 | offset, value)
    void set_rs(ID3D12RootSignature* r) {
        // A different root signature invalidates every argument; the same one keeps them.
        if (!rs_set || r != rs) { mask = 0; constants.clear(); }
        rs = r;
        rs_set = true;
    }
    void set(UINT index, uint8_t k, uint64_t v) {
        if (index >= 64) return;
        mask |= 1ull << index;
        kind[index] = k;
        value[index] = v;
    }
    void constant(UINT index, UINT offset, uint32_t v) {
        const uint32_t key = (index << 16) | (offset & 0xFFFF);
        for (auto& c : constants) if (c.first == key) { c.second = v; return; }
        constants.emplace_back(key, v);
    }
};

struct ListState {
    bool valid{};
    UINT heap_count{};
    ID3D12DescriptorHeap* heaps[2]{};
    RootArgs root[2];   // 0 compute, 1 graphics
    ID3D12PipelineState* pso{};
    bool pso_set{};
    StateObject* state_object{};
    bool so_set{}, so_last{};
    D3D12_PRIMITIVE_TOPOLOGY topology{};
    bool topology_set{};
    D3D12_INDEX_BUFFER_VIEW ib{};
    bool ib_set{}, ib_null{};
    D3D12_VERTEX_BUFFER_VIEW vb[32]{};
    uint32_t vb_mask{};
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so[4]{};
    uint32_t so_mask{};
    UINT rt_count{};
    D3D12_CPU_DESCRIPTOR_HANDLE rt[8]{};
    BOOL rt_single{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool rt_set{}, dsv_set{};
    UINT viewport_count{};
    D3D12_VIEWPORT viewports[16]{};
    bool viewports_set{};
    UINT scissor_count{};
    D3D12_RECT scissors[16]{};
    bool scissors_set{};
    float blend[4]{};
    bool blend_set{};
    uint8_t stencil_mode{};   // 1 OMSetStencilRef, 2 OMSetFrontAndBackStencilRef
    UINT stencil_front{}, stencil_back{};
    float depth_bounds[2]{};
    bool depth_bounds_set{};
    UINT sample_count{}, sample_pixels{};
    std::vector<D3D12_SAMPLE_POSITION> samples;
    bool samples_set{};
    UINT view_instance_mask{};
    bool view_instance_set{};
    ShadingRate shading_rate{};
    ShadingRateCombiner combiners[2]{};
    bool shading_rate_set{}, combiners_set{};
    ID3D12Resource* shading_rate_image{};
    bool shading_rate_image_set{};
    float depth_bias[3]{};
    bool depth_bias_set{};
    StripCut strip_cut{};
    bool strip_cut_set{};
    ID3D12Resource* predicate{};
    UINT64 predicate_offset{};
    D3D12_PREDICATION_OP predicate_op{};
    bool predicate_set{};
    // What a cut must not split: queries begun and not ended, a render pass begun and not ended or
    // suspended and not yet resumed, and a bundle whose state we could not follow.
    int open_queries{};
    bool in_render_pass{}, pass_suspended{}, unknown{};
    // The list's own copies of the render-target and depth-stencil descriptors it bound (D3D12 reads
    // the descriptors when OMSetRenderTargets is recorded; the game may rewrite its slots afterwards).
    // Belongs to the list, not to a recording: kept across Reset.
    int rt_block{-1};   // -1 none yet, -2 none available

    void reset(ID3D12PipelineState* initial) {
        // Keep the vectors' storage and the descriptor block; everything else back to a fresh list.
        auto keep0 = std::move(root[0].constants), keep1 = std::move(root[1].constants);
        auto keep_samples = std::move(samples);
        const int block = rt_block;
        *this = ListState{};
        keep0.clear(); keep1.clear(); keep_samples.clear();
        root[0].constants = std::move(keep0);
        root[1].constants = std::move(keep1);
        samples = std::move(keep_samples);
        rt_block = block;
        valid = true;
        if (initial) { pso = initial; pso_set = true; }
    }
};

// Render-target / depth-stencil descriptor blocks, one per list, in CPU heaps of the list's device.
struct RtPool {
    ID3D12Device* device{};
    ID3D12DescriptorHeap* rtv{};
    ID3D12DescriptorHeap* dsv{};
    UINT rtv_inc{}, dsv_inc{};
    int next{};
};
constexpr int kRtBlocks = 2048;
std::mutex g_rt_mutex;
RtPool g_rt_pools[8];   // fixed, so a reader needs no lock once its block was handed out
int g_rt_pool_count = 0;
// The pool index and block for `list` (block = pool * kRtBlocks + n), or -2 when there is none.
int rt_block_for(ID3D12GraphicsCommandList* list) {
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) return -2;
    std::lock_guard<std::mutex> guard(g_rt_mutex);
    int pool = -1;
    for (int i = 0; i < g_rt_pool_count; ++i) if (g_rt_pools[i].device == device) pool = i;
    if (pool < 0 && g_rt_pool_count == 8) { device->Release(); return -2; }
    if (pool < 0) {
        RtPool p{};
        p.device = device;   // the reference is kept: the heaps belong to it
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 8 * kRtBlocks;
        HRESULT hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtv));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = kRtBlocks;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.dsv));
        if (FAILED(hr)) {
            if (p.rtv) p.rtv->Release();
            device->Release();
            static bool said = false;
            if (!said) { said = true; note("no descriptor heap for render-target copies; bound targets are replayed by handle"); }
            return -2;
        }
        p.rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        p.dsv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        g_rt_pools[g_rt_pool_count] = p;
        pool = g_rt_pool_count++;
    } else {
        device->Release();
    }
    RtPool& p = g_rt_pools[size_t(pool)];
    if (p.next >= kRtBlocks) {
        static bool said = false;
        if (!said) { said = true; note("more than %d command lists bind render targets; the rest are replayed by handle", kRtBlocks); }
        return -2;
    }
    return pool * kRtBlocks + p.next++;
}
D3D12_CPU_DESCRIPTOR_HANDLE rt_slot(int block, UINT i) {
    RtPool& p = g_rt_pools[size_t(block / kRtBlocks)];
    D3D12_CPU_DESCRIPTOR_HANDLE h = p.rtv->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(block % kRtBlocks * 8 + int(i)) * p.rtv_inc;
    return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE ds_slot(int block) {
    RtPool& p = g_rt_pools[size_t(block / kRtBlocks)];
    D3D12_CPU_DESCRIPTOR_HANDLE h = p.dsv->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(block % kRtBlocks) * p.dsv_inc;
    return h;
}

std::mutex g_state_mutex;
std::unordered_map<void*, std::unique_ptr<ListState>> g_states;   // entries are never freed
thread_local void* tl_list = nullptr;
thread_local ListState* tl_state = nullptr;

ListState* state_of(void* self) {
    if (tl_list == self) return tl_state;
    std::lock_guard<std::mutex> guard(g_state_mutex);
    auto& p = g_states[self];
    if (!p) p = std::make_unique<ListState>();
    tl_list = self;
    tl_state = p.get();
    return tl_state;
}

using List = ID3D12GraphicsCommandList;

// ---- cuts -------------------------------------------------------------------------------------------
struct Cont {
    ID3D12Device* device{};
    D3D12_COMMAND_LIST_TYPE type{};
    ID3D12CommandAllocator* allocator{};
    List* list{};
    ID3D12Fence* fence{};   // signalled after the expansion that ran it
    uint64_t value{};
    // Held: part of an executed list that may be executed again (until its Reset or Release).
    enum { Free, Recording, InFlight, Held } state{Free};
};
struct Segment {
    Job job;
    Cont* cont{};
};
struct Cut {
    std::vector<Segment> segments;
};

std::recursive_mutex g_mutex;   // cuts, continuations, patching (a Release inside a cut can come back in)
std::unordered_map<void*, Cut> g_cuts;
std::atomic<int> g_pending{0};
// Lists executed with their cut: the continuations stay theirs until the list is reset or released, so
// executing the same closed list again runs all of it (the network is not run again: the first half's
// copy in and the continuation's copy out give the frame back as it came).
std::unordered_map<void*, std::vector<Cont*>> g_held;
std::atomic<int> g_held_count{0};
// Continuations of a list that was reset or released: finished once their last execution is.
void unhold(void* self) {
    auto it = g_held.find(self);
    if (it == g_held.end()) return;
    for (Cont* c : it->second) c->state = Cont::InFlight;
    g_held.erase(it);
    --g_held_count;
}
std::vector<std::unique_ptr<Cont>> g_conts;
bool g_disabled{};
std::string g_disabled_why;

struct QueueClass {
    void** vtable{};
    void* execute{};
};
// Read by the hook without the lock: a fixed table published by its count.
QueueClass g_queue_classes[8];
std::atomic<int> g_queue_count{0};
struct QueueFence {
    ID3D12CommandQueue* queue{};
    ID3D12Fence* fence{};
    uint64_t value{};
};
std::vector<QueueFence> g_queue_fences;

void set_pair(void* from, void* to) {
    for (auto& p : g_rt.pairs)
        if (p.from == from) { p.to = to; return; }
    for (auto& p : g_rt.pairs)
        if (!p.from) {
            p.to = to;
            std::atomic_thread_fence(std::memory_order_release);
            p.from = from;
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_rt.active));
            return;
        }
}
bool pair_free() {
    for (auto& p : g_rt.pairs) if (!p.from) return true;
    return false;
}
void clear_pair(void* from) {
    for (auto& p : g_rt.pairs)
        if (p.from == from) {
            p.from = nullptr;
            std::atomic_thread_fence(std::memory_order_release);
            p.to = nullptr;
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(&g_rt.active));
            return;
        }
}

// The continuations of a cut that will never run: closed and back to the pool, their jobs told.
void abandon(Cut& cut) {
    for (auto& s : cut.segments) {
        if (s.job.discard) s.job.discard();
        if (s.cont) {
            reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(List*)>(own_fn(s.cont->list, kClose))(s.cont->list);
            s.cont->state = Cont::Free;
        }
    }
    cut.segments.clear();
}

void forget_list(void* self) {
    if (g_pending.load(std::memory_order_acquire) || g_held_count.load(std::memory_order_acquire)) {
        std::lock_guard<std::recursive_mutex> guard(g_mutex);
        auto it = g_cuts.find(self);
        if (it != g_cuts.end()) {
            abandon(it->second);
            g_cuts.erase(it);
            clear_pair(self);
            --g_pending;
        }
        unhold(self);
    }
    std::lock_guard<std::mutex> guard(g_state_mutex);
    auto it = g_states.find(self);
    if (it != g_states.end()) it->second->valid = false;
}

bool patch_list_class(List* list, std::string* why);

// ---- the hooks ---------------------------------------------------------------------------------------
#define NR_TARGET(self) static_cast<List*>(tl_forward)

void forget_list(void* self);

ULONG STDMETHODCALLTYPE hk_Release(List* self) {
    const ULONG left = orig_fn<ULONG(STDMETHODCALLTYPE*)(List*)>(self, kRelease)(self);
    // Gone: a new list may be created at this address, and must neither inherit this one's state nor
    // be redirected into this one's continuation.
    if (left == 0) forget_list(self);
    return left;
}

HRESULT STDMETHODCALLTYPE hk_Reset(List* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    if (g_pending.load(std::memory_order_acquire) || g_held_count.load(std::memory_order_acquire)) {
        std::lock_guard<std::recursive_mutex> guard(g_mutex);
        unhold(self);
        auto it = g_cuts.find(self);
        if (it != g_cuts.end()) {
            // Reset with the cut still pending: the list was executed on a queue whose class was not
            // hooked (its continuations never ran), or thrown away. Either way the frame lost the
            // second half once; do not cut again.
            abandon(it->second);
            g_cuts.erase(it);
            clear_pair(self);
            --g_pending;
            if (!g_disabled) {
                g_disabled = true;
                g_disabled_why = "a cut command list was reset without being executed through a hooked queue";
                note("%s; the in-list path is off for this session", g_disabled_why.c_str());
            }
        }
    }
    const HRESULT hr = orig_fn<HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(
        self, kReset)(self, allocator, pso);
    if (SUCCEEDED(hr)) state_of(self)->reset(pso);
    return hr;
}

void STDMETHODCALLTYPE hk_ClearState(List* self, ID3D12PipelineState* pso) {
    ListState* s = state_of(self);
    const bool valid = s->valid;
    s->reset(pso);
    s->valid = valid;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*)>(self, kClearState)(NR_TARGET(self), pso);
}

void STDMETHODCALLTYPE hk_IASetPrimitiveTopology(List* self, D3D12_PRIMITIVE_TOPOLOGY t) {
    ListState* s = state_of(self);
    s->topology = t; s->topology_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, D3D12_PRIMITIVE_TOPOLOGY)>(self, kIASetPrimitiveTopology)(NR_TARGET(self), t);
}

void STDMETHODCALLTYPE hk_RSSetViewports(List* self, UINT n, const D3D12_VIEWPORT* v) {
    ListState* s = state_of(self);
    s->viewport_count = n > 16 ? 16 : n;
    if (v) std::memcpy(s->viewports, v, sizeof(D3D12_VIEWPORT) * s->viewport_count);
    s->viewports_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*)>(self, kRSSetViewports)(NR_TARGET(self), n, v);
}

void STDMETHODCALLTYPE hk_RSSetScissorRects(List* self, UINT n, const D3D12_RECT* r) {
    ListState* s = state_of(self);
    s->scissor_count = n > 16 ? 16 : n;
    if (r) std::memcpy(s->scissors, r, sizeof(D3D12_RECT) * s->scissor_count);
    s->scissors_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RECT*)>(self, kRSSetScissorRects)(NR_TARGET(self), n, r);
}

void STDMETHODCALLTYPE hk_OMSetBlendFactor(List* self, const FLOAT f[4]) {
    ListState* s = state_of(self);
    if (f) std::memcpy(s->blend, f, sizeof s->blend);
    else { s->blend[0] = s->blend[1] = s->blend[2] = s->blend[3] = 1.0f; }
    s->blend_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, const FLOAT*)>(self, kOMSetBlendFactor)(NR_TARGET(self), f);
}

void STDMETHODCALLTYPE hk_OMSetStencilRef(List* self, UINT ref) {
    ListState* s = state_of(self);
    s->stencil_mode = 1; s->stencil_front = s->stencil_back = ref;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT)>(self, kOMSetStencilRef)(NR_TARGET(self), ref);
}

void STDMETHODCALLTYPE hk_SetPipelineState(List* self, ID3D12PipelineState* pso) {
    ListState* s = state_of(self);
    s->pso = pso; s->pso_set = true; s->so_last = false;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*)>(self, kSetPipelineState)(NR_TARGET(self), pso);
}

void STDMETHODCALLTYPE hk_SetDescriptorHeaps(List* self, UINT n, ID3D12DescriptorHeap* const* heaps) {
    ListState* s = state_of(self);
    const UINT count = n > 2 ? 2 : n;
    bool same = count == s->heap_count;
    for (UINT i = 0; same && i < count; ++i) same = s->heaps[i] == (heaps ? heaps[i] : nullptr);
    if (!same) {
        // Other heaps: descriptor tables set against the old ones are no longer valid and are not replayed
        // (the game sets the ones it uses again). Root views and constants are not tied to a heap.
        for (auto& r : s->root)
            for (UINT i = 0; i < 64; ++i)
                if ((r.mask & (1ull << i)) && r.kind[i] == 1) r.mask &= ~(1ull << i);
    }
    s->heap_count = count;
    for (UINT i = 0; i < s->heap_count; ++i) s->heaps[i] = heaps ? heaps[i] : nullptr;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, ID3D12DescriptorHeap* const*)>(self, kSetDescriptorHeaps)(
        NR_TARGET(self), n, heaps);
}

template <unsigned kSlot, int kBind>
void STDMETHODCALLTYPE hk_SetRootSignature(List* self, ID3D12RootSignature* rs) {
    state_of(self)->root[kBind].set_rs(rs);
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12RootSignature*)>(self, kSlot)(NR_TARGET(self), rs);
}

template <unsigned kSlot, int kBind>
void STDMETHODCALLTYPE hk_SetRootDescriptorTable(List* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    state_of(self)->root[kBind].set(index, 1, h.ptr);
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(self, kSlot)(NR_TARGET(self), index, h);
}

template <unsigned kSlot, int kBind>
void STDMETHODCALLTYPE hk_SetRoot32BitConstant(List* self, UINT index, UINT value, UINT offset) {
    state_of(self)->root[kBind].constant(index, offset, value);
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT)>(self, kSlot)(NR_TARGET(self), index, value, offset);
}

template <unsigned kSlot, int kBind>
void STDMETHODCALLTYPE hk_SetRoot32BitConstants(List* self, UINT index, UINT count, const void* data, UINT offset) {
    RootArgs& r = state_of(self)->root[kBind];
    const auto* v = static_cast<const uint32_t*>(data);
    if (v) for (UINT i = 0; i < count; ++i) r.constant(index, offset + i, v[i]);
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const void*, UINT)>(self, kSlot)(NR_TARGET(self), index, count,
                                                                                         data, offset);
}

template <unsigned kSlot, int kBind, uint8_t kKind>
void STDMETHODCALLTYPE hk_SetRootView(List* self, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    state_of(self)->root[kBind].set(index, kKind, va);
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(self, kSlot)(NR_TARGET(self), index, va);
}

void STDMETHODCALLTYPE hk_IASetIndexBuffer(List* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    ListState* s = state_of(self);
    s->ib_set = true; s->ib_null = !view;
    if (view) s->ib = *view;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, const D3D12_INDEX_BUFFER_VIEW*)>(self, kIASetIndexBuffer)(NR_TARGET(self), view);
}

void STDMETHODCALLTYPE hk_IASetVertexBuffers(List* self, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* views) {
    ListState* s = state_of(self);
    for (UINT i = 0; i < n && start + i < 32; ++i) {
        s->vb[start + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
        s->vb_mask |= 1u << (start + i);
    }
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*)>(self, kIASetVertexBuffers)(
        NR_TARGET(self), start, n, views);
}

void STDMETHODCALLTYPE hk_SOSetTargets(List* self, UINT start, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) {
    ListState* s = state_of(self);
    for (UINT i = 0; i < n && start + i < 4; ++i) {
        s->so[start + i] = views ? views[i] : D3D12_STREAM_OUTPUT_BUFFER_VIEW{};
        s->so_mask |= 1u << (start + i);
    }
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*)>(self, kSOSetTargets)(
        NR_TARGET(self), start, n, views);
}

void STDMETHODCALLTYPE hk_OMSetRenderTargets(List* self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    ListState* s = state_of(self);
    s->rt_set = true;
    s->rt_count = n > 8 ? 8 : n;
    s->rt_single = single;
    if (s->rt_block == -1 && ((rts && s->rt_count) || dsv)) s->rt_block = rt_block_for(self);
    if (s->rt_block >= 0) {
        // The descriptors as they are now, into the list's own block: replayed from there.
        const RtPool& p = g_rt_pools[s->rt_block / kRtBlocks];
        if (rts && s->rt_count) {
            if (single) {
                p.device->CopyDescriptorsSimple(s->rt_count, rt_slot(s->rt_block, 0), rts[0], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            } else {
                for (UINT i = 0; i < s->rt_count; ++i)
                    p.device->CopyDescriptorsSimple(1, rt_slot(s->rt_block, i), rts[i], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            }
            for (UINT i = 0; i < s->rt_count; ++i) s->rt[i] = rt_slot(s->rt_block, i);
            s->rt_single = FALSE;
        }
        if (dsv) p.device->CopyDescriptorsSimple(1, ds_slot(s->rt_block), *dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        s->dsv_set = dsv != nullptr;
        if (dsv) s->dsv = ds_slot(s->rt_block);
    } else {
        if (rts) {
            const UINT copy = single ? (s->rt_count ? 1 : 0) : s->rt_count;
            for (UINT i = 0; i < copy; ++i) s->rt[i] = rts[i];
        }
        s->dsv_set = dsv != nullptr;
        if (dsv) s->dsv = *dsv;
    }
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE*)>(self, kOMSetRenderTargets)(NR_TARGET(self), n,
                                                                                                    rts, single, dsv);
}

void STDMETHODCALLTYPE hk_SetPredication(List* self, ID3D12Resource* r, UINT64 offset, D3D12_PREDICATION_OP op) {
    ListState* s = state_of(self);
    s->predicate = r; s->predicate_offset = offset; s->predicate_op = op; s->predicate_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP)>(self, kSetPredication)(
        NR_TARGET(self), r, offset, op);
}

void STDMETHODCALLTYPE hk_BeginQuery(List* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) ++state_of(self)->open_queries;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT)>(self, kBeginQuery)(
        NR_TARGET(self), heap, type, index);
}

void STDMETHODCALLTYPE hk_EndQuery(List* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    ListState* s = state_of(self);
    if (type != D3D12_QUERY_TYPE_TIMESTAMP && s->open_queries > 0) --s->open_queries;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT)>(self, kEndQuery)(
        NR_TARGET(self), heap, type, index);
}

// D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS / RESUMING_PASS (List4).
constexpr UINT kPassSuspending = 0x2, kPassResuming = 0x4;
void STDMETHODCALLTYPE hk_BeginRenderPass(List* self, UINT n, const void* rts, const void* ds, UINT flags) {
    ListState* s = state_of(self);
    s->in_render_pass = true;
    s->pass_suspended = (flags & kPassSuspending) != 0;   // ends suspended: nothing may come between it and its resume
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, const void*, const void*, UINT)>(self, kBeginRenderPass)(
        NR_TARGET(self), n, rts, ds, flags);
}

void STDMETHODCALLTYPE hk_EndRenderPass(List* self) {
    state_of(self)->in_render_pass = false;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*)>(self, kEndRenderPass)(NR_TARGET(self));
}

void STDMETHODCALLTYPE hk_ExecuteBundle(List* self, ID3D12GraphicsCommandList* bundle) {
    // What the bundle set stays set in the list afterwards: taken over into the list's state. A bundle
    // whose state was not followed (recorded before the hooks, or of another class) makes the list's
    // state unknown until its next Reset.
    ListState* s = state_of(self);
    ListState b;
    {
        ListState* bs = bundle ? state_of(bundle) : nullptr;
        if (bs) b = *bs;
        // state_of caches one list per thread: point it back at the list being recorded.
        state_of(self);
    }
    if (!bundle || !b.valid) {
        s->unknown = true;
        // A bundle class of its own: hooked from now on, so the next recording of it is followed.
        if (bundle) {
            std::lock_guard<std::recursive_mutex> guard(g_mutex);
            std::string why;
            patch_list_class(bundle, &why);
        }
    } else {
        for (int k = 0; k < 2; ++k) {
            const RootArgs& r = b.root[k];
            if (r.rs_set) s->root[k].set_rs(r.rs);
            for (UINT i = 0; i < 64; ++i)
                if (r.mask & (1ull << i)) s->root[k].set(i, r.kind[i], r.value[i]);
            for (const auto& c : r.constants) s->root[k].constant(c.first >> 16, c.first & 0xFFFF, c.second);
        }
        if (b.pso_set) { s->pso = b.pso; s->pso_set = true; s->so_last = false; }
        if (b.so_set) { s->state_object = b.state_object; s->so_set = true; s->so_last = b.so_last; }
        if (b.topology_set) { s->topology = b.topology; s->topology_set = true; }
        if (b.ib_set) { s->ib = b.ib; s->ib_null = b.ib_null; s->ib_set = true; }
        for (UINT i = 0; i < 32; ++i)
            if (b.vb_mask & (1u << i)) { s->vb[i] = b.vb[i]; s->vb_mask |= 1u << i; }
        if (b.unknown) s->unknown = true;
    }
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12GraphicsCommandList*)>(self, kExecuteBundle)(NR_TARGET(self), bundle);
}

void STDMETHODCALLTYPE hk_OMSetDepthBounds(List* self, FLOAT lo, FLOAT hi) {
    ListState* s = state_of(self);
    s->depth_bounds[0] = lo; s->depth_bounds[1] = hi; s->depth_bounds_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, FLOAT, FLOAT)>(self, kOMSetDepthBounds)(NR_TARGET(self), lo, hi);
}

void STDMETHODCALLTYPE hk_SetSamplePositions(List* self, UINT per_pixel, UINT pixels, D3D12_SAMPLE_POSITION* pos) {
    ListState* s = state_of(self);
    s->sample_count = per_pixel; s->sample_pixels = pixels; s->samples_set = true;
    s->samples.clear();
    if (pos && per_pixel) s->samples.assign(pos, pos + size_t(per_pixel) * (pixels ? pixels : 1));
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, D3D12_SAMPLE_POSITION*)>(self, kSetSamplePositions)(
        NR_TARGET(self), per_pixel, pixels, pos);
}

void STDMETHODCALLTYPE hk_SetViewInstanceMask(List* self, UINT mask) {
    ListState* s = state_of(self);
    s->view_instance_mask = mask; s->view_instance_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT)>(self, kSetViewInstanceMask)(NR_TARGET(self), mask);
}

void STDMETHODCALLTYPE hk_SetPipelineState1(List* self, StateObject* so) {
    ListState* s = state_of(self);
    s->state_object = so; s->so_set = true; s->so_last = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, StateObject*)>(self, kSetPipelineState1)(NR_TARGET(self), so);
}

void STDMETHODCALLTYPE hk_RSSetShadingRate(List* self, ShadingRate rate, const ShadingRateCombiner* c) {
    ListState* s = state_of(self);
    s->shading_rate = rate; s->shading_rate_set = true; s->combiners_set = c != nullptr;
    if (c) { s->combiners[0] = c[0]; s->combiners[1] = c[1]; }
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ShadingRate, const ShadingRateCombiner*)>(
        self, kRSSetShadingRate)(NR_TARGET(self), rate, c);
}

void STDMETHODCALLTYPE hk_RSSetShadingRateImage(List* self, ID3D12Resource* image) {
    ListState* s = state_of(self);
    s->shading_rate_image = image; s->shading_rate_image_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*)>(self, kRSSetShadingRateImage)(NR_TARGET(self), image);
}

void STDMETHODCALLTYPE hk_OMSetFrontAndBackStencilRef(List* self, UINT front, UINT back) {
    ListState* s = state_of(self);
    s->stencil_mode = 2; s->stencil_front = front; s->stencil_back = back;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, UINT, UINT)>(self, kOMSetFrontAndBackStencilRef)(NR_TARGET(self), front, back);
}

void STDMETHODCALLTYPE hk_RSSetDepthBias(List* self, FLOAT bias, FLOAT clamp, FLOAT slope) {
    ListState* s = state_of(self);
    s->depth_bias[0] = bias; s->depth_bias[1] = clamp; s->depth_bias[2] = slope; s->depth_bias_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, FLOAT, FLOAT, FLOAT)>(self, kRSSetDepthBias)(NR_TARGET(self), bias, clamp,
                                                                                         slope);
}

void STDMETHODCALLTYPE hk_IASetIndexBufferStripCutValue(List* self, StripCut v) {
    ListState* s = state_of(self);
    s->strip_cut = v; s->strip_cut_set = true;
    fwd_fn<void(STDMETHODCALLTYPE*)(List*, StripCut)>(self, kIASetIndexBufferStripCutValue)(
        NR_TARGET(self), v);
}

// The continuation gets the state the list had when the evaluate began, through the originals (it is
// not tracked as anything of the game's).
void apply(const ListState& s, List* c) {
    auto fn = [c](unsigned slot) { return own_fn(c, slot); };
    const unsigned slots = cont_slots(c);
    if (s.heap_count)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, ID3D12DescriptorHeap* const*)>(fn(kSetDescriptorHeaps))(
            c, s.heap_count, s.heaps);
    for (int b = 0; b < 2; ++b) {
        const RootArgs& r = s.root[b];
        if (!r.rs_set) continue;
        const unsigned set_rs = b ? kSetGraphicsRootSignature : kSetComputeRootSignature;
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, ID3D12RootSignature*)>(fn(set_rs))(c, r.rs);
        if (!r.rs) continue;
        for (UINT i = 0; i < 64; ++i) {
            if (!(r.mask & (1ull << i))) continue;
            if (r.kind[i] == 1) {
                const unsigned slot = b ? kSetGraphicsRootDescriptorTable : kSetComputeRootDescriptorTable;
                reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(fn(slot))(
                    c, i, D3D12_GPU_DESCRIPTOR_HANDLE{r.value[i]});
            } else {
                const unsigned base = r.kind[i] == 2 ? kSetComputeRootConstantBufferView
                                    : r.kind[i] == 3 ? kSetComputeRootShaderResourceView
                                                     : kSetComputeRootUnorderedAccessView;
                reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(fn(base + b))(
                    c, i, r.value[i]);
            }
        }
        const unsigned set_const = b ? kSetGraphicsRoot32BitConstant : kSetComputeRoot32BitConstant;
        for (const auto& k : r.constants)
            reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT)>(fn(set_const))(
                c, k.first >> 16, k.second, k.first & 0xFFFF);
    }
    if (s.pso_set && s.pso)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*)>(fn(kSetPipelineState))(c, s.pso);
    if (s.so_set && s.so_last && s.state_object && slots > kSetPipelineState1)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, StateObject*)>(fn(kSetPipelineState1))(c, s.state_object);
    if (s.topology_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, D3D12_PRIMITIVE_TOPOLOGY)>(fn(kIASetPrimitiveTopology))(
            c, s.topology);
    if (s.ib_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, const D3D12_INDEX_BUFFER_VIEW*)>(fn(kIASetIndexBuffer))(
            c, s.ib_null ? nullptr : &s.ib);
    for (UINT i = 0; i < 32; ++i)
        if (s.vb_mask & (1u << i))
            reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*)>(
                fn(kIASetVertexBuffers))(c, i, 1, &s.vb[i]);
    for (UINT i = 0; i < 4; ++i)
        if (s.so_mask & (1u << i))
            reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*)>(
                fn(kSOSetTargets))(c, i, 1, &s.so[i]);
    if (s.rt_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                  const D3D12_CPU_DESCRIPTOR_HANDLE*)>(fn(kOMSetRenderTargets))(
            c, s.rt_count, s.rt_count ? s.rt : nullptr, s.rt_single, s.dsv_set ? &s.dsv : nullptr);
    if (s.viewports_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*)>(fn(kRSSetViewports))(
            c, s.viewport_count, s.viewports);
    if (s.scissors_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RECT*)>(fn(kRSSetScissorRects))(
            c, s.scissor_count, s.scissors);
    if (s.blend_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, const FLOAT*)>(fn(kOMSetBlendFactor))(c, s.blend);
    if (s.stencil_mode == 1)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT)>(fn(kOMSetStencilRef))(c, s.stencil_front);
    if (s.stencil_mode == 2 && slots > kOMSetFrontAndBackStencilRef)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, UINT)>(fn(kOMSetFrontAndBackStencilRef))(
            c, s.stencil_front, s.stencil_back);
    if (s.depth_bounds_set && slots > kOMSetDepthBounds)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, FLOAT, FLOAT)>(fn(kOMSetDepthBounds))(
            c, s.depth_bounds[0], s.depth_bounds[1]);
    if (s.samples_set && slots > kSetSamplePositions) {
        auto samples = s.samples;
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT, UINT, D3D12_SAMPLE_POSITION*)>(fn(kSetSamplePositions))(
            c, s.sample_count, s.sample_pixels, samples.empty() ? nullptr : samples.data());
    }
    if (s.view_instance_set && slots > kSetViewInstanceMask)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, UINT)>(fn(kSetViewInstanceMask))(c, s.view_instance_mask);
    if (s.shading_rate_set && slots > kRSSetShadingRate)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, ShadingRate, const ShadingRateCombiner*)>(
            fn(kRSSetShadingRate))(c, s.shading_rate, s.combiners_set ? s.combiners : nullptr);
    if (s.shading_rate_image_set && slots > kRSSetShadingRateImage)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*)>(fn(kRSSetShadingRateImage))(
            c, s.shading_rate_image);
    if (s.depth_bias_set && slots > kRSSetDepthBias)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, FLOAT, FLOAT, FLOAT)>(fn(kRSSetDepthBias))(
            c, s.depth_bias[0], s.depth_bias[1], s.depth_bias[2]);
    if (s.strip_cut_set && slots > kIASetIndexBufferStripCutValue)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, StripCut)>(
            fn(kIASetIndexBufferStripCutValue))(c, s.strip_cut);
    if (s.predicate_set)
        reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP)>(
            fn(kSetPredication))(c, s.predicate, s.predicate_offset, s.predicate_op);
}

// ---- ExecuteCommandLists -----------------------------------------------------------------------------
using PFN_Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PFN_Execute execute_of(ID3D12CommandQueue* q) {
    void** vt = *reinterpret_cast<void***>(q);
    const int n = g_queue_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_queue_classes[i].vtable == vt) return reinterpret_cast<PFN_Execute>(g_queue_classes[i].execute);
    return n ? reinterpret_cast<PFN_Execute>(g_queue_classes[n - 1].execute) : nullptr;
}

QueueFence* completion_fence(ID3D12CommandQueue* q) {
    for (auto& f : g_queue_fences) if (f.queue == q) return &f;
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&device))) && device) {
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        device->Release();
    }
    if (!fence) return nullptr;
    q->AddRef();   // held, so a later queue at the same address is never taken for this one
    g_queue_fences.push_back({q, fence, 0});
    return &g_queue_fences.back();
}

void STDMETHODCALLTYPE hk_Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    // Patching is under g_mutex; the class table only grows, so a lookup outside it is safe once the
    // hook it led here through exists.
    const PFN_Execute execute = execute_of(q);
    if (g_pending.load(std::memory_order_acquire) == 0 && g_held_count.load(std::memory_order_acquire) == 0)
        return execute(q, n, lists);
    // The cuts of these lists are taken out under the lock; the lists are executed outside it, so no
    // lock of ours is held across the runtime's (or an overlay's) ExecuteCommandLists or the driver.
    struct Expand { UINT index; Cut cut; std::vector<Cont*> again; };
    std::vector<Expand> work;
    {
        std::lock_guard<std::recursive_mutex> guard(g_mutex);
        for (UINT i = 0; i < n; ++i) {
            auto it = g_cuts.find(lists[i]);
            if (it != g_cuts.end()) {
                work.push_back({i, std::move(it->second), {}});
                g_cuts.erase(it);
                clear_pair(lists[i]);
                --g_pending;
                continue;
            }
            auto held = g_held.find(lists[i]);
            if (held != g_held.end()) work.push_back({i, Cut{}, held->second});
        }
    }
    if (work.empty()) return execute(q, n, lists);
    static bool said_again = false;
    std::vector<ID3D12CommandList*> batch;
    std::vector<Cont*> ran;
    size_t next = 0;
    for (UINT i = 0; i < n; ++i) {
        batch.push_back(lists[i]);
        if (next >= work.size() || work[next].index != i) continue;
        auto& w = work[next++];
        for (auto& seg : w.cut.segments) {
            execute(q, UINT(batch.size()), batch.data());
            batch.clear();
            seg.job.run(q);
            batch.push_back(seg.cont->list);
            ran.push_back(seg.cont);
        }
        if (!w.again.empty() && !said_again) {
            said_again = true;
            note("a cut command list was executed again; it runs whole, without the network");
        }
        for (Cont* c : w.again) { batch.push_back(c->list); ran.push_back(c); }
    }
    if (!batch.empty()) execute(q, UINT(batch.size()), batch.data());
    std::lock_guard<std::recursive_mutex> guard(g_mutex);
    QueueFence* f = completion_fence(q);
    const bool signalled = f && SUCCEEDED(q->Signal(f->fence, f->value + 1));
    if (signalled) ++f->value;
    for (Cont* c : ran) {
        // Without a completion mark a continuation is never handed out again.
        c->fence = signalled ? f->fence : nullptr;
        c->value = signalled ? f->value : ~0ull;
        if (c->state != Cont::Held) c->state = Cont::InFlight;
    }
    // The continuations of a list executed with its cut stay with it until it is reset or released.
    for (auto& w : work) {
        if (w.cut.segments.empty()) continue;
        void* self = lists[w.index];
        auto& held = g_held[self];
        if (held.empty()) ++g_held_count;
        for (auto& seg : w.cut.segments) { seg.cont->state = Cont::Held; held.push_back(seg.cont); }
    }
}

// ---- patching ----------------------------------------------------------------------------------------
bool write_vtable(void** vt, unsigned slot, void* fn) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    vt[slot] = fn;
    DWORD ignored = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &ignored);
    return true;
}

// Where an address lives, for the log: a module's file name, or private memory (a heap).
std::string where_is(const void* p) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<const char*>(p), &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        const char* base = std::strrchr(path, '\\');
        return base ? base + 1 : path;
    }
    return "private memory";
}

bool hook_vtable(void** vt, const ListClass* cls, std::string* why) {
    for (unsigned i = 0; i < cls->slots; ++i)
        if (cls->entry[i] && !write_vtable(vt, i, cls->entry[i])) {
            *why = "the command list vtable could not be written";
            note("%s (slot %u)", why->c_str(), i);
            return false;
        }
    return true;
}

// Another vtable of a class we have: registered, and hooked unless it already holds the hooks.
bool add_vtable(void** vt, ListClass* cls, bool hooked, std::string* why) {
    if (!register_vtable(vt, cls)) { *why = "too many command list vtables"; return false; }
    if (!hooked && !hook_vtable(vt, cls, why)) return false;
    static int copies = 0;
    ++copies;
    if (copies <= 2 || (copies & (copies - 1)) == 0)
        note("command list vtable %p (%s) has the methods of class %p, hooked alike (%d such vtables)",
            static_cast<void*>(vt), where_is(vt).c_str(), static_cast<void*>(cls->vtable), copies);
    return true;
}

bool patch_list_class(List* list, std::string* why) {
    void** vt = *reinterpret_cast<void***>(list);
    if (ListClass* known = class_by_vtable(vt)) {
        // Hooked already - unless the vtable holds its class's originals again: a per-object copy that
        // was freed and its memory used for a new copy.
        if (vt[kReset] == known->orig[kReset] && known->orig[kReset] != known->entry[kReset])
            return hook_vtable(vt, known, why);
        return true;
    }
    const int n = g_class_count.load();
    // A copy of a hooked vtable, our hooks copied with it (Close is a stub of exactly one class).
    for (int i = 0; i < n; ++i) {
        const ListClass* c = g_classes[i];
        const auto* p = static_cast<const uint8_t*>(vt[kClose]);
        if (c->slots > kClose && p >= c->stubs && p < c->stubs + kStubSize * kMaxSlots)
            return add_vtable(vt, g_classes[i], true, why);
    }
    // The same methods as a class we have.
    const unsigned slots = slot_count(list);
    for (int i = 0; i < n; ++i) {
        const ListClass* c = g_classes[i];
        bool same = c->slots == slots;
        for (unsigned k = 0; same && k < slots; ++k) same = vt[k] == c->orig[k];
        if (same) return add_vtable(vt, g_classes[i], false, why);
    }
    if (n >= 32) { *why = "too many command list classes"; return false; }
    auto* cls = new ListClass();
    cls->vtable = vt;
    cls->slots = slots;
    for (unsigned i = 0; i < cls->slots; ++i) cls->orig[i] = vt[i];

    void* typed[kMaxSlots] = {};
    typed[kRelease] = reinterpret_cast<void*>(&hk_Release);
    typed[kReset] = reinterpret_cast<void*>(&hk_Reset);
    typed[kClearState] = reinterpret_cast<void*>(&hk_ClearState);
    typed[kIASetPrimitiveTopology] = reinterpret_cast<void*>(&hk_IASetPrimitiveTopology);
    typed[kRSSetViewports] = reinterpret_cast<void*>(&hk_RSSetViewports);
    typed[kRSSetScissorRects] = reinterpret_cast<void*>(&hk_RSSetScissorRects);
    typed[kOMSetBlendFactor] = reinterpret_cast<void*>(&hk_OMSetBlendFactor);
    typed[kOMSetStencilRef] = reinterpret_cast<void*>(&hk_OMSetStencilRef);
    typed[kSetPipelineState] = reinterpret_cast<void*>(&hk_SetPipelineState);
    typed[kSetDescriptorHeaps] = reinterpret_cast<void*>(&hk_SetDescriptorHeaps);
    typed[kSetComputeRootSignature] = reinterpret_cast<void*>(&hk_SetRootSignature<kSetComputeRootSignature, 0>);
    typed[kSetGraphicsRootSignature] = reinterpret_cast<void*>(&hk_SetRootSignature<kSetGraphicsRootSignature, 1>);
    typed[kSetComputeRootDescriptorTable] =
        reinterpret_cast<void*>(&hk_SetRootDescriptorTable<kSetComputeRootDescriptorTable, 0>);
    typed[kSetGraphicsRootDescriptorTable] =
        reinterpret_cast<void*>(&hk_SetRootDescriptorTable<kSetGraphicsRootDescriptorTable, 1>);
    typed[kSetComputeRoot32BitConstant] =
        reinterpret_cast<void*>(&hk_SetRoot32BitConstant<kSetComputeRoot32BitConstant, 0>);
    typed[kSetGraphicsRoot32BitConstant] =
        reinterpret_cast<void*>(&hk_SetRoot32BitConstant<kSetGraphicsRoot32BitConstant, 1>);
    typed[kSetComputeRoot32BitConstants] =
        reinterpret_cast<void*>(&hk_SetRoot32BitConstants<kSetComputeRoot32BitConstants, 0>);
    typed[kSetGraphicsRoot32BitConstants] =
        reinterpret_cast<void*>(&hk_SetRoot32BitConstants<kSetGraphicsRoot32BitConstants, 1>);
    typed[kSetComputeRootConstantBufferView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetComputeRootConstantBufferView, 0, 2>);
    typed[kSetGraphicsRootConstantBufferView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetGraphicsRootConstantBufferView, 1, 2>);
    typed[kSetComputeRootShaderResourceView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetComputeRootShaderResourceView, 0, 3>);
    typed[kSetGraphicsRootShaderResourceView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetGraphicsRootShaderResourceView, 1, 3>);
    typed[kSetComputeRootUnorderedAccessView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetComputeRootUnorderedAccessView, 0, 4>);
    typed[kSetGraphicsRootUnorderedAccessView] =
        reinterpret_cast<void*>(&hk_SetRootView<kSetGraphicsRootUnorderedAccessView, 1, 4>);
    typed[kIASetIndexBuffer] = reinterpret_cast<void*>(&hk_IASetIndexBuffer);
    typed[kIASetVertexBuffers] = reinterpret_cast<void*>(&hk_IASetVertexBuffers);
    typed[kSOSetTargets] = reinterpret_cast<void*>(&hk_SOSetTargets);
    typed[kOMSetRenderTargets] = reinterpret_cast<void*>(&hk_OMSetRenderTargets);
    typed[kSetPredication] = reinterpret_cast<void*>(&hk_SetPredication);
    typed[kBeginQuery] = reinterpret_cast<void*>(&hk_BeginQuery);
    typed[kEndQuery] = reinterpret_cast<void*>(&hk_EndQuery);
    typed[kExecuteBundle] = reinterpret_cast<void*>(&hk_ExecuteBundle);
    typed[kBeginRenderPass] = reinterpret_cast<void*>(&hk_BeginRenderPass);
    typed[kEndRenderPass] = reinterpret_cast<void*>(&hk_EndRenderPass);
    typed[kOMSetDepthBounds] = reinterpret_cast<void*>(&hk_OMSetDepthBounds);
    typed[kSetSamplePositions] = reinterpret_cast<void*>(&hk_SetSamplePositions);
    typed[kSetViewInstanceMask] = reinterpret_cast<void*>(&hk_SetViewInstanceMask);
    typed[kSetPipelineState1] = reinterpret_cast<void*>(&hk_SetPipelineState1);
    typed[kRSSetShadingRate] = reinterpret_cast<void*>(&hk_RSSetShadingRate);
    typed[kRSSetShadingRateImage] = reinterpret_cast<void*>(&hk_RSSetShadingRateImage);
    typed[kOMSetFrontAndBackStencilRef] = reinterpret_cast<void*>(&hk_OMSetFrontAndBackStencilRef);
    typed[kRSSetDepthBias] = reinterpret_cast<void*>(&hk_RSSetDepthBias);
    typed[kIASetIndexBufferStripCutValue] = reinterpret_cast<void*>(&hk_IASetIndexBufferStripCutValue);

    // Every recording method from Close on: typed hooks above, the redirecting stub for the rest.
    // QueryInterface, AddRef, the private data, GetDevice and GetType stay with the list itself.
    auto* stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStubSize * kMaxSlots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!stubs) { delete cls; *why = "no memory for the command list stubs"; return false; }
    for (unsigned i = kClose; i < cls->slots; ++i) {
        if (typed[i]) { cls->entry[i] = typed[i]; continue; }
        write_stub(stubs + kStubSize * i, &g_rt, cls->orig[i], i);
        cls->entry[i] = stubs + kStubSize * i;
    }
    cls->entry[kRelease] = typed[kRelease];
    cls->stubs = stubs;
    DWORD old = 0;
    VirtualProtect(stubs, kStubSize * kMaxSlots, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stubs, kStubSize * kMaxSlots);
    // Published before the vtable points at anything that looks it up.
    if (!register_vtable(vt, cls)) { *why = "too many command list vtables"; return false; }
    g_classes[n] = cls;
    g_class_count.store(n + 1, std::memory_order_release);
    if (!hook_vtable(vt, cls, why)) return false;
    note("command list class %p hooked (%u methods; vtable in %s, methods in %s)", static_cast<void*>(vt),
        cls->slots, where_is(vt).c_str(), where_is(cls->orig[kClose]).c_str());
    return true;
}

// ID3D12Device::CreateCommandList (slot 12): a list made after the hooks starts recording with nothing
// bound, so its state is known from its first call - no Reset needed (a game may make a list per frame).
constexpr unsigned kDeviceCreateCommandList = 12;
struct DeviceClass {
    void** vtable{};
    void* create{};
};
DeviceClass g_device_classes[8];
std::atomic<int> g_device_count{0};

using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                          ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
HRESULT STDMETHODCALLTYPE hk_CreateCommandList(ID3D12Device* d, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                               ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid,
                                               void** out) {
    void** vt = *reinterpret_cast<void***>(d);
    const int n = g_device_count.load(std::memory_order_acquire);
    PFN_CreateCommandList create = nullptr;
    for (int i = 0; i < n && !create; ++i)
        if (g_device_classes[i].vtable == vt) create = reinterpret_cast<PFN_CreateCommandList>(g_device_classes[i].create);
    if (!create && n) create = reinterpret_cast<PFN_CreateCommandList>(g_device_classes[n - 1].create);
    const HRESULT hr = create(d, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            state_of(list)->reset(pso);
            list->Release();
        }
    }
    return hr;
}

bool patch_device_class(ID3D12Device* device, std::string* why) {
    void** vt = *reinterpret_cast<void***>(device);
    const int n = g_device_count.load();
    for (int i = 0; i < n; ++i) if (g_device_classes[i].vtable == vt) return true;
    if (n >= 8) { *why = "too many device classes"; return false; }
    g_device_classes[n] = {vt, vt[kDeviceCreateCommandList]};
    g_device_count.store(n + 1, std::memory_order_release);
    if (!write_vtable(vt, kDeviceCreateCommandList, reinterpret_cast<void*>(&hk_CreateCommandList))) {
        *why = "the device vtable could not be written";
        return false;
    }
    note("device class %p hooked (command lists made from now on are followed from the start)",
        static_cast<void*>(vt));
    return true;
}

bool patch_queue_classes(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, std::string* why) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&q))) || !q) {
        *why = "could not create a probe command queue";
        return false;
    }
    void** vt = *reinterpret_cast<void***>(q);
    bool known = false;
    const int n = g_queue_count.load();
    for (int i = 0; i < n; ++i) known = known || g_queue_classes[i].vtable == vt;
    bool ok = true;
    if (!known && n >= 8) { ok = false; *why = "too many command queue classes"; }
    else if (!known) {
        g_queue_classes[n] = {vt, vt[kQueueExecute]};
        g_queue_count.store(n + 1, std::memory_order_release);
        ok = write_vtable(vt, kQueueExecute, reinterpret_cast<void*>(&hk_Execute));
        if (!ok) *why = "the command queue vtable could not be written";
        else note("command queue class %p hooked", static_cast<void*>(vt));
    }
    q->Release();
    return ok;
}

}  // namespace

struct Snapshot {
    ListState state;
};

bool prepare(List* list, std::string* why) {
    std::lock_guard<std::recursive_mutex> guard(g_mutex);
    if (g_disabled) { *why = g_disabled_why; return false; }
    if (!patch_list_class(list, why)) { g_disabled = true; g_disabled_why = *why; return false; }
    void** vt = *reinterpret_cast<void***>(list);
    static std::vector<void**> queues_done;
    bool done = false;
    for (void** v : queues_done) done = done || v == vt;
    if (!done) {
        ID3D12Device* device = nullptr;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) {
            *why = "the command list has no device";
            return false;
        }
        const bool ok = patch_queue_classes(device, D3D12_COMMAND_LIST_TYPE_DIRECT, why) &&
                        patch_queue_classes(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, why) &&
                        patch_device_class(device, why);
        device->Release();
        if (!ok) { g_disabled = true; g_disabled_why = *why; return false; }
        queues_done.push_back(vt);
    }
    const ListState* st = state_of(list);
    if (!st->valid) {
        *why = "waiting for the command list's next Reset (its state is not known yet)";
        return false;
    }
    // Ranges a cut would split: the frame passes through rather than break them.
    if (st->open_queries > 0) { *why = "a query is open in the command list"; return false; }
    if (st->in_render_pass || st->pass_suspended) { *why = "the command list is inside a render pass"; return false; }
    if (st->predicate_set && st->predicate) { *why = "the command list has a predicate set"; return false; }
    if (st->unknown) { *why = "the command list ran a bundle whose state is not known"; return false; }
    if (!pair_free()) {
        *why = "too many command lists are cut at once";
        return false;
    }
    return true;
}

std::shared_ptr<Snapshot> snapshot(List* list) {
    ListState* s = state_of(list);
    if (!s->valid) return nullptr;
    auto snap = std::make_shared<Snapshot>();
    snap->state = *s;
    return snap;
}

void restore(List* list, const std::shared_ptr<Snapshot>& state) {
    if (!state) return;
    std::lock_guard<std::recursive_mutex> guard(g_mutex);
    ListClass* cls = class_of(list);
    if (!cls || class_by_vtable(*reinterpret_cast<void***>(list)) != cls) return;
    apply(state->state, static_cast<List*>(target_of(list)));
    *state_of(list) = state->state;
}

bool cut(List* list, Job job, const std::shared_ptr<Snapshot>& state, std::string* why) {
    std::lock_guard<std::recursive_mutex> guard(g_mutex);
    if (g_disabled) { *why = g_disabled_why; return false; }
    if (!state) { *why = "no state to continue from"; return false; }
    if (state->state.unknown) { *why = "the command list ran a bundle whose state is not known"; return false; }
    // The redirect needs a slot (this list's own, or a free one) before anything is closed: a closed
    // list without a redirect would swallow the rest of the frame.
    bool slot = pair_free();
    for (auto& p : g_rt.pairs) slot = slot || p.from == list;
    if (!slot) { *why = "too many command lists are cut at once"; return false; }
    ListClass* cls = class_of(list);
    if (!cls || class_by_vtable(*reinterpret_cast<void***>(list)) != cls) {
        *why = "the command list's class is not hooked";
        return false;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) { *why = "the command list has no device"; return false; }
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();

    // A continuation: a finished one of the same device and type, or a new one.
    Cont* cont = nullptr;
    for (auto& c : g_conts) {
        if (c->device != device || c->type != type) continue;
        if (c->state == Cont::InFlight && c->fence) {
            const uint64_t completed = c->fence->GetCompletedValue();
            if (completed == UINT64_MAX) {
                // The device was removed: nothing more is cut.
                device->Release();
                g_disabled = true;
                g_disabled_why = *why = "the game's D3D12 device was removed";
                return false;
            }
            if (completed >= c->value) c->state = Cont::Free;
        }
        if (c->state == Cont::Free) { cont = c.get(); break; }
    }
    if (cont) {
        if (FAILED(cont->allocator->Reset()) ||
            FAILED(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(
                       own_fn(cont->list, kReset))(cont->list, cont->allocator, nullptr))) {
            device->Release();
            *why = "could not reset a continuation command list";
            return false;
        }
    } else {
        auto c = std::make_unique<Cont>();
        c->device = device;
        c->type = type;
        if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&c->allocator))) ||
            FAILED(device->CreateCommandList(0, type, c->allocator, nullptr, IID_PPV_ARGS(&c->list)))) {
            if (c->allocator) c->allocator->Release();
            device->Release();
            *why = "could not create a continuation command list";
            return false;
        }
        device->AddRef();   // held by the continuation for its life
        g_conts.push_back(std::move(c));
        cont = g_conts.back().get();
    }
    device->Release();
    // The list's device may make lists of another class than the game's list: something between the
    // two gave the game's lists vtables of their own. The continuation is then called through its own
    // vtable (kForeign), never through the methods of the list's class.
    void** cont_vt = *reinterpret_cast<void***>(cont->list);
    const bool foreign = class_by_vtable(cont_vt) != cls;
    static bool said_foreign = false;
    if (foreign && !said_foreign) {
        said_foreign = true;
        const unsigned cs = slot_count(cont->list), n = cs < cls->slots ? cs : cls->slots;
        unsigned differ = 0;
        for (unsigned i = 0; i < n; ++i) differ += cont_vt[i] != cls->orig[i];
        note("the continuation is of another class than the list (list vtable %p in %s, methods in %s; "
            "continuation vtable %p in %s; %u of %u methods differ): called through its own vtable",
            static_cast<void*>(*reinterpret_cast<void***>(list)), where_is(*reinterpret_cast<void***>(list)).c_str(),
            where_is(cls->orig[kClose]).c_str(), static_cast<void*>(cont_vt), where_is(cont_vt).c_str(), differ, n);
    }

    // Close the half recorded so far: the list itself on the first cut, the previous continuation on
    // a later one in the same list (one per pass).
    List* current = static_cast<List*>(target_of(list));
    const HRESULT hr = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(List*)>(
        current == list ? cls->orig[kClose] : own_fn(current, kClose))(current);
    if (FAILED(hr)) {
        reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(List*)>(own_fn(cont->list, kClose))(cont->list);
        cont->state = Cont::Free;
        char text[96];
        std::snprintf(text, sizeof text, "the command list would not close (0x%08lX)", (unsigned long)hr);
        *why = text;
        return false;
    }
    cont->state = Cont::Recording;
    apply(state->state, cont->list);
    // What the list holds from here on is what the continuation was given, not what was bound while
    // the evaluate recorded its copies: the next cut in this list starts from that.
    *state_of(list) = state->state;
    auto& c = g_cuts[list];
    if (c.segments.empty()) ++g_pending;
    c.segments.push_back(Segment{std::move(job), cont});
    set_pair(list, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(cont->list) | (foreign ? kForeign : 0)));
    return true;
}

#endif

}  // namespace nr_xess_split

// ---- for C (nr_xess_split.h) ---------------------------------------------------------------------

struct nr_split_snapshot {
    std::shared_ptr<nr_xess_split::Snapshot> state;
};

static int nr_split_why(int ok, const std::string& reason, char* why, size_t size) {
    if (!ok && why && size) std::snprintf(why, size, "%s", reason.c_str());
    return ok;
}

extern "C" void nr_split_set_log(void (*say)(const char* line)) {
    nr_xess_split::g_say = say;
}

extern "C" int nr_split_prepare(ID3D12GraphicsCommandList* list, char* why, size_t why_size) {
    std::string reason;
    return nr_split_why(nr_xess_split::prepare(list, &reason), reason, why, why_size);
}

extern "C" nr_split_snapshot* nr_split_snapshot_take(ID3D12GraphicsCommandList* list) {
    std::shared_ptr<nr_xess_split::Snapshot> state = nr_xess_split::snapshot(list);
    return state ? new nr_split_snapshot{std::move(state)} : nullptr;
}

extern "C" void nr_split_snapshot_free(nr_split_snapshot* snapshot) {
    delete snapshot;
}

extern "C" void nr_split_restore(ID3D12GraphicsCommandList* list, const nr_split_snapshot* snapshot) {
    if (snapshot) nr_xess_split::restore(list, snapshot->state);
}

extern "C" int nr_split_cut(ID3D12GraphicsCommandList* list, nr_split_run run, nr_split_discard discard,
                            void* user, const nr_split_snapshot* snapshot, char* why, size_t why_size) {
    nr_xess_split::Job job;
    job.run = [run, user](ID3D12CommandQueue* queue) { run(user, queue); };
    job.discard = [discard, user] { if (discard) discard(user); };
    std::string reason;
    return nr_split_why(nr_xess_split::cut(list, std::move(job), snapshot ? snapshot->state : nullptr, &reason),
                        reason, why, why_size);
}
