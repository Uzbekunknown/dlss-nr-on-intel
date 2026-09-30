/* CPU image passes. Keep the NumPy operation order and every FP16 rounding point.
 * Built locally for the host CPU; no fast-math or fused multiply-add is allowed.
 * Allocation, shape validation and buffer lifetime belong to nr_image.py.
 *
 * Taken from the parallel ProjectsCodex tree, which wrote it and measured it
 * (`notes/phase57`): feature assembly 146 -> 24 ms, composition 53 -> 9, the two
 * resizes 60 -> 8. Extended here for the two passes this tree has and that one does
 * not — history in the feature channels, and the temporal composition with its floor.
 *
 * Every function is a transcription of the NumPy above it, not a reimplementation.
 * The outputs are required to be byte-identical, and `test_native_image.py` checks it.
 *
 * Each outer row loop is an OpenMP `parallel for`. No row reads another row's result,
 * so which thread computes a row changes nothing about its bytes; at a 1080p output it
 * changes the composition from 15.5 ms to 3.3 on this machine's eight cores. Rows are
 * handed out four at a time as threads come free rather than split evenly up front: four
 * of the eight cores are low-power ones, and an even split left the others waiting on
 * them — the fused composition 2.6 -> 2.2 ms at 1280x720. The two loops over single
 * pixels or elements keep the even split, where a chunk of four is all overhead.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static float half(float value) { return (float)(_Float16)value; }

static float unit(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/* Toward zero onto the half grid, which is what the vendor's RGBA16F history keeps of the
 * prediction (`nr_frame.truncate_half`): a normal half keeps ten mantissa bits, a subnormal
 * one is a multiple of 2^-24. Selects rather than branches, so the fused pass vectorises. */
static inline float truncate_half(float x)
{
    uint32_t f, t;
    memcpy(&f, &x, sizeof f);
    t = f & 0xffffe000u;
    float kept, small = floorf(fabsf(x) * 16777216.0f) * 0x1p-24f;
    memcpy(&kept, &t, sizeof kept);
    small = x < 0.0f ? -small : small;
    return (f & 0x7fffffffu) >= 0x38800000u ? kept : small;
}

static inline float grade_unit(float value)
{
    value = value < 0.0f ? 0.0f : value;
    return value > 1.0f ? 1.0f : value;
}

static inline __attribute__((always_inline)) float hue_channel(float p, float q, float t)
{
    float up = t + 1.0f;
    t = t < 0.0f ? up : t;
    float down = t - 1.0f;
    t = t > 1.0f ? down : t;
    float rising = p + t * ((q - p) * 6.0f);
    float falling = p + (2.0f / 3.0f - t) * (q - p) * 6.0f;
    return t < 1.0f / 6.0f ? rising : t < 0.5f ? q : t < 2.0f / 3.0f ? falling : p;
}

/* The vendor's colour grade for a style on one pixel, `nr_frame.style_grade` operation for
 * operation: per channel an exposure and a smoothstep contrast, then a saturation
 * multiplier in HSL. `grade` is `nr_frame.grade_for`'s three factors. The stages of
 * `cg2r_post_process_kernel` no style moves are identities and are not here
 * (`notes/phase70-post-process.md`). Every branch is a select, as NumPy's `where` computes
 * both sides — the unused one on a stand-in divisor — so the fused rows vectorise it. */
static inline __attribute__((always_inline)) void style_grade(float c[3], const float *grade)
{
    const float exposure = grade[0], contrast = grade[1], saturation = grade[2];
    for (int i = 0; i < 3; ++i) {
        float v = grade_unit(grade_unit(c[i]) * exposure);
        c[i] = grade_unit(v + contrast * (v * v * (3.0f - (v + v)) - v));
    }
    float r = c[0], g = c[1], b = c[2];
    float high = r > g ? r : g, low = r < g ? r : g;
    high = high > b ? high : b;
    low = low < b ? low : b;
    float light = (high + low) * 0.5f;
    float delta = high - low;
    int grey = !(high > low);
    float spread = grey ? 1.0f : delta;
    float bright = (2.0f - high) - low, dark = high + low;
    float by_bright = delta / (grey ? 1.0f : bright), by_dark = delta / (grey ? 1.0f : dark);
    float chroma = light > 0.5f ? by_bright : by_dark;
    float from_r = ((g - b) / spread + (g < b ? 6.0f : 0.0f)) / 6.0f;
    float from_g = ((b - r) / spread + 2.0f) / 6.0f;
    float from_b = ((r - g) / spread + 4.0f) / 6.0f;
    float hue = grey ? 0.0f : high == r ? from_r : high == g ? from_g : from_b;
    chroma = grade_unit((grey ? 0.0f : chroma) * saturation);
    float q_dark = light * (chroma + 1.0f), q_bright = (light + chroma) - light * chroma;
    float q = light < 0.5f ? q_dark : q_bright;
    float p = (light + light) - q;
    int flat = chroma <= 0.0f;
    float lit = grade_unit(light);
    c[0] = flat ? lit : grade_unit(hue_channel(p, q, hue + 1.0f / 3.0f));
    c[1] = flat ? lit : grade_unit(hue_channel(p, q, hue));
    c[2] = flat ? lit : grade_unit(hue_channel(p, q, hue - 1.0f / 3.0f));
}

