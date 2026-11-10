#!/usr/bin/env python3
"""A GEMM's B as E4M3 bytes against the same values as half.

The bottleneck keeps its weights as their E4M3 bytes where the device takes it
(`xmx_e4m3_weights`), and the staged kernel's packed loader decodes each byte to a half
(gemm_staged.comp, 0x4000000). Every E4M3 value is a half, so on the same values the GEMM
must come out byte for byte the same: plain, published, under the gate, with the residual
(a float32 skip, and a half one published) and with the ViT's QKV epilogue, at the
bottleneck's shapes on the 64-row build, the 32-row deep one and a partial block,
specialised and not. B holds every E4M3 value, the subnormals and both zeros among them.

Also: the byte table against the format's definition; a half no E4M3 value equals is not
made a byte; and the paths that cannot read bytes refuse an E4M3 B rather than read it as
halves.
"""
import numpy as np
import xmxres as X

GUARD = 64
FILL16 = np.array([0x7E5A], np.uint16).view(np.float16)[0]
FILL32 = np.array([0x7FC0DEAD], np.uint32).view(np.float32)[0]


def fill(dtype):
    return FILL16 if np.dtype(dtype) == np.float16 else FILL32


def filled(values, dtype):
    bits = np.uint16 if np.dtype(dtype) == np.float16 else np.uint32
    return np.asarray(values).view(bits) == np.asarray(fill(dtype)).view(bits)


def e4m3_values():
    """The 254 E4M3 values (0x7f and 0xff, the NaNs, left out), from the format's own
    definition: bias 7, three mantissa bits, subnormals below exponent 1."""
    values = {}
    for code in range(256):
        exponent, mantissa = (code >> 3) & 15, code & 7
        if exponent == 15 and mantissa == 7:
            continue
        value = (mantissa / 8 * 2.0 ** -6 if exponent == 0
                 else (1 + mantissa / 8) * 2.0 ** (exponent - 7))
        values[code] = np.float16(-value if code & 0x80 else value)
    return values


def check_table():
    values = e4m3_values()
    codes = np.array(sorted(values), np.uint8)
    halves = np.array([values[c] for c in codes], np.float16)
    got = X.e4m3_bytes(halves)
    assert got is not None and np.array_equal(got, codes), "E4M3 byte table differs"
    assert X.e4m3_bytes(np.array([0.1], np.float16)) is None, "0.1 is no E4M3 value"
    assert X.e4m3_bytes(np.array([np.float16(448), np.float16(-448)])) is not None
    assert X.e4m3_bytes(np.array([np.float16(480)])) is None, "480 is past E4M3's largest"
    # the staged order: each four columns' bytes as 0 2 1 3, rows a whole number of fours
    row = halves[:8].reshape(1, 8)
    assert np.array_equal(X.e4m3_codes(row)[0], codes[[0, 2, 1, 3, 4, 6, 5, 7]])
    assert X.e4m3_codes(halves[:6].reshape(1, 6)) is None, "a row of six is no whole fours"
    return codes, halves


