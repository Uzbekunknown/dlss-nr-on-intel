/* nr_xess.hlsl — the proxy's compute passes, one root signature.
 *
 * Built nine times (build_xess.bat), one pass each. With mode=carry, NR's change to an
 * earlier frame is carried to this one:
 *   CLEAR   empties the claims;
 *   CLAIM   marks, on the last frame's pixels, the vectors of the surfaces that came from each;
 *   TRACK   follows the game's motion vectors, frame by frame, back to the frame NR was given;
 *   MATCH   reads each change where the tracker says, and how far to trust it there;
 *   REDUCE  sums what MATCH trusted over CELL-pixel squares;
 *   APPLY   adds the change to the colour XeSS is about to upscale, filling what MATCH did not
 *           trust from the trusted change around it.
 * With mode=cut, the game's command list is cut at XeSS and NR answers this very frame
 * between the halves (nr_xess_split.cpp):
 *   ENCODE  the frame NR is given, into the buffer the daemon shares, before the cut;
 *   MOTION  after it in that buffer, each pixel's offset to the frame NR was given before,
 *           which the daemon's history follows;
 *   COMPOSE NR's change to it added to the colour, after the cut, when its answer came.
 *
 * Everything arrives as buffers — copies of the game's textures laid out in their own
 * formats — because root descriptors reach buffers directly, where a texture would need a
 * descriptor heap of ours bound on the game's command list.
 *
 * Positions. A colour pixel's index is its centre. Pixel c of a frame drawn with jitter J
 * shows the scene at c - J, the "unjittered" position (XeSS shears the projection by +J, so
 * the picture moves by +J). A motion vector points from where a surface is now to where it
 * was the frame before, in pixels of the motion texture, and holds no jitter.
 *
 * A tracker holds, for each pixel of the current frame, how far its surface has moved since
 * the frame NR was given: unjittered position then, minus unjittered position now. It is
 * zero on the frame NR was given, and each frame adds that frame's vector to the tracker of
 * the frame before, read where the surface was — at the nearest edge for a surface that was
 * off screen then. Whether the surface found there is the same one, MATCH asks the picture;
 * the tracker only gives up where two surfaces that move differently came from the same place
 * (CLAIM, `contested`): a surface uncovered behind something that moved. There it holds
 * UNKNOWN, which reads as off screen from then on.
 */
#define RS "RootConstants(num32BitConstants=32, b0), SRV(t0), SRV(t1), SRV(t2), SRV(t3), SRV(t4), UAV(u0)"

#if defined(ENCODE) || defined(COMPOSE) || defined(MOTION)

/* nr_xess.c `struct cut_constants`, field for field. */
cbuffer Constants : register(b0)
{
    uint width;            /* the colour's extent */
    uint height;
    uint pitch;            /* bytes between rows of the colour and of the result */
    uint kind;             /* 0 R16G16B16A16_FLOAT, 1 R11G11B10_FLOAT, 2 R10G10B10A2_UNORM */
    float white;           /* the scene value the proxy shows as display white */
    float strength;        /* compose: 0 adds nothing, 1 all of NR's change */
    uint2 frame;           /* compose: the frame's fence value, low and high, as the stamp names it */
    uint motion_pitch;     /* motion: bytes between rows of the motion copy */
    uint offsets;          /* ... where the offsets start in the buffer the daemon shares */
    uint2 motion_size;     /* ... the motion region, in its texels */
    float2 motion_scale;   /* ... a texel's value to colour pixels (velocity scale, extents) */
    float2 motion_texels;  /* ... motion texels per colour pixel */
    float2 jitter;         /* ... this frame's */
    float2 previous_jitter;/* ... the frame's NR was given before */
};

#else