/* The prediction after the history's blend, as the vendor goes on with it
 * (`nr_frame._vendor_prediction`): kept in `neural` truncated to half, the next frame's
 * history, and that half through the style's grade; then the blend back towards the frame.
 * With neither `grade` nor `neural` it is the blend alone, as it always was. */
static inline void finish_pixel(float predicted[3], const float *rgb, ptrdiff_t sc,
                                float blend, const float *grade, float *neural, float *out)
{
    if (grade || neural) {
        float kept[3];
        for (int c = 0; c < 3; ++c) kept[c] = truncate_half(predicted[c]);
        if (neural)
            for (int c = 0; c < 3; ++c) neural[c] = kept[c];
        if (grade) {
            style_grade(kept, grade);
            for (int c = 0; c < 3; ++c) predicted[c] = kept[c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        float source = rgb[(ptrdiff_t)c * sc];
        out[c] = unit(source + blend * (predicted[c] - source));
    }
}

void nr_decode8(const uint8_t *source, size_t pixels, int bgra, float *output)
{
    #pragma omp parallel for schedule(static)
    for (size_t p = 0; p < pixels; ++p) {
        output[p * 3] = (float)source[p * 4 + (bgra ? 2 : 0)] / 255.0f;
        output[p * 3 + 1] = (float)source[p * 4 + 1] / 255.0f;
        output[p * 3 + 2] = (float)source[p * 4 + (bgra ? 0 : 2)] / 255.0f;
    }
}

void nr_encode8(const float *image, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                 const uint8_t *raw, size_t height, size_t width, int bgra,
                 uint8_t *output)
{
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const float *rgb = image + (ptrdiff_t)y * sy + (ptrdiff_t)x * sx;
            size_t p = y * width + x;
            for (size_t c = 0; c < 3; ++c) {
                float value = rgb[(ptrdiff_t)c * sc];
                /* NumPy's byte cast maps NaN to zero; do not cast NaN in C. */
                value = value == value ? unit(value) : 0.0f;
                output[p * 4 + (bgra ? 2 - c : c)] = (uint8_t)(value * 255.0f + 0.5f);
            }
            output[p * 4 + 3] = raw[p * 4 + 3];
        }
    }
}

void nr_compose(const float *head, ptrdiff_t hy, ptrdiff_t hx, ptrdiff_t hc,
                const float *colour, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                size_t height, size_t width, float intensity,
                const float *grade, float *neural, float *output)
{
    /* Our intensity > 1 extrapolates; the vendor recipe clamps negative intensity.
     * Keep both subtract/add operations even at intensity 1: simplifying to the
     * prediction would remove an FP32 rounding and can change encoded pixels.
     */
    float blend = intensity > 1.0f ? intensity : unit(intensity);
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const float *h = head + (ptrdiff_t)y * hy + (ptrdiff_t)x * hx;
            const float *rgb = colour + (ptrdiff_t)y * sy + (ptrdiff_t)x * sx;
            float predicted[3];
            for (size_t c = 0; c < 3; ++c)
                predicted[c] = unit(rgb[(ptrdiff_t)c * sc] + half(h[(ptrdiff_t)c * hc]) * 0.25f);
            finish_pixel(predicted, rgb, sc, blend, grade,
                         neural ? neural + (y * width + x) * 3 : NULL,
                         output + (y * width + x) * 3);
        }
    }
}

/* `history` is this tree's addition: the previous output, at the same logical extent as
 * the colour and mirrored onto the network extent the same way, standing in channels 7-9
 * where the first-frame layout repeats the colour. Identity reprojection, because a
 * layer at vkQueuePresentKHR has no motion vectors (notes/phase54). NULL for a still
 * frame, which is then bit-identical to the vendor's own first-frame layout.
 */
/* The sixteen channels of one pixel, as float. Both stores below are this. */
static inline void feature_pixel(const float *rgb, ptrdiff_t sc, const float *was,
                                 ptrdiff_t tc, const float *noise, const float *controls,
                                 float out[16])
{
    for (size_t c = 0; c < 3; ++c) {
        float scaled = half(half(half(rgb[(ptrdiff_t)c * sc]) - 0.5f) * 0.125f);
        out[c] = noise[c];
        out[4 + c] = scaled;
        out[7 + c] = was
            ? half(half(half(was[(ptrdiff_t)c * tc]) - 0.5f) * 0.125f)
            : scaled;
    }
    out[3] = 1.0f;
    for (size_t c = 0; c < 5; ++c) out[10 + c] = controls[c];
    out[15] = 0.0f;
}

