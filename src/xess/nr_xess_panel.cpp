/* nr_xess_panel.cpp - NR's in-game panel in a DirectX 12 game. See nr_xess_panel.h.
 *
 * Where it is drawn. In IDXGISwapChain::Present and Present1, hooked by writing into the swap chain
 * class's vtable in dxgi.dll, as nr_xess_split.cpp writes into D3D12's command list and queue
 * classes. The vtable is found by nr_present_probe.exe in a process of its own, because a swap chain
 * made in the game is seen by whatever else hooks there (DLSSNR-AMD's finding with OptiScaler); its
 * answer is kept beside nr-env.txt and used again while dxgi.dll is the same file. Every swap chain
 * of that class presents through the hook: the game's, and under XeSS frame generation the
 * generator's own, which presents both the frames it makes and the game's - so the panel is on every
 * frame shown, after the game's interface and the generator's work.
 *
 * Which queue. The panel's work has to go on the queue the swap chain presents on, after the frame
 * and before the Present, and DXGI does not say which that is (GetDevice gives the device and
 * refuses ID3D12CommandQueue). The swap chain keeps it in a field the probe finds; what is read there
 * is used only when it is a direct queue nr_xess_split.cpp has seen execute command lists. Failing
 * that, the direct queue this thread last executed on, when it is the swap chain's device's - what
 * DLSSNR-AMD's NR Control does - and failing both, no panel on this Present.
 *
 * What is drawn. The Vulkan layer's panel: nr_panel.c reads the keys behind the focus gate, holds the
 * game's input while the panel is open, builds the rows and the ImGui frame and applies what the
 * user did; nr_menu_ui_d3d12.cpp draws it over the back buffer in its encoding. One panel for the
 * process, whichever swap chain shows it, made on the first Present and made again when the game
 * changes its device. The pacing rows of the Vulkan layer are not offered: a DirectX 12 game's frames
 * are on the GPU, each on its own frame. The network row is, through the daemon's live preview.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "nr_xess_panel.h"
#include "nr_xess_split.h"

#include "../layer/nr_panel.h"
#include "../layer/nr_menu.h"
#include "../layer/nr_menu_bridge.h"
#include "../layer/nr_menu_ui.h"
#include "../layer/nr_menu_font.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

nr_xess_panel_host g_host;

void note(const char* fmt, ...)
{
    if (!g_host.say) return;
    char line[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_host.say(line);
}

template <class T> void drop(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

// ---- where Present is: the probe's answer --------------------------------------------------------

struct Answer {
    wchar_t module[1024] = L"";
    unsigned long stamp = 0, size = 0;
    unsigned long long vtable = 0, present = 0;
    long queue_at = -1;
};

bool read_answer(const wchar_t* path, Answer* a)
{
    *a = Answer();
    FILE* f = path[0] ? _wfopen(path, L"rb") : nullptr;
    if (!f) return false;
    char line[2048];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        char* equals = std::strchr(line, '=');
        if (line[0] == '#' || !equals) continue;
        *equals = '\0';
        const char* v = equals + 1;
        if (!std::strcmp(line, "module"))
            MultiByteToWideChar(CP_UTF8, 0, v, -1, a->module, int(sizeof a->module / sizeof a->module[0]));
        else if (!std::strcmp(line, "stamp")) a->stamp = std::strtoul(v, nullptr, 16);
        else if (!std::strcmp(line, "size")) a->size = std::strtoul(v, nullptr, 16);
        else if (!std::strcmp(line, "vtable")) a->vtable = std::strtoull(v, nullptr, 16);
        else if (!std::strcmp(line, "present")) a->present = std::strtoull(v, nullptr, 16);
        else if (!std::strcmp(line, "queue_at")) a->queue_at = long(std::strtoul(v, nullptr, 16));
    }
    std::fclose(f);
    return a->module[0] && a->vtable && a->size;
}

bool run_probe()
{
    if (GetFileAttributesW(g_host.probe) == INVALID_FILE_ATTRIBUTES) {
        note("there is no %ls, so DXGI's Present cannot be found; no panel in this game", g_host.probe);
        return false;
    }
    std::wstring command = L"\"" + std::wstring(g_host.probe) + L"\" \"" + g_host.answer + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(g_host.probe, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        note("%ls would not start (error %lu); no panel", g_host.probe, GetLastError());
        return false;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) note("the swap-chain probe found nothing (exit %lu); no panel", code);
    return code == 0;
}

/* The module as this process has it, when it is the file the probe read: its timestamp and image
 * size. A Windows update that replaces dxgi.dll moves the vtable, and an answer for the old file
 * must not be written into the new one. */
