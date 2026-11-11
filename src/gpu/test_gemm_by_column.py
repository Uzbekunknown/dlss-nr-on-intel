#!/usr/bin/env python3
"""A GEMM's B kept by column against the same matrix by row.

Where the device reads its weights faster so (`xmx_weights_by_column`), the graph keeps a
wide weight matrix transposed, cols x inner, and marks the buffer (`Runtime.weight`,
`xmx_buf_mark_by_column`). The staged kernel then reads a column's `inner` as consecutive
bytes, keeps its B tile by column and loads the fragments column-major (gemm_staged.comp,
0x8000000); any other kernel takes the same bytes as a transposed B. The values in every
fragment and the order of every sum are the by-row B's, so the GEMM must come out byte for
byte the same: plain, published, under the gate, with the residual (a float32 skip, and a
half one published), with the ViT's QKV epilogue and the window-gathered one, as a batch of
groups and as their slices, at the bottleneck's shapes and the levels', on the 64-row build,
the 32-row ones and a partial block, on a kernel that is not the staged one, specialised and
not. As half, and as E4M3 bytes where the device takes them.

Any device reads a B kept by column, so this runs wherever the GEMMs do. Also: a B kept by
column is not transposed again, and a pass that is not a GEMM refuses the buffer.
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


def e4m3_halves():
    """The 254 E4M3 values as halves (0x7f and 0xff, the NaNs, left out)."""
    values = []
    for code in range(256):
        exponent, mantissa = (code >> 3) & 15, code & 7
        if exponent == 15 and mantissa == 7:
            continue
        value = (mantissa / 8 * 2.0 ** -6 if exponent == 0
                 else (1 + mantissa / 8) * 2.0 ** (exponent - 7))
        values.append(-value if code & 0x80 else value)
    return np.array(values, np.float16)


class Case:
    """One runtime's buffers for a shape, freed together."""

    def __init__(self, rt):
        self.rt, self.buffers = rt, []

    def alloc(self, n, dtype):
        b = self.rt.buffer(n, dtype)
        self.buffers.append(b)
        return b

    def keep(self, buffer):
        self.buffers.append(buffer)
        return buffer

    def free(self):
        for b in self.buffers:
            b.free()


def same(name, outs):
    first = outs[0].view(np.uint8)
    for label, other in zip(("by column", "by column, E4M3"), outs[1:]):
        other = other.view(np.uint8)
        if not np.array_equal(first, other):
            differ = np.flatnonzero(first != other).size
            raise AssertionError(f"{name}: {differ} bytes differ between a B by row and {label}")


