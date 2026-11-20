/* The history's reads along the client's motion, shared by image_history.comp (the follower's
 * passes) and image_compose.comp (which does the follower's active-extent pass itself, flag
 * MOVED): nr_image.c `nr_history_sample` with the vendor's five-tap Catmull-Rom, in the
 * reference's float32 operation order. The includer defines FloatBuf and `divide` (the corrected
 * division) first. One text for both, so the two read the same bits. */

float clamp_to(float value, float lower, float upper) {
    value = value < lower ? lower : value;
    return value > upper ? upper : value;
}

/* nr_image.c `catmull`: the two inner weights folded into one offset bilinear tap. */
void catmull(float normalized, uint dimension, out float outer0, out float middle,
             out float outer3, out float w0, out float w3, out float g) {
    precise float pixel = normalized * float(dimension) - 0.5;
    float base_index = floor(pixel);
    precise float t = clamp_to(pixel - base_index, 0.0, 1.0);
    precise float square = t * t;
    precise float cube = square * t;
    precise float w1 = 1.0 - 2.5 * square + 1.5 * cube;
    precise float w2 = 0.5 * t + 2.0 * square - 1.5 * cube;
    precise float first = -0.5 * t + square - 0.5 * cube;
    precise float last = -0.5 * square + 0.5 * cube;
    precise float gain = w1 + w2;
    w0 = first;
    w3 = last;
    g = gain;
    precise float base = base_index + 0.5;
    float lower = 0.5, upper = float(dimension) - 0.5;
    precise float before = base - 1.0;
    precise float inside = base + divide(w2, gain);
    precise float after = base + 2.0;
    outer0 = clamp_to(before, lower, upper);
    middle = clamp_to(inside, lower, upper);
    outer3 = clamp_to(after, lower, upper);
}

/* nr_image.c `sample_linear`, three channels. */
vec3 linear_tap(FloatBuf image, uint height, uint width, float x, float y) {
    precise float px = x - 0.5;
    precise float py = y - 0.5;
    float fx = floor(px), fy = floor(py);
    int x0 = fx < 0.0 ? 0 : fx > float(width - 1u) ? int(width - 1u) : int(fx);
    int y0 = fy < 0.0 ? 0 : fy > float(height - 1u) ? int(height - 1u) : int(fy);
    int x1 = x0 + 1 < int(width) ? x0 + 1 : int(width) - 1;
    int y1 = y0 + 1 < int(height) ? y0 + 1 : int(height) - 1;
    precise float tx = clamp_to(px - float(x0), 0.0, 1.0);
    precise float ty = clamp_to(py - float(y0), 0.0, 1.0);
    vec3 result;
    for (uint c = 0u; c < 3u; ++c) {
        precise float top = image.v[(uint(y0) * width + uint(x0)) * 3u + c] * (1.0 - tx)
                          + image.v[(uint(y0) * width + uint(x1)) * 3u + c] * tx;
        precise float bottom = image.v[(uint(y1) * width + uint(x0)) * 3u + c] * (1.0 - tx)
                             + image.v[(uint(y1) * width + uint(x1)) * 3u + c] * tx;
        precise float value = top * (1.0 - ty) + bottom * ty;
        result[c] = value;
    }
    return result;
}

/* nr_image.c `nr_history_sample` at one normalised coordinate. */
vec3 history_sample(FloatBuf image, uint height, uint width, float u, float v) {
    float xo0, xm, xo3, xw0, xw3, xg, yo0, ym, yo3, yw0, yw3, yg;
    catmull(u, width, xo0, xm, xo3, xw0, xw3, xg);
    catmull(v, height, yo0, ym, yo3, yw0, yw3, yg);
    precise float k0 = xw0 * yg;
    precise float k1 = xg * yw0;
    precise float k2 = xg * yg;
    precise float k3 = xg * yw3;
    precise float k4 = xw3 * yg;
    vec3 t0 = linear_tap(image, height, width, xo0, ym);
    vec3 t1 = linear_tap(image, height, width, xm, yo0);
    vec3 t2 = linear_tap(image, height, width, xm, ym);
    vec3 t3 = linear_tap(image, height, width, xm, yo3);
    vec3 t4 = linear_tap(image, height, width, xo3, ym);
    /* Both sums start from 0, as the reference's do. */
    precise float sum = 0.0;
    sum = sum + k0;
    sum = sum + k1;
    sum = sum + k2;
    sum = sum + k3;
    sum = sum + k4;
    vec3 result;
    for (uint c = 0u; c < 3u; ++c) {
        precise float total = 0.0;
        total = total + k0 * t0[c];
        total = total + k1 * t1[c];
        total = total + k2 * t2[c];
        total = total + k3 * t3[c];
        total = total + k4 * t4[c];
        result[c] = divide(total, sum);
    }
    return result;
}

float apart(vec3 a, vec3 b) {
    precise vec3 step = abs(a - b);
    return max(max(step.r, step.g), step.b);
}

/* Where the pixel (x, y) of an active extent width x height read its surface on the frame
 * before, normalised, along the motion of the frame's pixel (mx + left, my + top), two float32
 * a pixel at frame_width; `known` where that motion is finite. */
bool moved_to(FloatBuf motion, uint frame_width, uint left, uint top, uint mx, uint my,
              uint x, uint y, uint width, uint height, uint active_width, uint active_height,
              out float u_out, out float v_out) {
    uint at = 2u * ((my + top) * frame_width + mx + left);
    float dx = motion.v[at], dy = motion.v[at + 1u];
    bool known = !isnan(dx) && !isinf(dx) && !isnan(dy) && !isinf(dy);
    precise float u = divide(float(x) + 0.5, float(width));
    precise float v = divide(float(y) + 0.5, float(height));
    if (known) {
        /* The offset is in active pixels, so normalised by the active extent. */
        u = u + divide(dx, float(active_width));
        v = v + divide(dy, float(active_height));
    }
    u_out = u;
    v_out = v;
    return known;
}