cbuffer Constants : register(b0)
{
    uint width;            /* the colour's extent */
    uint height;
    uint pitch;            /* match, apply: bytes between rows of the colour and of the result */
    uint kind;             /* match, apply: 0 R16G16B16A16_FLOAT, 1 R11G11B10_FLOAT, 2 R10G10B10A2_UNORM */
    float white;           /* the scene value the proxy showed as display white */
    float strength;        /* apply: 0 adds nothing, 1 all of NR's change */
    uint change_pitch;     /* match: bytes between rows of a change */
    uint warp;             /* match: 1 reads each change where its tracker says */
    float2 shift;          /* match: the newest change's frame's jitter, minus this frame's */
    uint motion_pitch;     /* the vectors' passes: bytes between rows of the motion copy */
    uint reset;            /* track: 1 starts the tracker afresh, on the frame NR is given */
    uint2 motion_size;     /* ... the motion region, in its texels */
    float2 motion_scale;   /* ... a texel's value to colour pixels (velocity scale, extents) */
    float2 motion_texels;  /* ... motion texels per colour pixel */
    float2 jitter;         /* ... this frame's */
    float2 previous_jitter;/* claim, track: the last frame's */
    uint vectors;          /* track: 1 t2 holds this frame's claims, 2 t3 the last frame's vectors;
                              reduce, apply: 1 this frame's vectors are bound */
    uint fading;           /* match: t3 and t4 hold the change before the newest, fading out */
    float2 old_shift;      /* match: its frame's jitter, minus this frame's */
    float fade;            /* match: 0 all the older change, 1 all the newest */
    uint cell;             /* reduce, apply: pixels a side of a cell */
    uint2 cells;           /* reduce, apply: the cells' extent */
    float match_from;      /* match: how far outside the neighbourhood a change starts to be refused */
    float match_to;        /* match: ... and where it is refused entirely */
};

#endif

/* What MATCH leaves for each pixel, seven floats: the change it trusted times the trust
 * (rgb), the trust (0-1), and the pixel's own `likeness` (rgb). REDUCE leaves the same seven
 * summed over a cell, the last three weighted by the trust, and two more: the cell's vectors,
 * weighted by the trust too. */
#define MATCHED 28
#define SUMMED 36

/* A tracker's value for a surface it cannot follow: far off any screen, so MATCH reads no
 * change for it and the daemon is told nothing of where it was. */
#define UNKNOWN 1e30

#if defined(CLAIM) || defined(TRACK) || defined(REDUCE) || defined(APPLY) || defined(MOTION)

/* The vector at an unjittered colour position, to where its surface was the frame before, in
 * colour pixels: the nearest texel, because high-resolution vectors are dilated to the
 * foremost surface and blending them across an edge invents a motion neither side has. */
float2 vector_in(ByteAddressBuffer vectors_of, float2 u)
{
    int2 t = clamp(int2(floor((u + 0.5) * motion_texels)), 0, int2(motion_size) - 1);
    uint v = vectors_of.Load(t.y * motion_pitch + t.x * 4);
    return float2(f16tof32(v), f16tof32(v >> 16)) * motion_scale;
}

/* How far apart two vectors may be and still be one surface's: a little for the vectors' own
 * precision, and more for a fast surface, whose vectors spread across it as it turns or nears
 * the camera. Two surfaces that part faster than this are told apart. */
float spread(float2 m)
{
    return 1.5 + 0.15 * length(m);
}

#endif

#if defined(MATCH) || defined(APPLY) || defined(ENCODE) || defined(COMPOSE)

ByteAddressBuffer colour : register(t0);       /* the colour, in its own format */

float4 load_texel(uint at)
{
    float4 c;
    if (kind == 0) {
        uint2 v = colour.Load2(at);
        c = float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16));
    } else {
        uint v = colour.Load(at);
        if (kind == 1)  /* 6- and 5-bit mantissas, the same exponent as a half */
            c = float4(f16tof32((v & 0x7FF) << 4), f16tof32(((v >> 11) & 0x7FF) << 4),
                       f16tof32(((v >> 22) & 0x3FF) << 5), 1);
        else
            c = float4(v & 0x3FF, (v >> 10) & 0x3FF, (v >> 20) & 0x3FF, (v >> 30) * 341)
                / 1023.0;
    }
    return c;
}