void nr_features(const float *colour, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                 const float *history, ptrdiff_t ty, ptrdiff_t tx, ptrdiff_t tc,
                 const int32_t *rows, const int32_t *columns,
                 size_t height, size_t width, const float *noise,
                 const float *controls, float *output)
{
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        const float *row = colour + rows[y] * sy;
        const float *old = history ? history + rows[y] * ty : 0;
        for (size_t x = 0; x < width; ++x) {
            size_t pixel = y * width + x;
            feature_pixel(row + columns[x] * sx, sc, old ? old + columns[x] * tx : 0, tc,
                          noise + pixel * 3, controls, output + pixel * 16);
        }
    }
}

/* The same features stored as half, which is what the graph's first GEMM reads: the
 * to_half pass the GPU ran over them goes, and so do half the bytes written here. Every
 * rounding is to nearest even, as that pass's, so the half values are its values
 * (test_native_image.py, test_input_fp16.py). */
void nr_features_half(const float *colour, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                      const float *history, ptrdiff_t ty, ptrdiff_t tx, ptrdiff_t tc,
                      const int32_t *rows, const int32_t *columns,
                      size_t height, size_t width, const float *noise,
                      const float *controls, _Float16 *output)
{
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        const float *row = colour + rows[y] * sy;
        const float *old = history ? history + rows[y] * ty : 0;
        for (size_t x = 0; x < width; ++x) {
            size_t pixel = y * width + x;
            float values[16];
            feature_pixel(row + columns[x] * sx, sc, old ? old + columns[x] * tx : 0, tc,
                          noise + pixel * 3, controls, values);
            _Float16 *out = output + pixel * 16;
            for (size_t c = 0; c < 16; ++c) out[c] = (_Float16)values[c];
        }
    }
}

/* float32 to half, rounding to nearest even — for features a caller built as float. */
void nr_to_half(const float *source, size_t count, _Float16 *target)
{
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < count; ++i) target[i] = (_Float16)source[i];
}

/* The area mean of a downscale by whole factors — `nr_daemon.resample`'s other branch,
 * which averages instead of sampling so the network is not handed aliasing to enhance.
 * The order is that NumPy code's: a block's first sample, each other one added in
 * row-major order, then one division by the count. */
void nr_area_mean(const float *source, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                  size_t height, size_t width, size_t channels, size_t fy, size_t fx,
                  float *output)
{
    float count = (float)(fy * fx);
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const float *block = source + (ptrdiff_t)(y * fy) * sy + (ptrdiff_t)(x * fx) * sx;
            float *out = output + (y * width + x) * channels;
            for (size_t c = 0; c < channels; ++c) {
                const float *first = block + (ptrdiff_t)c * sc;
                float total = first[0];
                for (size_t dy = 0; dy < fy; ++dy)
                    for (size_t dx = 0; dx < fx; ++dx)
                        if (dy || dx)
                            total += first[(ptrdiff_t)dy * sy + (ptrdiff_t)dx * sx];
                out[c] = total / count;
            }
        }
    }
}

/* One axis at a time: the intermediate is deliberately rounded to FP32 before
 * the second axis. Coordinates/weights come from the unchanged NumPy formula.
 * Signed strides permit padded crops and reversed views without another copy.
 */
void nr_resize_axis(const float *source, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                    size_t height, size_t width, size_t channels, int axis,
                    const int32_t *low, const int32_t *high,
                    const float *weight, float *output)
{
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        if (axis == 0 && sx == (ptrdiff_t)channels && sc == 1) {
            const float *a = source + low[y] * sy;
            const float *b = source + high[y] * sy;
            float w = weight[y], other = 1.0f - w;
            for (size_t i = 0; i < width * channels; ++i)
                output[y * width * channels + i] = a[i] * other + b[i] * w;
        } else {
            for (size_t x = 0; x < width; ++x) {
                size_t index = axis == 0 ? y : x;
                const float *a = axis == 0 ? source + low[y] * sy + (ptrdiff_t)x * sx
                                          : source + (ptrdiff_t)y * sy + low[x] * sx;
                const float *b = axis == 0 ? source + high[y] * sy + (ptrdiff_t)x * sx
                                          : source + (ptrdiff_t)y * sy + high[x] * sx;
                float w = weight[index], other = 1.0f - w;
                for (size_t c = 0; c < channels; ++c)
                    output[(y * width + x) * channels + c] =
                        a[(ptrdiff_t)c * sc] * other + b[(ptrdiff_t)c * sc] * w;
            }
        }
    }
}


/* The temporal composition, which this tree has and the other does not.
 *
 * The model's own history weight comes from `table`: NumPy's sigmoid evaluated once on
 * every half value, indexed here by the half logit's sixteen bits. `expf` and NumPy's
 * float32 exponential do not agree in the last bit, and the contract for all of this is
 * byte-identical output, not nearly — but the logit is rounded to half before the
 * sigmoid, so there are only 65536 inputs and NumPy can own every one of them.
 * `confidence` then scales it, as NumPy does. With no table, `gate` carries the weight
 * already computed, confidence and all.
 *
 * `previous` is the game's own frame from the present before, or NULL. Where it is
 * unchanged the history is right for that pixel by construction, so the gate gets a
 * floor: full at no change, gone by four levels of 255, never above `scale`
 * (notes/phase54). `mask` is the interface control mask's red channel, or NULL.
 */