def main():
    rt = X.Runtime()
    if not rt.e4m3_weights:
        print("E4M3 B: off on this device (xmx_e4m3_weights; XMX_E4M3_WEIGHTS=1 forces it "
              "where float16 subnormals are kept), nothing to compare")
        return
    codes, halves = check_table()
    rng = np.random.default_rng(4343)
    cases = 0
    # the bottleneck's four GEMMs at 64 tokens (the 64-row build for N > 1024, two 32-row
    # deep blocks for N <= 1024), a partial block, the 32-row build, and two row blocks
    for rows, cols, inner in ((64, 1024, 4096), (64, 4096, 1024), (64, 3072, 1024),
                              (64, 1024, 1024), (40, 1024, 1024), (32, 1024, 4096),
                              (128, 4096, 1024), (192, 512, 256)):
        buffers = []

        def alloc(n, dtype):
            b = rt.buffer(n, dtype)
            buffers.append(b)
            return b
        try:
            # every E4M3 value at least once, then random ones
            picks = np.concatenate([np.arange(halves.size),
                                    rng.integers(0, halves.size, inner * cols - halves.size)])
            weights = halves[rng.permutation(picks)].reshape(inner, cols)
            half_b = rt.buffer_from(weights, np.float16)
            byte_b = rt.buffer_e4m3(weights)
            buffers += [half_b, byte_b]
            assert byte_b is not None and byte_b.e4m3 and byte_b.nbytes == inner * cols
            a = alloc(rows * inner, np.float16)
            skip32 = alloc(rows * cols, np.float32)
            skip16 = alloc(rows * cols, np.float16)
            cosine = alloc(cols, np.float32)
            X.host_write(a, rng.normal(0, 2.0, rows * inner).astype(np.float16))
            X.host_write(cosine, rng.uniform(-1.5, 1.5, cols).astype(np.float32))
            skip = rng.normal(0, 2.0, rows * cols)
            X.host_write(skip32, skip.astype(np.float32))
            X.host_write(skip16, skip.astype(np.float16))
            variants = [("plain", np.float32, {}, None),
                        ("e4m3", np.float16, {"epilogue": X.EPI_E4M3, "narrow": True}, None),
                        ("gate e4m3", np.float16, {"epilogue": X.EPI_GATE_E4M3, "narrow": True}, None),
                        ("residual", np.float32, {}, skip32),
                        ("residual half skip, e4m3", np.float16,
                         {"epilogue": X.EPI_E4M3, "narrow": True}, skip16)]
            if cols == 3 * inner and rows % 16 == 0:
                variants.append(("vit qkv", np.float16, None, None))
            heads = inner // 32
            scale = alloc(heads, np.float32)
            X.host_write(scale, rng.uniform(0.5, 4.0, heads).astype(np.float32))
            for name, dtype, kw, residual in variants:
                for mask in (0, 7):
                    rt.specialize(mask)
                    outs = []
                    for b in (half_b, byte_b):
                        if kw is None:
                            q, k, v = (alloc(rows * inner, np.float16) for _ in range(3))
                            rt.begin()
                            rt.gemm_qkv(a, b, q, k, v, scale, rows, inner, heads, rows, vit=True)
                            rt.submit()
                            got = [X.host_view(t, np.float16)[:rows * inner].copy() for t in (q, k, v)]
                            outs.append((np.concatenate(got),))
                            continue
                        total = rows * cols
                        out = alloc(total + GUARD, dtype)
                        X.host_write(out, np.full(total + GUARD, fill(dtype), dtype))
                        rt.begin()
                        if residual is not None:
                            rt.gemm_residual(a, b, residual, cosine, out, rows, cols, inner,
                                             skip_half=residual is skip16, **kw)
                        else:
                            rt.gemm(a, b, out, rows, cols, inner, **kw)
                        rt.submit()
                        got = X.host_view(out, dtype)
                        assert filled(got[total:], dtype).all(), f"{name}: written past M rows"
                        assert not filled(got[:total], dtype).any(), f"{name}: output left unwritten"
                        outs.append((got[:total].copy(),))
                    first, second = outs[0][0].view(np.uint8), outs[1][0].view(np.uint8)
                    if not np.array_equal(first, second):
                        differ = np.flatnonzero(first != second).size
                        raise AssertionError(f"{name} {rows}x{cols}x{inner}, specialise {mask}: "
                                             f"{differ} bytes differ between a half B and an E4M3 B")
                    cases += 1
            # what cannot read bytes refuses them: a transposed B, a batch
            for name, call in (("transposed", lambda: rt.gemm(a, byte_b, skip32, 64, 32, 64,
                                                              transpose_b=True)),):
                rt.begin()
                try:
                    call()
                except RuntimeError as error:
                    assert "E4M3" in str(error), f"{name}: refused for another reason: {error}"
                    rt.lib.xmx_abort()
                else:
                    rt.lib.xmx_abort()
                    raise AssertionError(f"{name}: an E4M3 B was not refused")
        finally:
            for buf in buffers:
                buf.free()
    rt.specialize(7)
    print(f"GEMM with an E4M3 B: {cases} cases bit-identical to the same values as half, "
          f"all 254 E4M3 values among them; the byte table checked; a transposed B refused")


if __name__ == "__main__":
    main()