/* What the match compares: the square root of each of the proxy's channels, over white and
 * through the knee, as the CPU has them for a frame NR was given (nr_xess.c `publish`). Each
 * channel, not the luminance alone: a blue surface and a beige one as bright are different
 * surfaces. The root spreads the darks the way display levels would. */
float3 likeness(float3 c)
{
    float3 d = max(c, 0) / white;
    float lit = dot(d, float3(0.2126, 0.7152, 0.0722));
    if (lit > 0.75)
        d *= (0.75 + 0.25 * (1 - exp(-(lit - 0.75) / 0.25))) / lit;
    return sqrt(saturate(d));
}

#endif

#if defined(APPLY) || defined(COMPOSE)

RWByteAddressBuffer result : register(u0);     /* the colour XeSS gets */

void store_texel(uint at, float4 c)
{
    if (kind == 0) {
        result.Store2(at, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16),
                                f32tof16(c.b) | (f32tof16(c.a) << 16)));
        return;
    }
    if (kind == 1) {
        /* Rounded to nearest, and held below infinity: a carry out of the largest finite
         * value would otherwise land on the format's infinity. */
        uint r = min((f32tof16(c.r) + 0x8) >> 4, 0x7BF);
        uint g = min((f32tof16(c.g) + 0x8) >> 4, 0x7BF);
        uint b = min((f32tof16(c.b) + 0x10) >> 5, 0x3DF);
        result.Store(at, r | (g << 11) | (b << 22));
        return;
    }
    uint4 q = uint4(round(saturate(c) * float4(1023, 1023, 1023, 3)));
    result.Store(at, q.r | (q.g << 10) | (q.b << 20) | (q.a << 30));
}

/* What the proxy's knee took from a pixel, as a factor: a change made to its proxy, scaled by
 * it, is the model's relative change at the original's own level, which is what `resolve` does
 * there. */
float knee_gain(float3 c)
{
    float lit = dot(max(c, 0) / white, float3(0.2126, 0.7152, 0.0722));
    float rolled = 0.75 + 0.25 * (1 - exp(-(lit - 0.75) / 0.25));
    return lit > 0.75 ? lit / rolled : 1;
}

#endif

/* Claims: for each pixel of the last frame, the least and the most of each component of the
 * vectors that came from it this frame, sixteen bytes, as unsigned integers that keep the
 * floats' order (`ordered`), so the atomics can take them. Nobody came from a pixel whose
 * least x is still CLEAR's 0xFFFFFFFF. */

#if defined(CLAIM) || defined(TRACK)

uint ordered(float f)
{
    uint u = asuint(f);
    return (u & 0x80000000) ? ~u : u | 0x80000000;
}

float unordered(uint u)
{
    return asfloat((u & 0x80000000) ? u & 0x7FFFFFFF : ~u);
}

#endif

#ifdef CLEAR

RWByteAddressBuffer claims : register(u0);

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    claims.Store4((id.y * width + id.x) * 16, uint4(0xFFFFFFFF, 0, 0xFFFFFFFF, 0));
}

#endif

#ifdef CLAIM

ByteAddressBuffer motion : register(t0);    /* this frame's vectors, R16G16_FLOAT */
RWByteAddressBuffer claims : register(u0);

/* Each pixel of this frame claims the last frame's pixel nearest to where its surface was,
 * and leaves its vector there. */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    float2 u = float2(id.xy) - jitter;
    float2 m = vector_in(motion, u);
    int2 i = int2(floor(u + m + previous_jitter + 0.5));
    if (any(i < 0) || any(i >= int2(width, height)))
        return;
    uint at = (i.y * width + i.x) * 16;
    claims.InterlockedMin(at, ordered(m.x));
    claims.InterlockedMax(at + 4, ordered(m.x));
    claims.InterlockedMin(at + 8, ordered(m.y));
    claims.InterlockedMax(at + 12, ordered(m.y));
}

