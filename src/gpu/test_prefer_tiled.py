#!/usr/bin/env python3
"""Plain GEMMs on the tiled kernel against the staged one: the same bytes.

`XMX_PREFER_TILED=1` (libxmx `prefer_tiled`), the experimental switch for Arc A-series cards in
the setup window and the panel, sends plain GEMMs to the tiled kernel: on an A770 the staged
kernel's K loop was the slower one, where on Xe2 it is the faster. A row's sums are its own and
run in the same K order on both, so the switch must move no byte. Checked here on any device,
the routing switched between recordings (`xmx_prefer_tiled_set`):

  * every epilogue of a plain GEMM (plain, published, under the gate, both residuals, the ViT's
    QKV epilogue), specialised and not, at the bottleneck's shapes and the levels', a partial
    block and the 32-row builds' sizes: the same bytes either way, and with the routing on not
    one of them recorded on the staged kernel;
  * the forms each kernel reads fastest, as the runtime keeps the weights on each side: the
    staged kernel on E4M3 bytes by column (where the device takes them) against the tiled one
    on half by row, the same bytes;
  * what only the staged kernel does stays there with the routing on: a window-gathered QKV
    projection and an E4M3 B.
"""
import numpy as np
import xmxres as X

GUARD = 64
FILL16 = np.array([0x7E5A], np.uint16).view(np.float16)[0]
FILL32 = np.array([0x7FC0DEAD], np.uint32).view(np.float32)[0]


def fill(dtype):
    return FILL16 if np.dtype(dtype) == np.float16 else FILL32


def e4m3_halves():
    """The 254 E4M3 values as halves (the two NaNs left out)."""
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


def routed(rt, tiled, record):
    """`record()` on one routing, with how many GEMMs it put on the staged kernel."""
    rt.lib.xmx_prefer_tiled_set(1 if tiled else 0)
    before = rt.lib.xmx_staged_calls()
    rt.begin()
    record()
    rt.submit()
    return (rt.lib.xmx_staged_calls() - before) & 0xFFFFFFFF


def same(name, first, second):
    a, b = first.view(np.uint8), second.view(np.uint8)
    if not np.array_equal(a, b):
        raise AssertionError(f"{name}: {np.flatnonzero(a != b).size} bytes differ between the "
                             "staged and the tiled routing")


def run_dense(rt, rng, halves, rows, cols, inner):
    case, cases = Case(rt), 0
    try:
        weights = halves[rng.integers(0, halves.size, inner * cols)].reshape(inner, cols)
        by_row = case.keep(rt.buffer_from(weights, np.float16))
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
        for name, dtype, kw, residual in variants:
            for mask in (0, 7):
                rt.specialize(mask)
                outs, staged = [], []
                for tiled in (0, 1):
                    if kw is None:
                        q, k, v = (case.alloc(rows * inner, np.float16) for _ in range(3))
                        staged.append(routed(rt, tiled, lambda: rt.gemm_qkv(
                            a, by_row, q, k, v, scale, rows, inner, heads, rows, vit=True)))
                        outs.append(np.concatenate([
                            X.host_view(t, np.float16)[:rows * inner].copy() for t in (q, k, v)]))
                        continue
                    total = rows * cols
                    out = case.alloc(total + GUARD, dtype)
                    X.host_write(out, np.full(total + GUARD, fill(dtype), dtype))
                    if residual is not None:
                        staged.append(routed(rt, tiled, lambda: rt.gemm_residual(
                            a, by_row, residual, cosine, out, rows, cols, inner,
                            skip_half=residual is skip16, **kw)))
                    else:
                        staged.append(routed(rt, tiled, lambda: rt.gemm(
                            a, by_row, out, rows, cols, inner, **kw)))
                    outs.append(X.host_view(out, dtype)[:total].copy())
                same(f"{name} {rows}x{cols}x{inner}, specialise {mask}", outs[0], outs[1])
                if staged[1]:
                    raise AssertionError(f"{name} {rows}x{cols}x{inner}: recorded on the staged "
                                         "kernel with the tiled routing on")
                cases += 1
        # the production pairing: the staged kernel on the forms it reads fastest, the tiled
        # one on half by row
        if rt.e4m3_weights or rt.weights_by_column:
            fast = case.keep(rt.weight(weights, e4m3=rt.e4m3_weights))
            outs = []
            for tiled, b in ((0, fast), (1, by_row)):
                out = case.alloc(rows * cols, np.float32)
                routed(rt, tiled, lambda: rt.gemm(a, b, out, rows, cols, inner))
                outs.append(X.host_view(out, np.float32)[:rows * cols].copy())
            same(f"forms {rows}x{cols}x{inner}", outs[0], outs[1])
            cases += 1
            if getattr(fast, "e4m3", False):
                # an E4M3 B stays on the staged kernel with the routing on
                out = case.alloc(rows * cols, np.float32)
                if not routed(rt, 1, lambda: rt.gemm(a, fast, out, rows, cols, inner)):
                    raise AssertionError("an E4M3 B left the staged kernel")
    finally:
        case.free()
    return cases


