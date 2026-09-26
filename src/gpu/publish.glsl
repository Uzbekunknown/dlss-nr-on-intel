/* The vendor's rounding points, shared by every shader that has to reproduce them.
 *
 * `precise` is not decoration here: the gate rounds to half between the multiply and
 * the add, and an FMA contraction would skip that rounding and give a different answer
 * from the reference.
 */
#ifndef PUBLISH_GLSL
#define PUBLISH_GLSL

/* The hardware float16 conversion. `float(float16_t(x))` is the obvious spelling and
 * the compiler folds it away, leaving the value in float32 — the bug that once made
 * every vendor rounding point in this graph silently vanish. `packHalf2x16` changes
 * the bit representation so it cannot be elided, and it is bit-exact against numpy's
 * float16 over ordinary values, half subnormals and overflow to infinity
 * (`src/bench/half_probe.py`): two instructions and no branches, where doing the
 * exponent and mantissa by hand took ten and two branches.
 *
 * That is the default and it stays the default: on Mesa the other spelling is folded
 * away, so it is not a substitute there — it is only a fallback for a driver that
 * cannot run this one at all. The B580's Windows driver (101.8993) is such a driver:
 * `packHalf2x16` makes it lose the device (`VK_ERROR_DEVICE_LOST`). A build for it
 * defines HALF_ROUND_FLOAT16, where `float(float16_t(x))` is used instead. On the
 * B580 the two agree bit for bit — 0 differences over 12020 values covering ordinary
 * values, subnormals, NaN and Inf (`src/bench/half_probe.py`) — so this changes which
 * instruction runs, not the number it produces. */
#ifdef HALF_ROUND_FLOAT16
float half_round(float x) { return float(float16_t(x)); }
#else
float half_round(float x) { return unpackHalf2x16(packHalf2x16(vec2(x, 0.0))).x; }
#endif

/* `packHalf2x16` is not only a rounding point: the softmax's exponential in the four
 * attention shaders is a bit trick on the packed word —
 * `(packHalf2x16(affine) << 5) + 0x7ff88000u` and read back — so the shaders below use
 * the hardware instruction there and never `half_round`. The two jobs must not be
 * swapped. Measured on the B580's Windows driver: the round trip through
 * `packHalf2x16` disagrees with float16 on 90109 of 90368 values, while the attention
 * bit trick built on the same instruction produces the correct picture — the trick
 * needs a *deterministic* packing whose bits land where the +0x7ff88000 bias expects,
 * and it gets that; it never reads a float back and compares it with numpy. Replacing
 * the hardware instruction in the bit trick with a hand-spelled pack of matching
 * float16 bits is what broke the picture: the trick's bias was tuned to the hardware
 * spelling, and the hand-spelled bits land elsewhere. `half_round`, which does read a
 * value back, keeps the switch above. */

float e4m3(float x) {
    float magnitude = min(abs(x), 448.0);
    int exponent = (floatBitsToInt(magnitude) >> 23) & 0xFF;
    exponent = max(exponent, 121) - 3;                 // 121-3 = the 2^-9 subnormal step
    float step = intBitsToFloat(exponent << 23);
    float reciprocal = intBitsToFloat((254 - exponent) << 23);
    float rounded = roundEven(magnitude * reciprocal) * step;
    return x < 0.0 ? -rounded : rounded;
}

float gate_activation(float x) {
    precise float wide = half_round(x);
    precise float clamped = clamp(wide, -4.0, 4.0);
    precise float linear = abs(clamped) * -0.055908203125;
    linear += 0.447265625;
    linear = half_round(linear);
    linear *= clamped;
    linear += 0.89453125;
    linear = half_round(linear);
    return half_round(wide * linear);
}

/* The publish an epilogue applies: bits 8-11 of a pass's `flags` pick the transform. */
float publish(uint epilogue, float value) {
    if (epilogue == 2u || epilogue == 3u) value = gate_activation(value);
    if (epilogue == 1u || epilogue == 3u) value = e4m3(value);
    if (epilogue == 4u) value = half_round(value);
    return value;
}

#endif