HMODULE loaded(const Answer& a)
{
    HMODULE m = GetModuleHandleW(a.module);
    if (!m) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(m) + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != a.stamp || nt->OptionalHeader.SizeOfImage != a.size) return nullptr;
    if (a.vtable + 40 * sizeof(void*) > a.size) return nullptr;
    return m;
}

// ---- the hooks -------------------------------------------------------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ColourSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
enum : unsigned { kPresent = 8, kPresent1 = 22, kColourSpace = 38 };
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ColourSpaceFn g_colour_space = nullptr;
long g_queue_at = -1;
std::atomic<bool> g_hooked{false};

/* What each swap chain said its colour space was (SetColorSpace1), for the encoding. */
std::mutex g_spaces_mutex;
std::unordered_map<void*, DXGI_COLOR_SPACE_TYPE> g_spaces;

void draw_on(IDXGISwapChain* sc, UINT flags);

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT interval, UINT flags)
{
    draw_on(sc, flags);
    return g_present(sc, interval, flags);
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* parameters)
{
    draw_on(sc, flags);
    return g_present1(sc, interval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE hk_colour_space(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space)
{
    const HRESULT hr = g_colour_space(sc, space);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        g_spaces[sc] = space;
    }
    return hr;
}

bool write_slot(void** vt, unsigned slot, void* fn)
{
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    vt[slot] = fn;
    DWORD ignored = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &ignored);
    return true;
}

DWORD WINAPI install(LPVOID)
{
    Answer a;
    bool probed = false;
    if (!read_answer(g_host.answer, &a)) {
        if (!run_probe() || !read_answer(g_host.answer, &a)) return 0;
        probed = true;
    }
    /* dxgi.dll may not be in the game yet when XeSS is: wait for it, two minutes at most. */
    HMODULE m = nullptr;
    for (int i = 0; i < 240 && !(m = GetModuleHandleW(a.module)); ++i) Sleep(500);
    if (!m) {
        note("%ls was not loaded within two minutes; no panel", a.module);
        return 0;
    }
    if (!loaded(a)) {
        if (probed || !run_probe() || !read_answer(g_host.answer, &a) || !loaded(a)) {
            note("the swap-chain probe's answer does not fit %ls in this process; no panel", a.module);
            return 0;
        }
    }
    void** vt = reinterpret_cast<void**>(reinterpret_cast<char*>(m) + a.vtable);
    const uintptr_t base = reinterpret_cast<uintptr_t>(m);
    if (a.present && reinterpret_cast<uintptr_t>(vt[kPresent]) != base + a.present)
        note("Present in the swap chain's vtable is already hooked by something else; hooked over it");
    /* The originals first, so a Present that runs between two writes calls what was there. And this
     * module stays loaded from now on: the vtable will point into it. */
    g_present = reinterpret_cast<PresentFn>(vt[kPresent]);
    g_present1 = reinterpret_cast<Present1Fn>(vt[kPresent1]);
    g_colour_space = reinterpret_cast<ColourSpaceFn>(vt[kColourSpace]);
    g_queue_at = a.queue_at;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&hk_present), &self);
    if (!write_slot(vt, kColourSpace, reinterpret_cast<void*>(&hk_colour_space)) ||
        !write_slot(vt, kPresent1, reinterpret_cast<void*>(&hk_present1)) ||
        !write_slot(vt, kPresent, reinterpret_cast<void*>(&hk_present))) {
        note("the swap chain's vtable could not be written; no panel");
        return 0;
    }
    g_hooked = true;
    note("INS opens the panel: DXGI's Present hooked (%ls +0x%llx, the queue %s)", a.module, a.vtable,
         a.queue_at >= 0 ? "read from the swap chain" : "the one each Present's thread executed on last");
    return 0;
}

// ---- the panel -----------------------------------------------------------------------------------

struct Overlay {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;          // the queue its last work went on
    ID3D12CommandAllocator* allocators[NREX_D3D12_FRAMES] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0, done_at[NREX_D3D12_FRAMES] = {};
    HANDLE event = nullptr;
    unsigned frame = 0;
};

std::mutex g_draw;                 // one Present at a time draws the panel
thread_local bool t_inside = false;
Overlay g_over;
nr_menu* g_menu = nullptr;
nrex_ui* g_ui = nullptr;
nr_panel* g_panel = nullptr;
ID3D12Device* g_failed_device = nullptr;   // the device the panel could not be made on (said once)
void* g_unqueued = nullptr;                 // the swap chain no queue was found for (said once)
int g_dumped = 0;

void wait_for(UINT64 value)
{
    if (!value || !g_over.fence || g_over.fence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(g_over.fence->SetEventOnCompletion(value, g_over.event)))
        WaitForSingleObject(g_over.event, 2000);
}