def run_window_qkv(rt, rng, height, width, origin, channels):
    """The window-gathered QKV projection stays on the staged kernel, the same bytes."""
    case = Case(rt)
    try:
        heads = channels // 32
        ph, pw, _ = rt.window_extent(height, width, origin, 8)
        rows = ph * pw
        weights = rng.normal(0, 0.5, (channels, 3 * channels)).astype(np.float16)
        b = case.keep(rt.buffer_from(weights, np.float16))
        image = case.alloc(height * width * channels, np.float16)
        X.host_write(image, rng.normal(0, 2.0, height * width * channels).astype(np.float16))
        scale = case.alloc(heads, np.float32)
        X.host_write(scale, rng.uniform(0.5, 4.0, heads).astype(np.float32))
        rt.specialize(7)
        outs = []
        for tiled in (0, 1):
            q, k, v = (case.alloc(rows * channels, np.float16) for _ in range(3))
            staged = routed(rt, tiled, lambda: rt.gemm_qkv(
                image, b, q, k, v, scale, rows, channels, heads, 64,
                window=(height, width, origin), image_half=True))
            if not staged:
                raise AssertionError("the window-gathered QKV left the staged kernel")
            outs.append(np.concatenate([X.host_view(t, np.float16)[:rows * channels].copy()
                                        for t in (q, k, v)]))
        same(f"window QKV {height}x{width} origin {origin}, {channels} channels", *outs)
    finally:
        case.free()
    return 1


def main():
    rt = X.Runtime()
    before = rt.lib.xmx_prefer_tiled()
    halves = e4m3_halves()
    rng = np.random.default_rng(1118)
    cases = 0
    try:
        # the bottleneck's shapes (the 64-row build, two 32-row deep blocks, a partial block,
        # the 32-row build), the levels', and the start-up timing's own
        for rows, cols, inner in ((64, 1024, 4096), (64, 4096, 1024), (64, 3072, 1024),
                                  (40, 1024, 1024), (32, 1024, 4096), (128, 4096, 1024),
                                  (192, 512, 256), (256, 192, 64), (320, 64, 32),
                                  (2048, 128, 512), (8192, 64, 64)):
            cases += run_dense(rt, rng, halves, rows, cols, inner)
        cases += run_window_qkv(rt, rng, 16, 24, (0, 0), 64)
        cases += run_window_qkv(rt, rng, 16, 24, (-4, -4), 128)
    finally:
        rt.lib.xmx_prefer_tiled_set(before)
        rt.specialize(7)
    print(f"plain GEMMs on the tiled kernel: {cases} cases bit-identical to the staged routing, "
          f"none of them recorded staged with it on; the window-gathered QKV and an E4M3 B stay "
          f"staged; this device runs the network's shape "
          f"{'without offering it (Arc A-series)' if rt.shape_unoffered else 'as offered'}, "
          f"plain GEMMs {'tiled' if before else 'staged'} by default here")


if __name__ == "__main__":
    main()