/* One pixel of the temporal composition, as nr_compose_temporal describes it: `h` the
 * head's four channels, `gate_at` this pixel's gate where there is no table, `before` the
 * game's previous pixel or NULL, `mask_at` the control mask's red here or NULL. Shared by
 * nr_compose_temporal and the fused pass below, so the two are one arithmetic. */
static inline void temporal_pixel(const float *h, ptrdiff_t hc,
                                  const float *rgb, ptrdiff_t sc,
                                  const float *was, ptrdiff_t rc,
                                  const float *before, ptrdiff_t pc,
                                  const float *gate_at, const float *table, float confidence,
                                  const float *mask_at, float intensity,
                                  float scale, float hold, float slope, float release,
                                  const float *grade, float *neural, float *out)
{
    /* What the game itself did to this pixel since its previous frame: the
     * largest step of the three channels. Both the floor and the release read it. */
    float moved = 0.0f;
    if (before) {
        for (size_t c = 0; c < 3; ++c) {
            float step = rgb[(ptrdiff_t)c * sc] - before[(ptrdiff_t)c * pc];
            if (step < 0.0f) step = -step;
            if (step > moved) moved = step;
        }
    }
    float alpha;
    if (table) {
        /* The model's gate looked up rather than recomputed: `nr_frame.gate_table`
         * holds NumPy's own expression evaluated on every half value, so the exp
         * that kept the gate in NumPy is inside the table, and the rounding to half
         * here is the one `half` does. Then the confidence, as NumPy applies it. */
        _Float16 logit = (_Float16)h[3 * hc];
        uint16_t bits;
        memcpy(&bits, &logit, sizeof bits);
        alpha = table[bits];
        if (confidence != 1.0f) alpha *= confidence;
    } else {
        alpha = *gate_at;
    }
    if (before && release != 0.0f) {
        /* The release: where the game's pixel changed, what the previous output
         * holds there is something that has since moved, and the gate is not local
         * enough to know it — it reads 0.6 over a whole Tekken frame. Its share
         * fades from all of it at no change to none by `-1 / release`, folded like
         * `slope` and for the same reason. Before the floor, which it never lowers. */
        float kept = moved * release + 1.0f;
        if (kept < 0.0f) kept = 0.0f;
        if (kept > 1.0f) kept = 1.0f;
        alpha *= kept;
    }
    if (before) {
        /* `moved * slope + hold`, clamped to [0, hold], and `slope` arrives
         * already folded: NumPy multiplies by one constant and adds another,
         * and `clip(1 - moved * 255 / ramp, 0, 1) * hold` is the same value by
         * algebra and a different one in float32. */
        float floored = moved * slope + hold;
        if (floored < 0.0f) floored = 0.0f;
        if (floored > hold) floored = hold;
        if (floored > 1.0f) floored = 1.0f;     /* `compose` clips the floor to [0, 1] */
        floored *= scale;
        if (floored > alpha) alpha = floored;
    }
    /* No clamp on the blend: the NumPy this transcribes does not clamp it
     * either, and the vendor's clamp is the one thing our composition
     * deliberately drops so that intensity above 1 can extrapolate. */
    float blend = intensity;
    if (mask_at) blend *= *mask_at;
    float predicted[3];
    for (size_t c = 0; c < 3; ++c) {
        predicted[c] = unit(rgb[(ptrdiff_t)c * sc] + half(h[(ptrdiff_t)c * hc]) * 0.25f);
        predicted[c] += alpha * (was[(ptrdiff_t)c * rc] - predicted[c]);
    }
    finish_pixel(predicted, rgb, sc, blend, grade, neural, out);
}

/* The temporal composition, which this tree has and the other does not.
 *
 * The model's own history weight comes from `table`: NumPy's sigmoid evaluated once on
 * every half value, indexed here by the half logit's sixteen bits. `expf` and NumPy's
 * float32 exponential do not agree in the last bit, and the contract for all of this is
 * byte-identical output, not nearly — but the logit is rounded to half before the
 * sigmoid, so there are only 65536 inputs and NumPy can own every one of them.
 * `confidence` then scales it, as NumPy does. With no table, `gate` carries the weight
 * already computed, confidence and all.
 *
 * `previous` is the game's own frame from the present before, or NULL. Where it is
 * unchanged the history is right for that pixel by construction, so the gate gets a
 * floor: full at no change, gone by four levels of 255, never above `scale`
 * (notes/phase54). `mask` is the interface control mask's red channel, or NULL.
 */