void unmake()
{
    wait_for(g_over.value);
    if (g_panel) nr_panel_destroy(g_panel);
    if (g_ui) nrex_destroy_d3d12(g_ui);
    if (g_menu) nr_menu_destroy(g_menu);
    g_panel = nullptr;
    g_ui = nullptr;
    g_menu = nullptr;
    drop(g_over.list);
    for (auto& a : g_over.allocators) drop(a);
    drop(g_over.fence);
    if (g_over.event) CloseHandle(g_over.event);
    drop(g_over.device);
    g_over = Overlay();
}

bool make(ID3D12Device* device)
{
    size_t font_size = 0;
    char* font = nr_panel_font(&font_size);
    static int codepoints[NR_FONT_MAX_CODEPOINTS];
    const int count = nr_panel_codepoints(codepoints, NR_FONT_MAX_CODEPOINTS);
    const char* why = nullptr;
    g_ui = nrex_create_d3d12(device, font, font_size, codepoints, count, &why);
    if (!g_ui) {
        std::free(font);   /* adopted only by a renderer that started */
        note("no panel on this device: %s", why ? why : "the renderer did not start");
        return false;
    }
    g_menu = nr_menu_create_headless();
    if (g_host.env_file[0]) nr_menu_bridge_use_env_file(g_host.env_file);
    const nr_panel_host host = {g_host.trigger_get, g_host.trigger_set, g_host.user, 0};
    g_panel = g_menu ? nr_panel_create(g_menu, g_ui, g_host.settings, NR_MENU_LANG_EN, &host) : nullptr;
    g_over.device = device;
    device->AddRef();
    bool ok = g_panel != nullptr;
    for (auto& a : g_over.allocators)
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_over.allocators[0],
                                                   nullptr, IID_PPV_ARGS(&g_over.list)));
    if (ok) g_over.list->Close();
    ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_over.fence)));
    g_over.event = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!ok || !g_over.event) {
        note("no panel on this device: its command list, allocators or fence were refused");
        unmake();
        return false;
    }
    note("the panel is made on the game's device; INS opens it");
    return true;
}

/* The queue `sc` presents on. See the top of this file. */
ID3D12CommandQueue* queue_of(IDXGISwapChain* sc, ID3D12Device* device)
{
    if (g_queue_at >= 0) {
        void* candidate = *reinterpret_cast<void**>(reinterpret_cast<char*>(sc) + g_queue_at);
        if (nr_split_known_queue(candidate)) return static_cast<ID3D12CommandQueue*>(candidate);
    }
    ID3D12CommandQueue* last = nr_split_last_queue();
    if (!last) return nullptr;
    ID3D12Device* owner = nullptr;
    const bool same = SUCCEEDED(last->GetDevice(IID_PPV_ARGS(&owner))) && owner == device;
    drop(owner);
    return same ? last : nullptr;
}

int encoding_of(IDXGISwapChain* sc, DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) return NREX_ENCODING_SCRGB;
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        auto it = g_spaces.find(sc);
        if (it != g_spaces.end() && it->second == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            return NREX_ENCODING_HDR10;
    }
    return NREX_ENCODING_SDR;
}

/* `NR_MENU_DUMP=<file>`: the 30th back buffer the panel is drawn over, as a PPM, once - the panel
 * over the game, as it is presented, for a test that cannot look at the screen. Recorded after the
 * panel; written once the frame has finished. */
ID3D12Resource* dump_record(ID3D12GraphicsCommandList* list, ID3D12Resource* back,
                            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layout)
{
    const char* path = std::getenv("NR_MENU_DUMP");
    if (!path || !*path || g_dumped < 0 || ++g_dumped < 30) return nullptr;
    g_dumped = -1;   /* once */
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    UINT64 bytes = 0;
    g_over.device->GetCopyableFootprints(&bd, 0, 1, 0, layout, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    if (FAILED(g_over.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&readback))))
        return nullptr;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = *layout;
    src.pResource = back;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    return readback;
}

void dump_write(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout, DXGI_FORMAT format)
{
    const char* path = std::getenv("NR_MENU_DUMP");
    const UINT w = layout.Footprint.Width, h = layout.Footprint.Height, pitch = layout.Footprint.RowPitch;
    void* mapped = nullptr;
    const D3D12_RANGE all = {0, SIZE_T(pitch) * h};
    if (FAILED(readback->Map(0, &all, &mapped))) return;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::string row(size_t(w) * 3, '\0');
        for (UINT y = 0; y < h; ++y) {
            const unsigned char* p = static_cast<const unsigned char*>(mapped) + layout.Offset + size_t(y) * pitch;
            for (UINT x = 0; x < w; ++x) {
                unsigned char r, g, b;
                uint32_t v;
                std::memcpy(&v, p + size_t(x) * 4, 4);
                if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    r = (unsigned char)((v & 0x3FF) >> 2);
                    g = (unsigned char)(((v >> 10) & 0x3FF) >> 2);
                    b = (unsigned char)(((v >> 20) & 0x3FF) >> 2);
                } else if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    r = p[x * 4 + 2]; g = p[x * 4 + 1]; b = p[x * 4 + 0];
                } else {
                    r = p[x * 4 + 0]; g = p[x * 4 + 1]; b = p[x * 4 + 2];
                }
                row[x * 3 + 0] = char(r);
                row[x * 3 + 1] = char(g);
                row[x * 3 + 2] = char(b);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        note("the panel over the back buffer is in %s (%ux%u)", path, w, h);
    }
    const D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
}