#endif

#ifdef TRACK

ByteAddressBuffer motion : register(t0);    /* this frame's vectors, R16G16_FLOAT */
ByteAddressBuffer before : register(t1);    /* the tracker as of the last frame */
ByteAddressBuffer claims : register(t2);    /* this frame's claims, when `vectors & 1` */
ByteAddressBuffer last_motion : register(t3);  /* the last frame's vectors, when `vectors & 2` */
RWByteAddressBuffer after : register(u0);   /* the tracker as of this frame */

float2 tracker_at(int2 i)
{
    return asfloat(before.Load2((i.y * width + i.x) * 8));
}

/* Whether a surface that moves otherwise came from about where this one did — the last
 * frame's pixels within one of where this surface was, claimed by vectors further from its
 * own than `spread` — and this one is not the surface that was there. So it is for a surface
 * uncovered behind something that moved: the vectors trace it back to the thing in front,
 * the thing in front came from there too, and the tracker there would carry the thing in
 * front's change onto it, which the picture alone cannot always refuse: hair looks like dark
 * wood. The surface that was there moves on much as it moved then, so the one whose vector
 * is near the last frame's vector there, nearer than half the claims' spread, keeps its
 * tracker; the other is not followed, and APPLY fills it from the surfaces round it that move
 * as it does. Without the last frame's vectors neither keeps it. The claims are one frame's
 * vectors, so how the camera sped up since the last does not count against them; only the
 * owner's own vector is held to the last frame's, against the claims' own spread. A thing
 * that starts to move from rest loses the race to what it uncovers, for that frame. */
bool contested(float2 g, float2 was, float2 m)
{
    int2 n = clamp(int2(floor(g + 0.5)), 0, int2(width, height) - 1);
    float2 low = m, high = m;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int2 q = clamp(n + int2(dx, dy), 0, int2(width, height) - 1);
            uint4 c = claims.Load4((q.y * width + q.x) * 16);
            if (c.x != 0xFFFFFFFF) {
                low = min(low, float2(unordered(c.x), unordered(c.z)));
                high = max(high, float2(unordered(c.y), unordered(c.w)));
            }
        }
    float2 apart2 = max(m - low, high - m);
    float apart = max(apart2.x, apart2.y);
    bool lost = apart > spread(m);
    if (lost && (vectors & 2)) {
        float2 then = vector_in(last_motion, was);
        lost = !(length(then - m) < max(spread(m), 0.5 * apart));
    }
    return lost;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint at = (id.y * width + id.x) * 8;
    if (reset) {
        after.Store2(at, asuint(float2(0, 0)));
        return;
    }
    float2 extent = float2(width, height);
    float2 u = float2(id.xy) - jitter;          /* this pixel's surface, unjittered */
    float2 m = vector_in(motion, u);            /* to where it was the frame before */
    float2 g = u + m + previous_jitter;         /* ... on the last frame's pixel grid */
    if ((vectors & 1) && contested(g, u + m, m)) {
        after.Store2(at, asuint(float2(UNKNOWN, UNKNOWN)));
        return;
    }
    /* Off screen then: the nearest edge's tracker, as a pan carries it on. */
    float2 c = clamp(g, 0, extent - 1);
    int2 i = int2(floor(c));
    int2 j = min(i + 1, int2(extent) - 1);
    float2 f = c - floor(c);
    float2 a = tracker_at(i), b = tracker_at(int2(j.x, i.y));
    float2 d = tracker_at(int2(i.x, j.y)), e = tracker_at(j);
    after.Store2(at, asuint(m + lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y)));
}

#endif

#ifdef MATCH

ByteAddressBuffer change : register(t1);       /* NR's newest change */
ByteAddressBuffer tracker : register(t2);      /* where this frame's surfaces were on its frame */
ByteAddressBuffer old_change : register(t3);   /* the change before it, while it fades out */
ByteAddressBuffer old_tracker : register(t4);
RWByteAddressBuffer matched : register(u0);