void nr_compose_temporal(const float *head, ptrdiff_t hy, ptrdiff_t hx, ptrdiff_t hc,
                         const float *colour, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                         const float *history, ptrdiff_t ry, ptrdiff_t rx, ptrdiff_t rc,
                         const float *previous, ptrdiff_t py, ptrdiff_t px, ptrdiff_t pc,
                         const float *gate, ptrdiff_t gy, ptrdiff_t gx,
                         const float *table, float confidence,
                         const float *mask, ptrdiff_t my, ptrdiff_t mx,
                         size_t height, size_t width, float intensity,
                         float scale, float hold, float slope, float release,
                         const float *grade, float *neural, float *output)
{
    #pragma omp parallel for schedule(dynamic, 4)
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            temporal_pixel(head + (ptrdiff_t)y * hy + (ptrdiff_t)x * hx, hc,
                           colour + (ptrdiff_t)y * sy + (ptrdiff_t)x * sx, sc,
                           history + (ptrdiff_t)y * ry + (ptrdiff_t)x * rx, rc,
                           previous ? previous + (ptrdiff_t)y * py + (ptrdiff_t)x * px : NULL, pc,
                           table ? NULL : gate + (ptrdiff_t)y * gy + (ptrdiff_t)x * gx,
                           table, confidence,
                           mask ? mask + (ptrdiff_t)y * my + (ptrdiff_t)x * mx : NULL,
                           intensity, scale, hold, slope, release, grade,
                           neural ? neural + (y * width + x) * 3 : NULL,
                           output + (y * width + x) * 3);
        }
    }
}

/* One row of `nr_compose_encode` in the layout the daemon hands it: RGB float triples for
 * the colour, the history and the game's previous frame, the head's row already scaled on
 * the first axis with its channels adjacent, the gate from the table, no control mask.
 * The strides are constants here and the pixels independent, so the compiler vectorises
 * across them — eight pixels an instruction where the general loop took one — and each
 * pixel's arithmetic is `temporal_pixel`'s, or `nr_compose`'s without a history, operation
 * for operation: the same bytes, `test_native_image.py`. 22.7 ms on one core at 1080p for
 * the general loop, all of it arithmetic. */
/* A float rounded to half and back, and the half's sixteen bits, with no `_Float16`: this
 * CPU has no vector half arithmetic, and a conversion the vectoriser cannot take keeps the
 * whole loop scalar. Normal halves keep ten mantissa bits, ties to even; subnormal ones are
 * multiples of 2^-24, which the float adder's own rounding finds at 0.5; past 65520 is
 * infinity, and a NaN keeps its sign and its top payload bits, quieted — the hardware's
 * conversion, which both match for every one of the 2^32 floats. */
static inline float half_value(float x)
{
    uint32_t f, sb;
    memcpy(&f, &x, sizeof f);
    uint32_t sign = f & 0x80000000u, a = f & 0x7fffffffu;
    uint32_t normal = (a + 0x0fffu + ((a >> 13) & 1u)) & 0xffffe000u;
    float magnitude, tiny;
    memcpy(&magnitude, &a, sizeof a);
    tiny = (magnitude + 0.5f) - 0.5f;
    memcpy(&sb, &tiny, sizeof sb);
    uint32_t bits = a > 0x7f800000u ? 0x7fc00000u | (a & 0x003fe000u)
                  : a >= 0x477ff000u ? 0x7f800000u
                  : a >= 0x38800000u ? normal : sb;
    bits |= sign;
    float y;
    memcpy(&y, &bits, sizeof y);
    return y;
}

static inline uint32_t half_bits(float x)
{
    uint32_t f;
    memcpy(&f, &x, sizeof f);
    uint32_t sign = (f >> 16) & 0x8000u, a = f & 0x7fffffffu;
    uint32_t normal = (a + 0x0fffu + ((a >> 13) & 1u)) & 0xffffe000u;
    float magnitude;
    memcpy(&magnitude, &a, sizeof a);
    float tiny = (magnitude + 0.5f) - 0.5f;
    uint32_t bits = a > 0x7f800000u ? 0x7e00u | ((a >> 13) & 0x1ffu)
                  : a >= 0x477ff000u ? 0x7c00u
                  : a >= 0x38800000u ? (normal >> 13) - 0x1c000u
                  : (uint32_t)(tiny * 16777216.0f);
    return bits | sign;
}

static inline float clamp01(float value)
{
    value = value < 0.0f ? 0.0f : value;
    return value > 1.0f ? 1.0f : value;
}

/* `nr_compose_encode`'s pixel after the head's upscale, for one layout of the knobs: the
 * bodies of `temporal_pixel` (or `nr_compose` without a history) and of the encoder,
 * written as selects rather than branches so the compiler can run eight pixels at once.
 * `confidence` multiplies unconditionally: at 1 that is exact for every value the table
 * holds. */
