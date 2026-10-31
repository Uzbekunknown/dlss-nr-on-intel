/* The panel's Direct3D 12 renderer, for the DirectX 12 proxy (src/xess/nr_xess_panel.cpp).
 *
 * The panel itself is nr_menu_ui.cpp; this draws what it builds. Two passes, recorded into a command
 * list the proxy executes on the queue the game's swap chain presents on, after the game's frame and
 * before the Present:
 *
 *   ImGui's triangles -> an image of the panel's own, 8-bit, cleared to nothing  (UI)
 *   that image -> over the back buffer, in the back buffer's encoding          (OVER)
 *
 * Why not straight into the back buffer: a DirectX 12 game's swap chain may be scRGB or HDR10, and
 * the panel is drawn in sRGB. Drawing it once and laying it over the frame in the frame's encoding is
 * what DLSSNR-AMD's NR Control does (its nr_pe_menu.cpp, MIT), and it is the same shape as the
 * Vulkan path, which blends an image of the panel's own onto the swapchain image. The shaders are
 * nr_menu_ui_d3d12.hlsl, compiled by fxc at build time with their root signatures.
 *
 * This is a renderer of ImGui's draw data written for this tree rather than ImGui's own D3D12
 * backend, which the owner chose not to vendor (2026-10-31). It takes ImGui's texture requests
 * (ImGuiBackendFlags_RendererHasTextures) - the font atlas is made and grown on the GPU from them -
 * and honours ImDrawCmd::VtxOffset. It keeps NREX_D3D12_FRAMES frames' upload buffers; the caller
 * says which frame it is recording and has waited for that frame's previous use, which is what lets
 * this renderer hold no fence of its own.
 */
#include "nr_menu_ui.h"
#include "nr_menu_ui_backend.h"

#include "imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

/* nr_menu_ui_d3d12.hlsl, compiled by fxc once for each entry point */
#include "nr_menu_ui_d3d12_ui_vs.h"
#include "nr_menu_ui_d3d12_ui_ps.h"
#include "nr_menu_ui_d3d12_over_vs.h"
#include "nr_menu_ui_d3d12_over_ps.h"

#define NREX_ERROR(...) fprintf(stderr, __VA_ARGS__)