void draw(IDXGISwapChain* sc, ID3D12CommandQueue* queue, const DXGI_SWAP_CHAIN_DESC& desc)
{
    /* Another queue than the last: the old one's work is finished first, so the frames' slots and
     * the one fence stay in step. */
    if (g_over.queue != queue) {
        wait_for(g_over.value);
        g_over.queue = queue;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* back = nullptr;
    if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back))) || !back) return;
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    const unsigned slot = g_over.frame;
    wait_for(g_over.done_at[slot]);
    if (FAILED(g_over.allocators[slot]->Reset()) ||
        FAILED(g_over.list->Reset(g_over.allocators[slot], nullptr))) {
        back->Release();
        return;
    }
    nr_panel_frame(g_panel, uint32_t(bd.Width), bd.Height);
    const int drew = nrex_render_d3d12(g_ui, g_over.list, int(slot), back, uint32_t(desc.BufferDesc.Format),
                                       encoding_of(sc, desc.BufferDesc.Format));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    ID3D12Resource* readback = drew ? dump_record(g_over.list, back, &layout) : nullptr;
    g_over.list->Close();
    if (drew) {
        ID3D12CommandList* lists[] = {g_over.list};
        queue->ExecuteCommandLists(1, lists);
        if (SUCCEEDED(queue->Signal(g_over.fence, ++g_over.value))) g_over.done_at[slot] = g_over.value;
        g_over.frame = (slot + 1) % NREX_D3D12_FRAMES;
    }
    if (readback) {
        wait_for(g_over.value);
        dump_write(readback, layout, desc.BufferDesc.Format);
        readback->Release();
    }
    back->Release();
}

void draw_on(IDXGISwapChain* sc, UINT flags)
{
    if (t_inside || (flags & DXGI_PRESENT_TEST) || !g_host.say) return;
    t_inside = true;
    {
        std::lock_guard<std::mutex> lock(g_draw);
        ID3D12Device* device = nullptr;
        /* A D3D11 swap chain in the same process (a launcher, an overlay) is not the game's. */
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device) {
            /* Hooked here too, so the queues are known from the next frame even before XeSS runs.
             * Once a device, whether it took or not: the hook makes a queue to find the class. */
            static ID3D12Device* watched = nullptr;
            if (watched != device) {
                watched = device;
                char why[160] = "";
                if (!nr_split_watch(device, why, sizeof why))
                    note("the game's queues could not be hooked (%s); the panel takes the presenting "
                         "thread's", why);
            }
            if (g_over.device && g_over.device != device) {
                note("the game changed its D3D12 device; the panel is made again on the new one");
                unmake();
            }
            if (!g_panel && device != g_failed_device && !make(device)) g_failed_device = device;
            ID3D12CommandQueue* queue = g_panel ? queue_of(sc, device) : nullptr;
            if (g_panel && !queue && g_unqueued != sc) {
                g_unqueued = sc;
                note("no queue known yet for the swap chain %p; the panel waits for one",
                     static_cast<void*>(sc));
            }
            /* The keys on every Present, whether or not there is a queue to draw on: Home turns NR
             * on and off with the panel shut. */
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (g_panel && SUCCEEDED(sc->GetDesc(&desc)) && nr_panel_input(g_panel, desc.OutputWindow) && queue)
                draw(sc, queue, desc);
            device->Release();
        }
    }
    t_inside = false;
}

}  // namespace

extern "C" void nr_xess_panel_start(const nr_xess_panel_host* host)
{
    static std::once_flag once;
    std::call_once(once, [host] {
        g_host = *host;
        /* Its own thread: the probe takes a moment, and DllMain must not start a process. */
        HANDLE thread = CreateThread(nullptr, 0, install, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else note("the panel's install thread would not start; no panel");
    });
}

extern "C" int nr_xess_panel_hooked(void)
{
    return g_hooked.load() ? 1 : 0;
}

extern "C" void nr_xess_panel_list(ID3D12GraphicsCommandList* list)
{
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_relaxed) || !list || !g_host.say) return;
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    char why[160] = "";
    if (!nr_split_watch(device, why, sizeof why))
        note("the game's queues could not be hooked (%s); the panel takes the presenting thread's", why);
    device->Release();
    done = true;
}