/* A change is NR's answer minus what NR was given, both in the proxy's linear light
 * (src/ref/nr_display.py `encode`), sixteen bytes a pixel of the frame NR was given: the
 * change in three halves, and what NR was given there as `likeness` measures it in three
 * more. */
struct texel
{
    float3 change;
    float3 given;
};

texel change_texel(ByteAddressBuffer changes, int2 i)
{
    uint4 d = changes.Load4(i.y * change_pitch + i.x * 16);
    texel t;
    t.change = float3(f16tof32(d.x), f16tof32(d.x >> 16), f16tof32(d.y));
    t.given = float3(f16tof32(d.z), f16tof32(d.z >> 16), f16tof32(d.w));
    return t;
}

/* Bilinear between the change's pixel centres. */
texel change_at(ByteAddressBuffer changes, float2 g)
{
    float2 extent = float2(width, height);
    float2 c = clamp(g, 0, extent - 1);
    int2 i = int2(floor(c));
    int2 j = min(i + 1, int2(extent) - 1);
    float2 f = c - floor(c);
    texel a = change_texel(changes, i), b = change_texel(changes, int2(j.x, i.y));
    texel d = change_texel(changes, int2(i.x, j.y)), e = change_texel(changes, j);
    texel t;
    t.change = lerp(lerp(a.change, b.change, f.x), lerp(d.change, e.change, f.x), f.y);
    t.given = lerp(lerp(a.given, b.given, f.x), lerp(d.given, e.given, f.x), f.y);
    return t;
}

/* One change for this pixel, and how far to trust it: read where the tracker says its
 * surface was on the change's frame, trusted as far as what NR was given there looks like
 * this frame round here — inside the range of the pixel's 3x3 neighbourhood, as temporal
 * upscalers clamp their history, so a jittered edge still matches itself. A surface come into
 * view, one uncovered behind something that moved, a look the vectors do not carry: not
 * trusted, and APPLY fills it. */
float4 trusted(ByteAddressBuffer changes, ByteAddressBuffer places, float2 offset, int2 p,
               float3 low, float3 high)
{
    float4 found = float4(0, 0, 0, 0);
    if (!warp) {
        found = float4(change_texel(changes, p).change, 1);
    } else {
        float2 g = float2(p) + asfloat(places.Load2((p.y * width + p.x) * 8)) + offset;
        float2 extent = float2(width, height);
        if (all(g >= -0.5) && all(g <= extent - 0.5)) {
            texel d = change_at(changes, g);
            float3 outside3 = max(low - d.given, d.given - high);
            float outside = max(outside3.r, max(outside3.g, outside3.b));
            found = float4(d.change, saturate((match_to - outside) / (match_to - match_from)));
        }
    }
    return found;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint size = kind == 0 ? 8 : 4;
    float3 own = likeness(load_texel(id.y * pitch + id.x * size).rgb);
    float3 low = own, high = own;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int2 q = clamp(int2(id.xy) + int2(dx, dy), 0, int2(width, height) - 1);
            float3 l = likeness(load_texel(q.y * pitch + q.x * size).rgb);
            low = min(low, l);
            high = max(high, l);
        }
    float4 now = trusted(change, tracker, shift, int2(id.xy), low, high);
    float3 sum = now.rgb * now.a;
    float trust = now.a;
    if (fading) {  /* the change before, giving way over the frames until the next arrives */
        float4 then = trusted(old_change, old_tracker, old_shift, int2(id.xy), low, high);
        sum = lerp(then.rgb * then.a, sum, fade);
        trust = lerp(then.a, trust, fade);
    }
    uint at = (id.y * width + id.x) * MATCHED;
    matched.Store4(at, asuint(float4(sum, trust)));
    matched.Store3(at + 16, asuint(own));
}

#endif

#ifdef REDUCE