namespace {

constexpr UINT kSrvs = 64;                                  // the panel's image and ImGui's textures
constexpr DXGI_FORMAT kPanelFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float kWhite = 203.0f;                            // nits for the panel's white over HDR

template <class T> void drop(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

/* What one frame in flight keeps alive until its slot is used again: the vertices and indices it
 * drew from, and whatever was replaced while it was recorded (an upload, a texture, an image). */
struct Frame {
    ID3D12Resource* vertices = nullptr;
    ID3D12Resource* indices = nullptr;
    UINT vertex_room = 0, index_room = 0;
    std::vector<ID3D12Resource*> retired;
    std::vector<UINT> retired_srvs;
};

/* One of ImGui's textures, as this renderer holds it. */
struct Texture {
    ID3D12Resource* resource = nullptr;
    UINT srv = 0;
};

struct D3D12Ui : nrex_ui {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* ui_root = nullptr;
    ID3D12RootSignature* over_root = nullptr;
    ID3D12PipelineState* ui_pipeline = nullptr;
    struct { DXGI_FORMAT format; ID3D12PipelineState* pipeline; } over[8] = {};
    ID3D12DescriptorHeap* rtvs = nullptr;                    // 0 the panel's image, 1 the back buffer
    ID3D12DescriptorHeap* srvs = nullptr;                    // shader-visible
    UINT rtv_step = 0, srv_step = 0;
    bool srv_used[kSrvs] = {};
    ID3D12Resource* image = nullptr;
    UINT image_srv = 0, width = 0, height = 0;
    Frame frames[NREX_D3D12_FRAMES];
};

UINT srv_take(D3D12Ui* u)
{
    for (UINT i = 0; i < kSrvs; ++i)
        if (!u->srv_used[i]) {
            u->srv_used[i] = true;
            return i;
        }
    return kSrvs;
}

D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(D3D12Ui* u, UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = u->srvs->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * u->srv_step;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(D3D12Ui* u, UINT i)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = u->srvs->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * u->srv_step;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE rtv(D3D12Ui* u, UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = u->rtvs->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * u->rtv_step;
    return h;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

ID3D12Resource* buffer(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

ID3D12Resource* texture(ID3D12Device* device, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kPanelFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kPanelFormat;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                               (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clear : nullptr,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

/* ImGui's texture requests: make, fill, or let go of a texture. The copies are recorded into the
 * frame's list ahead of the draws that read them; the upload buffers stay with the frame. */
void update_texture(D3D12Ui* u, ImTextureData* tex, ID3D12GraphicsCommandList* list, Frame& frame)
{
    if (tex->Status == ImTextureStatus_WantDestroy) {
        /* Not before every frame that may still read it has finished: kept until this slot is
         * used again, as the Vulkan backend waits out its image count. */
        if (tex->UnusedFrames < NREX_D3D12_FRAMES) return;
        Texture* t = static_cast<Texture*>(tex->BackendUserData);
        if (t) {
            frame.retired.push_back(t->resource);
            frame.retired_srvs.push_back(t->srv);
            delete t;
        }
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    const bool create = tex->Status == ImTextureStatus_WantCreate;
    Texture* t = static_cast<Texture*>(tex->BackendUserData);
    if (create) {
        t = new (std::nothrow) Texture();
        if (!t) return;
        t->srv = srv_take(u);
        t->resource = t->srv < kSrvs ? texture(u->device, UINT(tex->Width), UINT(tex->Height),
                                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST)
                                     : nullptr;
        if (!t->resource) {
            if (t->srv < kSrvs) u->srv_used[t->srv] = false;
            delete t;
            NREX_ERROR("[menu ui] no texture for the panel's font (%dx%d)\n", tex->Width, tex->Height);
            return;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = kPanelFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        u->device->CreateShaderResourceView(t->resource, &view, srv_cpu(u, t->srv));
        tex->BackendUserData = t;
        tex->SetTexID((ImTextureID)srv_gpu(u, t->srv).ptr);
    }
    if (!t) return;
    /* The whole texture on creation, which also clears it; the rectangle that changed after. */
    const int x = create ? 0 : tex->UpdateRect.x, y = create ? 0 : tex->UpdateRect.y;
    const int w = create ? tex->Width : tex->UpdateRect.w, h = create ? tex->Height : tex->UpdateRect.h;
    if (w > 0 && h > 0) {
        const UINT pitch = (UINT(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        ID3D12Resource* upload = buffer(u->device, UINT64(pitch) * UINT(h));
        unsigned char* at = nullptr;
        const D3D12_RANGE none = {0, 0};
        if (!upload || FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&at)))) {
            drop(upload);
            NREX_ERROR("[menu ui] no upload buffer for the panel's font\n");
            return;
        }
        for (int row = 0; row < h; ++row) {
            const unsigned char* from = static_cast<const unsigned char*>(tex->GetPixelsAt(x, y + row));
            unsigned char* to = at + size_t(row) * pitch;
            if (tex->Format == ImTextureFormat_RGBA32) {
                std::memcpy(to, from, size_t(w) * 4);
            } else {   /* Alpha8: white, with the coverage as alpha */
                for (int i = 0; i < w; ++i) {
                    to[i * 4 + 0] = to[i * 4 + 1] = to[i * 4 + 2] = 255;
                    to[i * 4 + 3] = from[i];
                }
            }
        }
        upload->Unmap(0, nullptr);
        if (!create)
            barrier(list, t->resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = kPanelFormat;
        src.PlacedFootprint.Footprint.Width = UINT(w);
        src.PlacedFootprint.Footprint.Height = UINT(h);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = t->resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, UINT(x), UINT(y), 0, &src, nullptr);
        barrier(list, t->resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        frame.retired.push_back(upload);
    } else if (create) {
        barrier(list, t->resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    tex->SetStatus(ImTextureStatus_OK);
}

/* The panel's own image at the back buffer's size. One that is replaced may still be read by a frame
 * in flight, so it goes with this frame's slot, and its view moves to a new descriptor rather than
 * overwriting one a recorded list still points at. */
bool ensure_image(D3D12Ui* u, UINT width, UINT height, Frame& frame)
{
    if (u->image && u->width == width && u->height == height) return true;
    if (u->image) {
        frame.retired.push_back(u->image);
        frame.retired_srvs.push_back(u->image_srv);
        u->image = nullptr;
    }
    u->image_srv = srv_take(u);
    if (u->image_srv >= kSrvs) return false;
    u->image = texture(u->device, width, height, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!u->image) {
        u->srv_used[u->image_srv] = false;
        NREX_ERROR("[menu ui] no %ux%u image for the panel\n", width, height);
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = kPanelFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    u->device->CreateShaderResourceView(u->image, &view, srv_cpu(u, u->image_srv));
    u->device->CreateRenderTargetView(u->image, nullptr, rtv(u, 0));
    u->width = width;
    u->height = height;
    return true;
}

ID3D12PipelineState* over_pipeline(D3D12Ui* u, DXGI_FORMAT format)
{
    for (auto& o : u->over)
        if (o.pipeline && o.format == format) return o.pipeline;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = u->over_root;
    pd.VS = {nr_menu_ui_d3d12_over_vs, sizeof nr_menu_ui_d3d12_over_vs};
    pd.PS = {nr_menu_ui_d3d12_over_ps, sizeof nr_menu_ui_d3d12_over_ps};
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;                      // premultiplied over the game's colour
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;        // and the game's alpha kept
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = nullptr;
    if (FAILED(u->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)))) {
        NREX_ERROR("[menu ui] no pipeline to lay the panel over DXGI format %d\n", int(format));
        return nullptr;
    }
    for (auto& o : u->over)
        if (!o.pipeline) {
            o.format = format;
            o.pipeline = p;
            return p;
        }
    return p;   /* more formats than slots: used once, and leaked rather than freed under a list */
}

void free_textures(D3D12Ui* u)
{
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1) {
            Texture* t = static_cast<Texture*>(tex->BackendUserData);
            if (t) {
                drop(t->resource);
                if (t->srv < kSrvs) u->srv_used[t->srv] = false;
                delete t;
            }
            tex->SetTexID(ImTextureID_Invalid);
            tex->BackendUserData = nullptr;
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
}

void release_frame(D3D12Ui* u, Frame& frame)
{
    for (ID3D12Resource* r : frame.retired)
        if (r) r->Release();
    frame.retired.clear();
    for (UINT s : frame.retired_srvs)
        if (s < kSrvs) u->srv_used[s] = false;
    frame.retired_srvs.clear();
}

}  // namespace

extern "C" nrex_ui* nrex_create_d3d12(void* device_v, const char* font_bytes, size_t font_size,
                                      const int* codepoints, int codepoint_count, const char** why)
{
    static const char* last_why = "the panel's Direct3D 12 renderer could not start";
    if (!why) why = &last_why;
    ID3D12Device* device = static_cast<ID3D12Device*>(device_v);
    if (!device) {
        *why = "no Direct3D 12 device for the panel";
        return nullptr;
    }
    D3D12Ui* u = new (std::nothrow) D3D12Ui();
    if (!u) {
        *why = "out of memory for the panel";
        return nullptr;
    }
    u->device = device;
    device->AddRef();
    bool ok =
        SUCCEEDED(device->CreateRootSignature(0, nr_menu_ui_d3d12_ui_vs, sizeof nr_menu_ui_d3d12_ui_vs,
                                              IID_PPV_ARGS(&u->ui_root))) &&
        SUCCEEDED(device->CreateRootSignature(0, nr_menu_ui_d3d12_over_vs, sizeof nr_menu_ui_d3d12_over_vs,
                                              IID_PPV_ARGS(&u->over_root)));
    if (!ok) *why = "the panel's root signatures were refused";
    if (ok) {
        static const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, pos)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, uv)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(ImDrawVert, col)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = u->ui_root;
        pd.VS = {nr_menu_ui_d3d12_ui_vs, sizeof nr_menu_ui_d3d12_ui_vs};
        pd.PS = {nr_menu_ui_d3d12_ui_ps, sizeof nr_menu_ui_d3d12_ui_ps};
        pd.InputLayout = {layout, UINT(sizeof layout / sizeof layout[0])};
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;                  // ImGui's own blend, over a cleared image: the colour
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;    // comes out premultiplied by its coverage
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kPanelFormat;
        pd.SampleDesc.Count = 1;
        const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&u->ui_pipeline));
        ok = SUCCEEDED(hr);
        if (!ok) {
            static char said[96];
            std::snprintf(said, sizeof said, "the panel's pipeline was refused (0x%08lX)", (unsigned long)hr);
            *why = said;
        }
    }
    if (ok) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        ok = SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&u->rtvs)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSrvs;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = ok && SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&u->srvs)));
        if (!ok) *why = "no descriptor heaps for the panel";
        else {
            u->rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            u->srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    /* The panel's context, which every renderer shares; then what this one can do. */
    if (ok && !nrex_context_create(font_bytes, font_size, codepoints, codepoint_count, why)) ok = false;
    if (!ok) {
        drop(u->srvs);
        drop(u->rtvs);
        drop(u->ui_pipeline);
        drop(u->over_root);
        drop(u->ui_root);
        drop(u->device);
        delete u;
        return nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "nr_menu_ui_d3d12";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    return u;
}

extern "C" void nrex_destroy_d3d12(nrex_ui* base)
{
    D3D12Ui* u = static_cast<D3D12Ui*>(base);
    if (!u) return;
    free_textures(u);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::DestroyContext();
    for (Frame& f : u->frames) {
        release_frame(u, f);
        drop(f.vertices);
        drop(f.indices);
    }
    for (auto& o : u->over) drop(o.pipeline);
    drop(u->image);
    drop(u->srvs);
    drop(u->rtvs);
    drop(u->ui_pipeline);
    drop(u->over_root);
    drop(u->ui_root);
    drop(u->device);
    delete u;
}

extern "C" int nrex_render_d3d12(nrex_ui* base, void* list_v, int frame_index, void* target_v,
                                 uint32_t format_v, int encoding)
{
    D3D12Ui* u = static_cast<D3D12Ui*>(base);
    ID3D12GraphicsCommandList* list = static_cast<ID3D12GraphicsCommandList*>(list_v);
    ID3D12Resource* target = static_cast<ID3D12Resource*>(target_v);
    if (!u || !list || !target || !u->in_frame) return 0;
    u->in_frame = false;
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    Frame& frame = u->frames[unsigned(frame_index) % NREX_D3D12_FRAMES];
    /* The caller has waited for this slot's last frame, so what it kept can go now. */
    release_frame(u, frame);

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const UINT width = UINT(desc.Width), height = desc.Height;
    const DXGI_FORMAT format = format_v ? DXGI_FORMAT(format_v) : desc.Format;
    if (!draw || width == 0 || height == 0) return 0;
    if (!ensure_image(u, width, height, frame)) return 0;
    ID3D12PipelineState* over = over_pipeline(u, format);
    if (!over) return 0;

    ID3D12DescriptorHeap* heaps[] = {u->srvs};
    list->SetDescriptorHeaps(1, heaps);
    if (draw->Textures)
        for (ImTextureData* tex : *draw->Textures)
            if (tex->Status != ImTextureStatus_OK) update_texture(u, tex, list, frame);

    /* ImGui into the panel's image, from nothing. */
    barrier(list, u->image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE image_rtv = rtv(u, 0);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(image_rtv, clear, 0, nullptr);
    if (draw->TotalVtxCount > 0 && draw->DisplaySize.x > 0.0f && draw->DisplaySize.y > 0.0f) {
        if (frame.vertex_room < UINT(draw->TotalVtxCount)) {
            drop(frame.vertices);
            frame.vertex_room = UINT(draw->TotalVtxCount) + 5000;
            frame.vertices = buffer(u->device, UINT64(frame.vertex_room) * sizeof(ImDrawVert));
        }
        if (frame.index_room < UINT(draw->TotalIdxCount)) {
            drop(frame.indices);
            frame.index_room = UINT(draw->TotalIdxCount) + 10000;
            frame.indices = buffer(u->device, UINT64(frame.index_room) * sizeof(ImDrawIdx));
        }
        ImDrawVert* vertices = nullptr;
        ImDrawIdx* indices = nullptr;
        const D3D12_RANGE none = {0, 0};
        if (!frame.vertices || !frame.indices ||
            FAILED(frame.vertices->Map(0, &none, reinterpret_cast<void**>(&vertices))) ||
            FAILED(frame.indices->Map(0, &none, reinterpret_cast<void**>(&indices)))) {
            if (vertices) frame.vertices->Unmap(0, nullptr);
            frame.vertex_room = frame.index_room = 0;
            drop(frame.vertices);
            drop(frame.indices);
            barrier(list, u->image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return 0;
        }
        for (const ImDrawList* cl : draw->CmdLists) {
            std::memcpy(vertices, cl->VtxBuffer.Data, size_t(cl->VtxBuffer.Size) * sizeof(ImDrawVert));
            std::memcpy(indices, cl->IdxBuffer.Data, size_t(cl->IdxBuffer.Size) * sizeof(ImDrawIdx));
            vertices += cl->VtxBuffer.Size;
            indices += cl->IdxBuffer.Size;
        }
        frame.vertices->Unmap(0, nullptr);
        frame.indices->Unmap(0, nullptr);

        /* The projection ImGui's own backends use: its display rectangle onto clip space. */
        const float l = draw->DisplayPos.x, r = draw->DisplayPos.x + draw->DisplaySize.x;
        const float t = draw->DisplayPos.y, b = draw->DisplayPos.y + draw->DisplaySize.y;
        const float projection[4][4] = {
            {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
            {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
            {0.0f, 0.0f, 0.5f, 0.0f},
            {(r + l) / (l - r), (t + b) / (b - t), 0.5f, 1.0f},
        };
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, draw->DisplaySize.x, draw->DisplaySize.y, 0.0f, 1.0f};
        D3D12_VERTEX_BUFFER_VIEW vbv = {frame.vertices->GetGPUVirtualAddress(),
                                        UINT(draw->TotalVtxCount) * UINT(sizeof(ImDrawVert)),
                                        UINT(sizeof(ImDrawVert))};
        D3D12_INDEX_BUFFER_VIEW ibv = {frame.indices->GetGPUVirtualAddress(),
                                       UINT(draw->TotalIdxCount) * UINT(sizeof(ImDrawIdx)),
                                       sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        auto state = [&] {
            list->OMSetRenderTargets(1, &image_rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->SetPipelineState(u->ui_pipeline);
            list->SetGraphicsRootSignature(u->ui_root);
            list->SetGraphicsRoot32BitConstants(0, 16, projection, 0);
            const float blend[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            list->OMSetBlendFactor(blend);
        };
        state();
        UINT vertex_base = 0, index_base = 0;
        const ImVec2 origin = draw->DisplayPos;
        for (const ImDrawList* cl : draw->CmdLists) {
            for (const ImDrawCmd& cmd : cl->CmdBuffer) {
                if (cmd.UserCallback) {
                    if (cmd.UserCallback == ImDrawCallback_ResetRenderState) state();
                    else cmd.UserCallback(cl, &cmd);
                    continue;
                }
                const float x0 = cmd.ClipRect.x - origin.x, y0 = cmd.ClipRect.y - origin.y;
                const float x1 = cmd.ClipRect.z - origin.x, y1 = cmd.ClipRect.w - origin.y;
                if (x1 <= x0 || y1 <= y0) continue;
                const D3D12_RECT scissor = {LONG(x0), LONG(y0), LONG(x1), LONG(y1)};
                D3D12_GPU_DESCRIPTOR_HANDLE atlas;
                atlas.ptr = UINT64(cmd.GetTexID());
                if (!atlas.ptr) continue;   /* a texture not made yet: nothing to sample */
                list->SetGraphicsRootDescriptorTable(1, atlas);
                list->RSSetScissorRects(1, &scissor);
                list->DrawIndexedInstanced(cmd.ElemCount, 1, cmd.IdxOffset + index_base,
                                           INT(cmd.VtxOffset + vertex_base), 0);
            }
            vertex_base += UINT(cl->VtxBuffer.Size);
            index_base += UINT(cl->IdxBuffer.Size);
        }
    }
    barrier(list, u->image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    /* That image over the back buffer, which is in PRESENT at a Present and goes back to it. */
    const D3D12_CPU_DESCRIPTOR_HANDLE back_rtv = rtv(u, 1);
    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    u->device->CreateRenderTargetView(target, &view, back_rtv);
    barrier(list, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    const D3D12_RECT scissor = {0, 0, LONG(width), LONG(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetPipelineState(over);
    list->SetGraphicsRootSignature(u->over_root);
    list->SetGraphicsRootDescriptorTable(1, srv_gpu(u, u->image_srv));
    const UINT constants[2] = {UINT(encoding), 0};
    float white = kWhite;
    UINT white_bits;
    std::memcpy(&white_bits, &white, sizeof white_bits);
    list->SetGraphicsRoot32BitConstant(0, constants[0], 0);
    list->SetGraphicsRoot32BitConstant(0, white_bits, 1);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    barrier(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    return 1;
}