static inline __attribute__((always_inline)) void
compose_encode_pixel(const float *restrict hq, const float *restrict rgb, const float *restrict was,
                     const float *restrict before, const float *restrict table,
                     float confidence, float intensity, float still, float scale, float hold,
                     float slope, float release,
                     float *restrict out, float *restrict kept,
                     uint8_t *restrict pixel, int x, const int temporal, const int moving,
                     const int releasing, const int grading, const int bgra)
{
    const float *p = rgb + 3 * x;
    float h[3] = { hq[4 * x], hq[4 * x + 1], hq[4 * x + 2] };
    float o[3], predicted[3];
    if (temporal) {
        float moved = 0.0f;
        if (moving) {
            for (int c = 0; c < 3; ++c) {
                float step = p[c] - before[3 * x + c];
                step = step < 0.0f ? -step : step;
                moved = step > moved ? step : moved;
            }
        }
        float alpha = table[half_bits(hq[4 * x + 3])] * confidence;
        if (releasing)
            alpha *= clamp01(moved * release + 1.0f);
        if (moving) {
            float floored = moved * slope + hold;
            floored = floored < 0.0f ? 0.0f : floored;
            floored = floored > hold ? hold : floored;
            floored = floored > 1.0f ? 1.0f : floored;
            floored *= scale;
            alpha = floored > alpha ? floored : alpha;
        }
        for (int c = 0; c < 3; ++c) {
            predicted[c] = clamp01(p[c] + half_value(h[c]) * 0.25f);
            predicted[c] += alpha * (was[3 * x + c] - predicted[c]);
        }
    } else {
        for (int c = 0; c < 3; ++c)
            predicted[c] = clamp01(p[c] + half_value(h[c]) * 0.25f);
    }
    /* `finish_pixel`, with the history always kept: into the caller's row, or a row nobody
     * reads when there is no history to keep. A grading row stops there, and
     * `grade_blend_row` takes it on from the kept half. */
    for (int c = 0; c < 3; ++c) kept[3 * x + c] = truncate_half(predicted[c]);
    if (grading) return;
    const float blend = temporal ? intensity : still;
    for (int c = 0; c < 3; ++c) o[c] = clamp01(p[c] + blend * (predicted[c] - p[c]));
    for (int c = 0; c < 3; ++c) out[3 * x + c] = o[c];
    for (int c = 0; c < 3; ++c) {
        /* NumPy's byte cast maps NaN to zero; do not cast NaN in C. */
        float value = o[c] == o[c] ? clamp01(o[c]) : 0.0f;
        pixel[4 * x + (bgra ? 2 - c : c)] = (uint8_t)(value * 255.0f + 0.5f);
    }
}

/* A grading row's grade, over the row in a loop of its own. Inside the composition's pixel
 * it would not vectorise, and it took the whole row's composition down to one pixel at a
 * time with it: 47 -> 66 ms a 1080p frame. GCC's jump threading turns the grade's chain of
 * clamps into branches its vectoriser cannot take back, so it is off for this loop alone;
 * it moves no arithmetic, and the bytes are checked (`test_native_image.py`). */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-thread-jumps")))
#endif
static void grade_row(const float *restrict kept, float *restrict graded, int width,
                      const float *restrict grade)
{
    for (int x = 0; x < width; ++x) {
        float c[3] = { kept[3 * x], kept[3 * x + 1], kept[3 * x + 2] };
        style_grade(c, grade);
        for (int i = 0; i < 3; ++i) graded[3 * x + i] = c[i];
    }
}

/* Then the blend back towards the frame and the encoder, as `compose_encode_pixel` ends. */
static inline __attribute__((always_inline)) void
grade_blend_row(const float *restrict rgb, const float *restrict graded, float blend,
                int width, float *restrict out, uint8_t *restrict pixel, const int bgra)
{
    _Pragma("GCC ivdep")
    for (int x = 0; x < width; ++x)
        for (int c = 0; c < 3; ++c) {
            float source = rgb[3 * x + c];
            float o = clamp01(source + blend * (graded[3 * x + c] - source));
            out[3 * x + c] = o;
            /* NumPy's byte cast maps NaN to zero; do not cast NaN in C. */
            float value = o == o ? clamp01(o) : 0.0f;
            pixel[4 * x + (bgra ? 2 - c : c)] = (uint8_t)(value * 255.0f + 0.5f);
        }
}

/* One row of `nr_compose_encode` in the layout the daemon hands it: RGB float triples for
 * the colour, the history and the game's previous frame, the head's row already scaled on
 * the first axis with its channels adjacent, the gate from the table, no control mask.
 * The head's second axis goes into a row of its own, a pixel's channels side by side; then
 * each combination of the knobs and the byte order is its own loop, strides constant, so the
 * compiler vectorises across pixels where the general loop took them one at a time. Each
 * pixel's arithmetic is the general loop's, operation for operation — `half_value` and
 * `half_bits` are the conversion itself, checked on every float — so the bytes are the
 * same (`test_native_image.py`). The general loop spent 22.7 ms of one core on a 1080p
 * frame, all of it arithmetic. */
#define COMPOSE_ROW(temporal, moving, releasing, grading, bgra)                              \
    _Pragma("GCC ivdep")                                                                     \
    for (int x = 0; x < width; ++x)                                                          \
        compose_encode_pixel(rows, rgb, was, before, table, confidence, intensity,           \
                             still, scale, hold, slope, release, out, kept, pixel, x,        \
                             temporal, moving, releasing, grading, bgra)