def run_dense(rt, rng, halves, rows, cols, inner):
    """The dense GEMMs: every epilogue, both specialisations, half and E4M3 bytes."""
    case, cases = Case(rt), 0
    try:
        weights = rng.normal(0, 0.5, (inner, cols)).astype(np.float16)
        operands = [case.keep(rt.buffer_from(weights, np.float16)),
                    case.keep(rt.weight(weights, by_column=True))]
        assert operands[1].by_column and operands[1].nbytes == inner * cols * 2
        sets = [("half", operands)]
        if rt.e4m3_weights and inner % 32 == 0 and cols % 32 == 0:
            exact = halves[rng.integers(0, halves.size, inner * cols)].reshape(inner, cols)
            byte_b = case.keep(rt.weight(exact, e4m3=True, by_column=True))
            assert byte_b.e4m3 and byte_b.by_column and byte_b.nbytes == inner * cols
            sets.append(("E4M3", [case.keep(rt.buffer_from(exact, np.float16)), byte_b]))
        a = case.alloc(rows * inner, np.float16)
        skip32 = case.alloc(rows * cols, np.float32)
        skip16 = case.alloc(rows * cols, np.float16)
        cosine = case.alloc(cols, np.float32)
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
        if cols == 3 * inner and rows % 16 == 0 and inner % 32 == 0:
            variants.append(("vit qkv", np.float16, None, None))
        heads = max(inner // 32, 1)
        scale = case.alloc(heads, np.float32)
        X.host_write(scale, rng.uniform(0.5, 4.0, heads).astype(np.float32))
        for kind, pair in sets:
            for name, dtype, kw, residual in variants:
                for mask in (0, 7):
                    rt.specialize(mask)
                    outs = []
                    for b in pair:
                        if kw is None:
                            q, k, v = (case.alloc(rows * inner, np.float16) for _ in range(3))
                            rt.begin()
                            rt.gemm_qkv(a, b, q, k, v, scale, rows, inner, heads, rows, vit=True)
                            rt.submit()
                            outs.append(np.concatenate([
                                X.host_view(t, np.float16)[:rows * inner].copy() for t in (q, k, v)]))
                            continue
                        total = rows * cols
                        out = case.alloc(total + GUARD, dtype)
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
                        outs.append(got[:total].copy())
                    same(f"{kind} {name} {rows}x{cols}x{inner}, specialise {mask}", outs)
                    cases += 1
        # a B kept by column is not transposed again
        rt.begin()
        try:
            rt.gemm(a, operands[1], skip32, rows, cols, inner, transpose_b=True)
        except RuntimeError as error:
            assert "by column" in str(error), f"transposed: refused for another reason: {error}"
            rt.lib.xmx_abort()
        else:
            rt.lib.xmx_abort()
            raise AssertionError("a B kept by column was transposed again")
        # and is no other pass's operand
        rt.begin()
        try:
            rt.to_half(operands[1], skip16, 64)
        except RuntimeError:
            rt.lib.xmx_abort()
        else:
            rt.lib.xmx_abort()
            raise AssertionError("a pass that is not a GEMM took a B kept by column")
    finally:
        case.free()
    return cases


def run_groups(rt, rng, rows, cols, inner, groups):
    """The grouped feed-forward's GEMMs (nr_resident._ffn_groups): a batch of groups, and
    the same groups as slices, each with its own weights, into column slices of one buffer."""
    case, cases = Case(rt), 0
    try:
        weights = rng.normal(0, 0.5, (groups, inner, cols)).astype(np.float16)
        pair = [case.keep(rt.buffer_from(weights, np.float16)),
                case.keep(rt.weight(weights, by_column=True))]
        a = case.alloc(rows * groups * inner, np.float16)
        X.host_write(a, rng.normal(0, 2.0, rows * groups * inner).astype(np.float16))
        strides = (inner, inner * cols, cols)
        leading = (groups * inner, 0, groups * cols)
        for batched in (True, False):
            for mask in (0, 7):
                rt.specialize(mask)
                outs = []
                for b in pair:
                    total = rows * groups * cols
                    out = case.alloc(total + GUARD, np.float16)
                    X.host_write(out, np.full(total + GUARD, FILL16, np.float16))
                    rt.begin()
                    if batched:
                        rt.gemm(a, b, out, rows, cols, inner, batch=groups, strides=strides,
                                leading=leading, epilogue=X.EPI_GATE_E4M3, narrow=True)
                    else:
                        with rt.independent():
                            for group in range(groups):
                                rt.gemm(a, b, out, rows, cols, inner, leading=leading,
                                        offsets=tuple(group * step for step in strides),
                                        epilogue=X.EPI_GATE_E4M3, narrow=True)
                    rt.submit()
                    got = X.host_view(out, np.float16)
                    assert filled(got[total:], np.float16).all(), "groups: written past the end"
                    assert not filled(got[:total], np.float16).any(), "groups: output left unwritten"
                    outs.append(got[:total].copy())
                same(f"{groups} groups {rows}x{cols}x{inner}, "
                     f"{'a batch' if batched else 'slices'}, specialise {mask}", outs)
                cases += 1
    finally:
        case.free()
    return cases


def run_window_qkv(rt, rng, height, width, origin, channels):
    """The window-gathered QKV projection: A read out of the image, B by column beside it."""
    case, cases = Case(rt), 0
    try:
        heads = channels // 32
        ph, pw, _ = rt.window_extent(height, width, origin, 8)
        rows = ph * pw
        weights = rng.normal(0, 0.5, (channels, 3 * channels)).astype(np.float16)
        pair = [case.keep(rt.buffer_from(weights, np.float16)),
                case.keep(rt.weight(weights, by_column=True))]
        image = case.alloc(height * width * channels, np.float16)
        X.host_write(image, rng.normal(0, 2.0, height * width * channels).astype(np.float16))
        scale = case.alloc(heads, np.float32)
        X.host_write(scale, rng.uniform(0.5, 4.0, heads).astype(np.float32))
        for mask in (0, 7):
            rt.specialize(mask)
            outs = []
            for b in pair:
                q, k, v = (case.alloc(rows * channels, np.float16) for _ in range(3))
                rt.begin()
                rt.gemm_qkv(image, b, q, k, v, scale, rows, channels, heads, 64,
                            window=(height, width, origin), image_half=True)
                rt.submit()
                outs.append(np.concatenate([
                    X.host_view(t, np.float16)[:rows * channels].copy() for t in (q, k, v)]))
            same(f"window QKV {height}x{width} origin {origin}, {channels} channels, "
                 f"specialise {mask}", outs)
            cases += 1
    finally:
        case.free()
    return cases


def main():
    rt = X.Runtime()
    halves = e4m3_halves()
    rng = np.random.default_rng(1112)
    cases = 0
    # the bottleneck's four GEMMs at 64 tokens (the 64-row build for N > 1024, two 32-row deep
    # blocks for N <= 1024), a partial block, the 32-row build, more row blocks; then the
    # levels' shapes, a K that is not whole 64-deep steps, and K = 16, which the staged
    # kernel does not take: the same bytes as a transposed B to the kernel that does
    for rows, cols, inner in ((64, 1024, 4096), (64, 4096, 1024), (64, 3072, 1024),
                              (64, 1024, 1024), (40, 1024, 1024), (32, 1024, 4096),
                              (128, 4096, 1024), (192, 512, 256), (256, 192, 64),
                              (320, 64, 32), (64, 64, 96), (32, 64, 96), (64, 64, 16)):
        cases += run_dense(rt, rng, halves, rows, cols, inner)
    # the split family's group GEMMs and the branched blocks' expand
    cases += run_groups(rt, rng, 128, 256, 64, 8)
    cases += run_groups(rt, rng, 128, 64, 256, 8)
    cases += run_groups(rt, rng, 192, 128, 64, 2)
    # a window block's projection, unshifted and shifted
    cases += run_window_qkv(rt, rng, 16, 24, (0, 0), 64)
    cases += run_window_qkv(rt, rng, 16, 24, (-4, -4), 128)
    rt.specialize(7)
    print(f"GEMM with B kept by column: {cases} cases bit-identical to the same B by row"
          f"{', E4M3 bytes among them' if rt.e4m3_weights else ' (E4M3 bytes off on this device)'}; "
          f"a second transposition and a pass that is no GEMM refused; "
          f"this device keeps its wide weights by "
          f"{'column' if rt.weights_by_column else 'row'}")


if __name__ == "__main__":
    main()