ByteAddressBuffer matched : register(t0);
ByteAddressBuffer motion : register(t1);       /* this frame's vectors, when `vectors` */
RWByteAddressBuffer sums : register(u0);

/* One thread a cell: the trusted change, the trust, and the trust-weighted likeness and
 * vectors, summed row by row over the cell's pixels. */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= cells.x || id.y >= cells.y)
        return;
    float3 change_sum = 0, alike = 0;
    float2 moving = 0;
    float trust = 0;
    for (uint y = id.y * cell; y < min((id.y + 1) * cell, height); y++)
        for (uint x = id.x * cell; x < min((id.x + 1) * cell, width); x++) {
            uint at = (y * width + x) * MATCHED;
            float4 m = asfloat(matched.Load4(at));
            change_sum += m.rgb;
            trust += m.a;
            alike += m.a * asfloat(matched.Load3(at + 16));
            if (vectors)
                moving += m.a * vector_in(motion, float2(x, y) - jitter);
        }
    uint at = (id.y * cells.x + id.x) * SUMMED;
    sums.Store4(at, asuint(float4(change_sum, trust)));
    sums.Store3(at + 16, asuint(alike));
    sums.Store2(at + 28, asuint(moving));
}

#endif

#ifdef APPLY

ByteAddressBuffer matched : register(t1);
ByteAddressBuffer sums : register(t2);
ByteAddressBuffer motion : register(t3);       /* this frame's vectors, when `vectors` */

/* The trusted pixels' worth of support below which a fill is weaker than the change it reads:
 * a surface with little of its own trusted round it takes little, rather than all of what
 * some unlike cell happened to hold. */
#define SUPPORT 8

/* The change where MATCH did not trust its own: the trusted change of the cells round the
 * pixel, each by how much of it was trusted, how near its centre is, how alike its pixels
 * are to this one and how alike they move — so a background uncovered behind something that
 * moved takes the background's change, not the change of what moved, however alike the two
 * look. Less where little round it was trusted, nothing where nothing was. */
float3 fill(int2 p, float3 own)
{
    int2 home = int2(uint2(p) / cell);
    float2 mine = vectors ? vector_in(motion, float2(p) - jitter) : 0;
    float reach = spread(mine);
    float3 sum = 0;
    float total = 0;
    for (int cy = -1; cy <= 1; cy++)
        for (int cx = -1; cx <= 1; cx++) {
            int2 c = home + int2(cx, cy);
            if (any(c < 0) || any(c >= int2(cells)))
                continue;
            uint at = (c.y * cells.x + c.x) * SUMMED;
            float4 s = asfloat(sums.Load4(at));
            if (s.a < 1e-3)
                continue;
            float3 alike = asfloat(sums.Load3(at + 16)) / s.a - own;
            float2 centre = (float2(c) + 0.5) * cell - 0.5;
            float near = saturate(1.5 - length(float2(p) - centre) / cell);
            float weight = s.a * near * exp(-dot(alike, alike) / (0.04 * 0.04));
            if (vectors) {
                float2 apart = asfloat(sums.Load2(at + 28)) / s.a - mine;
                weight *= exp(-dot(apart, apart) / (reach * reach));
            }
            sum += weight * s.rgb / s.a;
            total += weight;
        }
    return sum / max(total, SUPPORT);
}

/* Where the proxy's knee compressed a pixel the change is scaled back up by the same ratio
 * (`knee_gain`). */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint at = id.y * pitch + id.x * (kind == 0 ? 8 : 4);
    float4 c = load_texel(at);
    uint here = (id.y * width + id.x) * MATCHED;
    float4 m = asfloat(matched.Load4(here));
    float3 delta = m.rgb;
    if (m.a < 1)
        delta += (1 - m.a) * fill(int2(id.xy), asfloat(matched.Load3(here + 16)));
    c.rgb = max(c.rgb + white * strength * knee_gain(c.rgb) * delta, 0);
    store_texel(at, c);
}

#endif

#ifdef ENCODE