/* each combination of the knobs, with and without a grade */
#define COMPOSE_ROWS(grading)                                                                \
    if (!was && bgra) COMPOSE_ROW(0, 0, 0, grading, 1);                                      \
    else if (!was) COMPOSE_ROW(0, 0, 0, grading, 0);                                         \
    else if (!before && bgra) COMPOSE_ROW(1, 0, 0, grading, 1);                              \
    else if (!before) COMPOSE_ROW(1, 0, 0, grading, 0);                                      \
    else if (!releasing && bgra) COMPOSE_ROW(1, 1, 0, grading, 1);                           \
    else if (!releasing) COMPOSE_ROW(1, 1, 0, grading, 0);                                   \
    else if (bgra) COMPOSE_ROW(1, 1, 1, grading, 1);                                         \
    else COMPOSE_ROW(1, 1, 1, grading, 0)
static void compose_encode_row(const float *restrict line, int lx,
                               const int32_t *restrict low_x, const int32_t *restrict high_x,
                               const float *restrict weight_x,
                               const float *restrict rgb, const float *restrict was,
                               const float *restrict before, const float *restrict table,
                               float confidence, float intensity, float still, float scale,
                               float hold, float slope, float release,
                               const float *restrict grade, int width,
                               float *restrict rows, float *restrict graded,
                               float *restrict out, float *restrict kept,
                               uint8_t *restrict pixel, int bgra)
{
    /* the head's second axis, its channels side by side — one vector a tap with a history,
     * whose four channels the head row holds; three without, which is all it holds then */
    if (was)
        for (int x = 0; x < width; ++x) {
            float w = weight_x[x], other = 1.0f - w;
            const float *a = line + low_x[x] * lx, *b = line + high_x[x] * lx;
            for (int c = 0; c < 4; ++c) rows[4 * x + c] = a[c] * other + b[c] * w;
        }
    else
        for (int x = 0; x < width; ++x) {
            float w = weight_x[x], other = 1.0f - w;
            const float *a = line + low_x[x] * lx, *b = line + high_x[x] * lx;
            for (int c = 0; c < 3; ++c) rows[4 * x + c] = a[c] * other + b[c] * w;
        }
    int releasing = before && release != 0.0f;
    if (grade) {
        COMPOSE_ROWS(1);
        grade_row(kept, graded, width, grade);
        const float blend = was ? intensity : still;
        if (bgra) grade_blend_row(rgb, graded, blend, width, out, pixel, 1);
        else grade_blend_row(rgb, graded, blend, width, out, pixel, 0);
    } else {
        COMPOSE_ROWS(0);
    }
}
#undef COMPOSE_ROWS
#undef COMPOSE_ROW

/* The head's upscale, the composition and the codec in one pass over the output.
 *
 * Separately they are three passes over the full frame — the bilinear resize writes the
 * head at the output's size through an intermediate, the composition reads it back, and
 * the encoder reads the composition again — about 100 MB of memory traffic a 1280x720
 * frame, where this moves about half of it. Each output row takes the resize's first axis
 * into a row of its own (the same float32 intermediate `nr_resize_axis` stores), then per
 * pixel the second axis, the composition — `temporal_pixel` with a history, `nr_compose`'s
 * arithmetic without — and the encoder's rounding into `encoded`, a copy of the request
 * whose alpha and whose rows and columns outside the active region stay as they came.
 *
 * `low_y`/`high_y`/`weight_y` and the `_x` three are `nr_image._axis_plan`'s, NULL for an
 * axis already at the output's extent; `channels` is the head's, 4 with a history and 3
 * without. The composition is still written to `output`, for the history and the log, and
 * the upscaled head itself on every `step`-th row and column into `samples` when given —
 * the values the log's gate figure reads.
 */
