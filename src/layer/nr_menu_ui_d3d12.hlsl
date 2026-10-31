// nr_menu_ui_d3d12.hlsl - the panel's two passes in Direct3D 12 (nr_menu_ui_d3d12.cpp).
//
// UI: ImGui's triangles into the panel's own 8-bit image, cleared to nothing, with ImGui's blend -
//     so what the image holds is the panel's colour premultiplied by its coverage.
// OVER: that image over the game's back buffer, one triangle across the screen, in the back
//     buffer's encoding: as drawn for SDR; for scRGB (linear, 1.0 = 80 nits) and HDR10 (BT.2020
//     under the PQ curve) the panel's sRGB is taken to light at `white` nits, 203 as BT.2408 puts
//     graphics white - DLSSNR-AMD's NR Control draws over HDR the same way. Premultiplied, so the
//     blend is ONE over INV_SRC_ALPHA in every encoding.
//
// fxc compiles each entry point on its own (src/xess/build_xess.bat), and each vertex shader carries
// its pass's root signature, so the C++ side needs no shader compiler and no serialiser in a game.

#define UI_ROOT \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "RootConstants(num32BitConstants=16, b0, visibility=SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, " \
    "addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL)"

#define OVER_ROOT \
    "RootConstants(num32BitConstants=2, b0, visibility=SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL)"

#if defined(UI_VS) || defined(UI_PS)

struct Vertex {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
struct Pixel {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};

#ifdef UI_VS
cbuffer Projection : register(b0) { float4x4 projection; };

[RootSignature(UI_ROOT)]
Pixel main(Vertex v)
{
    Pixel p;
    p.pos = mul(projection, float4(v.pos, 0.0, 1.0));
    p.col = v.col;
    p.uv = v.uv;
    return p;
}
#else
SamplerState linear_clamp : register(s0);
Texture2D atlas : register(t0);

float4 main(Pixel p) : SV_Target
{
    return p.col * atlas.Sample(linear_clamp, p.uv);
}
#endif

#else // OVER_VS, OVER_PS

#ifdef OVER_VS
[RootSignature(OVER_ROOT)]
float4 main(uint id : SV_VertexID) : SV_POSITION
{
    float2 at = float2((id << 1) & 2, id & 2);
    return float4(at * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#else
cbuffer Over : register(b0) {
    uint encoding;    // 0 SDR, 1 scRGB, 2 HDR10 (NREX_ENCODING_*)
    float white;      // the nits the panel's white is shown at, in scRGB and HDR10
};
Texture2D<float4> panel : register(t0);

float3 from_srgb(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// SMPTE ST 2084, from nits.
float3 to_pq(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(float4 at : SV_POSITION) : SV_Target
{
    float4 c = panel.Load(int3(at.xy, 0));
    if (c.a <= 0.0) discard;
    if (encoding == 0) return c;
    float3 straight = saturate(c.rgb / c.a);
    float3 light = from_srgb(straight) * white;          // BT.709, in nits
    if (encoding == 1) return float4(light / 80.0 * c.a, c.a);
    const float3x3 to_2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                                      0.0690970, 0.9195400, 0.0113612,
                                      0.0163916, 0.0880132, 0.8955950);
    return float4(to_pq(mul(to_2020, light)) * c.a, c.a);
}
#endif

#endif