ByteAddressBuffer srgb : register(t1);         /* nr_xess.c `srgb8`, packed four to a word */
RWByteAddressBuffer given : register(u0);      /* the frame NR is given, R8G8B8A8 */

#define SRGB_STEPS 16384

uint srgb_byte(float v)
{
    uint i = v >= 1 ? SRGB_STEPS : uint(v * SRGB_STEPS + 0.5);
    return (srgb.Load(i & ~3u) >> ((i & 3u) * 8)) & 0xFF;
}

/* nr_xess.c `encode_proxy` on the GPU: over white, the knee on luminance, then sRGB through
 * the CPU's own table, so that the daemon sees what mode=carry hands it. */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    float3 d = max(load_texel(id.y * pitch + id.x * (kind == 0 ? 8 : 4)).rgb, 0) / white;
    float lit = dot(d, float3(0.2126, 0.7152, 0.0722));
    if (lit > 0.75)
        d *= (0.75 + 0.25 * (1 - exp(-(lit - 0.75) / 0.25))) / lit;
    given.Store((id.y * width + id.x) * 4,
                srgb_byte(d.r) | (srgb_byte(d.g) << 8) | (srgb_byte(d.b) << 16) | 0xFF000000);
}

#endif

#ifdef COMPOSE

ByteAddressBuffer given : register(t1);        /* the frame NR was given, R8G8B8A8 */
ByteAddressBuffer answer : register(t2);       /* its answer, R8G8B8A8, and the daemon's stamp after it */
ByteAddressBuffer linear8 : register(t3);      /* nr_xess.c `linear8`: an sRGB byte's linear value */

#define STAMP_MAGIC 0x5453524E                 /* nr_gpu_wire.h `NR_GPU_STAMP_MAGIC` */

float3 linear_of(uint rgba)
{
    return float3(asfloat(linear8.Load((rgba & 0xFF) * 4)), asfloat(linear8.Load(((rgba >> 8) & 0xFF) * 4)),
                  asfloat(linear8.Load(((rgba >> 16) & 0xFF) * 4)));
}

/* The colour XeSS gets: the game's, with NR's change to this very frame added as APPLY adds a
 * carried one, when the stamp after the answer names this frame; otherwise the game's as it
 * was, the daemon having declined it or not come. */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint at = id.y * pitch + id.x * (kind == 0 ? 8 : 4);
    float4 c = load_texel(at);
    uint4 stamp = answer.Load4(width * height * 4);
    if (stamp.x == STAMP_MAGIC && stamp.y == frame.x && stamp.z == frame.y && strength > 0) {
        uint p = (id.y * width + id.x) * 4;
        float3 delta = linear_of(answer.Load(p)) - linear_of(given.Load(p));
        c.rgb = max(c.rgb + white * strength * knee_gain(c.rgb) * delta, 0);
    }
    store_texel(at, c);
}

#endif

#ifdef MOTION

ByteAddressBuffer motion : register(t0);       /* this frame's vectors, R16G16_FLOAT */
RWByteAddressBuffer shared_input : register(u0);   /* the frame NR is given, then the offsets */

/* For each pixel of the frame NR is given, the offset in its pixels to where its surface was
 * on the frame NR was given before: TRACK's one step from a fresh tracker, the vector at the
 * unjittered position and the two frames' jitter. NaN where that was off screen, as
 * nr_xess.c `measure_motion` hands the daemon (nr_gpu_wire.h, "Motion"); the daemon tells a
 * surface uncovered behind something that moved by the picture, as its history does. */
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    float2 m = vector_in(motion, float2(id.xy) - jitter);
    float2 d = m + previous_jitter - jitter;
    float2 g = float2(id.xy) + d;
    bool off = g.x < -0.5 || g.y < -0.5 || g.x > width - 0.5 || g.y > height - 0.5;
    float nan = asfloat(0x7FC00000u);
    shared_input.Store2(offsets + (id.y * width + id.x) * 8, asuint(off ? float2(nan, nan) : d));
}

#endif