void nr_compose_encode(const float *head, ptrdiff_t hy, ptrdiff_t hx, ptrdiff_t hc,
                       size_t head_width, size_t channels,
                       const int32_t *low_y, const int32_t *high_y, const float *weight_y,
                       const int32_t *low_x, const int32_t *high_x, const float *weight_x,
                       const float *colour, ptrdiff_t sy, ptrdiff_t sx, ptrdiff_t sc,
                       const float *history, ptrdiff_t ry, ptrdiff_t rx, ptrdiff_t rc,
                       const float *previous, ptrdiff_t py, ptrdiff_t px, ptrdiff_t pc,
                       const float *table, float confidence,
                       const float *mask, ptrdiff_t my, ptrdiff_t mx,
                       size_t height, size_t width, float intensity,
                       float scale, float hold, float slope, float release,
                       float *output, const float *grade, float *neural,
                       uint8_t *encoded, size_t frame_width,
                       size_t top, size_t left, int bgra, float *samples, size_t step)
{
    size_t sampled = samples && step ? (width + step - 1) / step : 0;
    /* nr_compose's blend: below 1 clamped to [0, 1], above it extrapolating */
    float still = intensity > 1.0f ? intensity : unit(intensity);
    /* the daemon's layout, which `compose_encode_row` takes with its strides fixed */
    int fast = low_x && sx == 3 && sc == 1 && !mask && channels == (history ? 4u : 3u)
               && (!history || (rx == 3 && rc == 1 && table))
               && (!previous || (px == 3 && pc == 1))
               && width < (1u << 24) && head_width * channels < (1u << 24);
    #pragma omp parallel
    {
        float *row = low_y ? malloc(head_width * channels * sizeof *row) : NULL;
        float *rows = fast ? malloc(4 * width * sizeof *rows) : NULL;
        float *graded = fast && grade ? malloc(3 * width * sizeof *graded) : NULL;
        /* where the fast rows put the history when nobody keeps it */
        float *discard = fast && !neural ? malloc(3 * width * sizeof *discard) : NULL;
        #pragma omp for schedule(dynamic, 4)
        for (size_t y = 0; y < height; ++y) {
            const float *line;
            ptrdiff_t lx, lc;
            if (low_y) {
                float w = weight_y[y], other = 1.0f - w;
                const float *a = head + (ptrdiff_t)low_y[y] * hy;
                const float *b = head + (ptrdiff_t)high_y[y] * hy;
                for (size_t i = 0; i < head_width; ++i)
                    for (size_t c = 0; c < channels; ++c)
                        row[i * channels + c] = a[(ptrdiff_t)i * hx + (ptrdiff_t)c * hc] * other
                                              + b[(ptrdiff_t)i * hx + (ptrdiff_t)c * hc] * w;
                line = row;
                lx = (ptrdiff_t)channels;
                lc = 1;
            } else {
                line = head + (ptrdiff_t)y * hy;
                lx = hx;
                lc = hc;
            }
            if (fast && lc == 1) {
                compose_encode_row(line, (int)lx, low_x, high_x, weight_x,
                                   colour + (ptrdiff_t)y * sy,
                                   history ? history + (ptrdiff_t)y * ry : NULL,
                                   previous ? previous + (ptrdiff_t)y * py : NULL,
                                   table, confidence, intensity, still, scale, hold, slope,
                                   release, grade, (int)width, rows, graded,
                                   output + y * width * 3,
                                   neural ? neural + y * width * 3 : discard,
                                   encoded + ((top + y) * frame_width + left) * 4, bgra);
                if (sampled && y % step == 0) {
                    for (size_t x = 0; x < width; x += step) {
                        float w = weight_x[x], other = 1.0f - w;
                        const float *a = line + (ptrdiff_t)low_x[x] * lx;
                        const float *b = line + (ptrdiff_t)high_x[x] * lx;
                        float *to = samples + ((y / step) * sampled + x / step) * channels;
                        for (size_t c = 0; c < channels; ++c) to[c] = a[c] * other + b[c] * w;
                    }
                }
                continue;
            }
            for (size_t x = 0; x < width; ++x) {
                float h[4];
                if (low_x) {
                    float w = weight_x[x], other = 1.0f - w;
                    const float *a = line + (ptrdiff_t)low_x[x] * lx;
                    const float *b = line + (ptrdiff_t)high_x[x] * lx;
                    for (size_t c = 0; c < channels; ++c)
                        h[c] = a[(ptrdiff_t)c * lc] * other + b[(ptrdiff_t)c * lc] * w;
                } else {
                    const float *a = line + (ptrdiff_t)x * lx;
                    for (size_t c = 0; c < channels; ++c) h[c] = a[(ptrdiff_t)c * lc];
                }
                if (sampled && y % step == 0 && x % step == 0)
                    memcpy(samples + ((y / step) * sampled + x / step) * channels, h,
                           channels * sizeof *h);
                const float *rgb = colour + (ptrdiff_t)y * sy + (ptrdiff_t)x * sx;
                float *out = output + (y * width + x) * 3;
                if (history) {
                    temporal_pixel(h, 1, rgb, sc,
                                   history + (ptrdiff_t)y * ry + (ptrdiff_t)x * rx, rc,
                                   previous ? previous + (ptrdiff_t)y * py + (ptrdiff_t)x * px
                                            : NULL, pc,
                                   NULL, table, confidence,
                                   mask ? mask + (ptrdiff_t)y * my + (ptrdiff_t)x * mx : NULL,
                                   intensity, scale, hold, slope, release, grade,
                                   neural ? neural + (y * width + x) * 3 : NULL, out);
                } else {
                    float predicted[3];
                    for (size_t c = 0; c < 3; ++c)
                        predicted[c] = unit(rgb[(ptrdiff_t)c * sc] + half(h[c]) * 0.25f);
                    finish_pixel(predicted, rgb, sc, still, grade,
                                 neural ? neural + (y * width + x) * 3 : NULL, out);
                }
                uint8_t *pixel = encoded + ((top + y) * frame_width + left + x) * 4;
                for (size_t c = 0; c < 3; ++c) {
                    float value = out[c];
                    /* NumPy's byte cast maps NaN to zero; do not cast NaN in C. */
                    value = value == value ? unit(value) : 0.0f;
                    pixel[bgra ? 2 - c : c] = (uint8_t)(value * 255.0f + 0.5f);
                }
            }
        }
        free(row);
        free(rows);
        free(graded);
        free(discard);
    }
}
