#!/usr/bin/env python3
"""
nr_frame — one RGB frame in, one RGB frame out, through the recovered graph.

The network itself is `nr_model`.  The surrounding contract (the 16-channel
feature assembly with its deterministic noise, and the head-to-RGB composition)
lives in MLX-DLSS's pure-numpy `features.py` / `composition.py`, which are loaded
by path from the clone in `work/` — they need no torch.  Apache-2.0, see
work/mlx-dlss/LICENSE.

    python3 src/ref/nr_frame.py IN.png OUT.png [--scale 1] [--profile standard]
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import pathlib
import sys
import time
import types

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
# NR_ROOT lets a deployed tree (or a launcher) point at the directory the daemon's
# work/ lives under; the in-tree default is two levels up from this file.
ROOT = pathlib.Path(os.environ.get("NR_ROOT") or HERE.parent.parent)
sys.path.insert(0, str(HERE))

import nr_model  # noqa: E402
import nr_topology  # noqa: E402
import image_io  # noqa: E402

# nvngx_dlssnr.dll 310.9.16 carries two networks (`nr_topology`): preset A, the model of
# 310.8 and of every release so far, and preset B, the 31-block one NVIDIA ships as its
# default. NR_PRESET picks; B's weights come out of the DLL with src/tools/unpack_preset.py.
PRESET_WEIGHTS = {"A": "dlssnr-logical.safetensors", "B": "dlssnr-b-logical.safetensors"}
PRESET = (os.environ.get("NR_PRESET") or "A").upper()
if PRESET not in PRESET_WEIGHTS:
    raise ValueError(f"NR_PRESET={PRESET}: the presets are {', '.join(PRESET_WEIGHTS)}")
WEIGHTS = ROOT / "work" / "mlxw" / PRESET_WEIGHTS[PRESET]
_MLX = ROOT / "work" / "mlx-dlss" / "python" / "mlxdlss"


# The pure-numpy halves of MLX-DLSS. `temporal` and `motion_quality` import cv2 and
# PIL lazily — only for optical flow and for a processing scale other than 1 — so at
# scale 1 with engine motion they need neither.
MLX_NUMPY_MODULES = ("features", "composition", "motion_quality", "temporal")


def load_mlx_numpy_modules(names=MLX_NUMPY_MODULES):
    """Import MLX-DLSS's numpy modules without its torch-laden `__init__`."""
    if not _MLX.is_dir():
        raise SystemExit(
            f"missing {_MLX}\n"
            "  git clone --depth 1 https://github.com/iamwavecut/MLX-DLSS work/mlx-dlss")
    if "mlxnp" not in sys.modules:
        package = types.ModuleType("mlxnp")
        package.__path__ = [str(_MLX)]
        sys.modules["mlxnp"] = package
    loaded = []
    for name in names:
        key = f"mlxnp.{name}"
        module = sys.modules.get(key)
        if module is None:
            spec = importlib.util.spec_from_file_location(key, _MLX / f"{name}.py")
            module = importlib.util.module_from_spec(spec)
            sys.modules[key] = module
            spec.loader.exec_module(module)
        loaded.append(module)
    return loaded


features_mod, composition_mod = load_mlx_numpy_modules(("features", "composition"))
NetworkGeometry = features_mod.NetworkGeometry

# The network's frame is padded, by mirroring, to at least this on a side. 320 is what
# NVIDIA's own driver does; the graph itself runs down to 128 — a window of 8 at a sixteenth
# of the extent, MLX-DLSS's graph contract. At a small live size most of a 320x320 frame is
# mirror padding: 44 % of it for a 320x180 frame, 82 % for 179x101.
VENDOR_MINIMUM_EXTENT = 320
GRAPH_MINIMUM_EXTENT = 128


def _field_alignment(extent):
    """Two to the number of times the graph reduces an axis of `extent` pixels: six halvings,
    each rounded up to 4, that shrink it, and a seventh when level 0 is not whole 8-pixel
    windows. The field is aligned to that, so every halving is exact."""
    reductions, size = 0, extent
    for level in range(6):
        half = -(-((size + 1) // 2) // 4) * 4
        reductions += half < size
        reductions += level == 0 and half % 8 != 0
        size = half
    return 1 << reductions


def network_geometry(width, height, minimum=VENDOR_MINIMUM_EXTENT):
    """The network extent for a `width` x `height` frame, as the vendor pads it (OpenDLSS-NR's
    `geometryFromValid`, bit-exact against captures of the original at eleven sizes): each
    side aligned to the graph's own reductions (`_field_alignment`) and at least `minimum`
    (never below the graph's 128), and the width one alignment more when both sides are
    four alignments.

    That last step is not cosmetic. Such a field — 1280x768, 1024x768, 512x512 — pools down
    to a bottleneck with no padding token in it, and there the pass comes out 25-30 % weaker
    than on any field around it (notes/opendlss-reference.md). MLX-DLSS's rule, a multiple
    of 64, ran a 1280x720 frame on exactly such a field.

    Below 129 pixels a side the vendor's field need not be a multiple of 64, which this graph
    — exact halvings down to level 3 — cannot follow; there it is rounded up to 64.
    """
    floor = max(GRAPH_MINIMUM_EXTENT, int(minimum))
    align_width, align_height = _field_alignment(width), _field_alignment(height)
    field_width = max(floor, -(-width // align_width) * align_width)
    field_height = max(floor, -(-height // align_height) * align_height)
    if field_width % (4 * align_width) == 0 and field_height % (4 * align_height) == 0:
        field_width += align_width
    return NetworkGeometry(width, height, -(-field_width // 64) * 64, -(-field_height // 64) * 64)


# MLX-DLSS's own pipeline and temporal session ask `vendor_aligned` for their field, and their
# answer — a multiple of 64 — is theirs, not the vendor's; in this tree it is the vendor's.
NetworkGeometry.vendor_aligned = classmethod(lambda cls, width, height: network_geometry(width, height))


def lowest_effective_scale(width, height, minimum=VENDOR_MINIMUM_EXTENT, step=0.05):
    """The lowest scale, on `step`, whose network field is smaller than the field one step below it.

    Everything at or under this scale runs the SAME field as it does - the field has stopped
    shrinking (`network_geometry` holds it at `minimum` a side) - so a knob that offered them would
    be selling speed it cannot deliver. Measured at 720p: 0.25, 0.2, 0.15, 0.1 and 0.05 all ran
    320x320, and the floor is 0.30.

    The search walks the knob's own steps, because the field is aligned and the price is not
    monotonic in the picture's size: `render_extent` picks the cheapest field at or above each
    scale, and a step that starts smaller can land on a cheaper, larger field. `None` when the
    picture's size is unknown, so a caller with no frame yet leaves its knob alone.
    """
    if not width or not height or step <= 0:
        return None
    previous = None
    scale = step
    while scale <= 1.0 + 1e-9:
        extent = render_extent(width, height, min(scale, 1.0), minimum)
        geometry = network_geometry(*extent, minimum=minimum)
        area = geometry.network_width * geometry.network_height
        if previous is not None and area > previous:
            return round(scale, 4)
        previous = area
        scale = round(scale + step, 4)
    return 1.0


_EXTENTS: dict = {}


def render_extent(width, height, scale, minimum=VENDOR_MINIMUM_EXTENT):
    """The frame the network is handed for a `width` x `height` picture at render `scale`, as
    (width, height): of all the frames at least the scale's own, aspect kept, the one on the
    cheapest network field — and of those the largest.

    The graph runs on the field, not on the frame: a frame smaller than the field is mirrored
    out to it, and every frame on the same field costs the same. On a small window the field
    is held at `minimum` a side, and there a low scale bought no speed and handed the network
    mostly its own mirror image — 640x360 at 0.35 is 224x126 in a 320x320 field, 28 % of it
    the picture, and the pass came out a quarter weaker than at 0.5, on the same field in the
    same time. And the vendor's field is not monotonic in the frame: 800x450 at 0.35 is
    280x158, whose width aligns to 384, where 0.4's 320x180 lands on 320x320 — the lower scale
    was the slower. Now 640x360 runs as 0.5 for any scale up to it and 800x450 at 0.35 as 0.4,
    a lower scale is never the slower, and where a scale already fills its field — 1280x720
    and up — nothing moves.
    """
    key = (int(width), int(height), float(scale), int(minimum))
    if key in _EXTENTS:
        return _EXTENTS[key]
    if scale >= 1.0:
        best = (int(width), int(height))
    else:
        def cost(w, h):
            geometry = network_geometry(w, h, minimum=minimum)
            return geometry.network_width * geometry.network_height
        best = (max(64, round(width * scale)), max(64, round(height * scale)))
        cheapest = cost(*best)
        for w in range(best[0] + 1, int(width) + 1):
            candidate = (w, max(64, round(height * w / width)))
            price = cost(*candidate)
            if price <= cheapest:
                best, cheapest = candidate, price
    _EXTENTS[key] = best
    return best
# The three noise channels depend on the extent and the frame index and on nothing else,
# and both callers copy the result into a slice rather than writing through it. In a live
# mode the frame index does not move — the daemon never sets one — so the same array was
# being rebuilt from four transcendentals per pixel on every frame, at 29-42 % of the
# whole feature assembly (notes/phase48). Memoised here rather than in `work/mlx-dlss`,
# which is a vendored dependency; the result is marked read-only so a future in-place
# user fails loudly instead of corrupting every later frame.
_raw_noise = features_mod.deterministic_noise
_noise_cache: dict = {}


def _cached_noise(height, width, frame_index=0):
    key = (int(height), int(width), int(frame_index))
    value = _noise_cache.get(key)
    if value is None:
        if len(_noise_cache) >= 8:          # extents change rarely; a swapchain resize
            _noise_cache.clear()            # or a scale change should not grow this
        value = _raw_noise(height, width, frame_index)
        value.flags.writeable = False
        _noise_cache[key] = value
    return value


features_mod.deterministic_noise = _cached_noise
deterministic_noise = _cached_noise
compose_head = composition_mod.compose_head
compose_detail = composition_mod.compose_detail
AutomaticMask = features_mod.AutomaticMask
half = features_mod.half
scaled_color = features_mod.scaled_color

# MLX-DLSS's profiles, and Hogwarts Legacy's own tuning: what the game sets through NVIDIA's
# Unreal plug-in since its DLSS 5 update of 2026-11-05, read out of its executable. The game's
# defaults are Tone 50, Detail 84 and Character 27 percent for every upscaler
# (`GetDefaultNeuralRenderingInfoForUpscaler`), and it hands each to the plug-in times 0.01, as
# `r.Streamline.DLSSNR.ToneStrength`, `.StructuralStrength` and `.Mask.SkinStructureStrength`,
# which Streamline passes to NGX unchanged as `DLSSNR.LocalToneStrength`,
# `.LocalStructureStrength` and `.SkinStructureStrength`. Intensity and style it leaves at the
# plug-in's 1.0 and 0. The skin strength acts only with the automatic mask on; the game's packed
# config, not its code, sets the mask's mode, and a Character slider would do nothing without
# it, so the mask is on here. A profile's `automatic_mask` goes to the feature builders with the
# three scalars. `notes/phase73-game-tuning.md`.
PROFILES = {**features_mod.PROFILES,
            "hogwarts": {"normalized_style": 0.0, "local_tone_strength": 0.5,
                         "local_structure_strength": 0.84,
                         "automatic_mask": AutomaticMask(0.27, -1.0)}}


def feature_lanes(normalized_style=0.0, local_tone_strength=1.0, local_structure_strength=1.0,
                  automatic_mask=None):
    """Feature channels 10-14 of a frame with no control mask, as MLX-DLSS's `make_features`
    writes them, each rounded to half: the style, the tone, the structure and the two automatic-
    mask lanes. With the automatic mask on (either of its strengths >= 0) the structure lane is
    1 and the two lanes carry the skin and the automatic-mask strengths, a negative one taking
    the local structure; with it off they are -1 (`notes/phase72`)."""
    style, tone = float(half(normalized_style)), float(half(local_tone_strength))
    if automatic_mask is not None and max(automatic_mask.skin_structure_strength,
                                          automatic_mask.automatic_mask_structure_strength) >= 0:
        def lane(strength):
            return float(half(strength if strength >= 0 else local_structure_strength))
        return (style, tone, 1.0, lane(automatic_mask.skin_structure_strength),
                lane(automatic_mask.automatic_mask_structure_strength))
    return (style, tone, float(half(local_structure_strength)), -1.0, -1.0)

# `DLSSNR.MaskBlendMode`, new in 310.9.16: how a control mask's structure meets the automatic
# mask's skin and automatic-mask strengths. 0 is 310.8's behaviour, a control mask switching
# the automatic mask off.
MASK_BLEND_MODES = {0: "off", 1: "average", 2: "minimum", 3: "maximum"}


def make_features(color, *, control_mask=None, automatic_mask=None, mask_blend_mode=0,
                  local_structure_strength=1.0, **values):
    """MLX-DLSS's `make_features`, with the vendor's lanes for a control mask.

    Without a control mask it is MLX-DLSS's, unchanged. With one, the vendor's pre-block
    (310.8 and 310.9.16 alike, `notes/phase72`) puts -1, "no automatic mask", in channels
    13 and 14, where MLX-DLSS puts 0. 310.9.16's mask blend modes 1-3 keep the automatic
    mask on beside the control mask: structure is then 1, and the skin and automatic-mask
    strengths are each averaged with (1), lowered to (2) or raised to (3) the masked
    structure, the mask's blue times the local structure, in float32 as the kernel does.
    """
    if mask_blend_mode not in MASK_BLEND_MODES:
        raise ValueError(f"mask blend mode {mask_blend_mode}: the modes are 0-3")
    features = features_mod.make_features(
        color, control_mask=control_mask, automatic_mask=automatic_mask,
        local_structure_strength=local_structure_strength, **values)
    if control_mask is None:
        return features
    enabled = (automatic_mask is not None and max(automatic_mask.skin_structure_strength,
                                                  automatic_mask.automatic_mask_structure_strength) >= 0)
    if not enabled or mask_blend_mode == 0:
        features[..., 13:15] = -1
        return features
    geometry = values.get("geometry") or NetworkGeometry.identity(color.shape[1], color.shape[0])
    rows, columns = geometry.source_rows(), geometry.source_columns()
    structure = np.float32(local_structure_strength)
    masked = np.asarray(control_mask, np.float32)[rows[:, None], columns[None, :], 2] * structure
    for channel, strength in ((13, automatic_mask.skin_structure_strength),
                              (14, automatic_mask.automatic_mask_structure_strength)):
        own = np.full(masked.shape, structure if strength < 0 else strength, np.float32)
        if mask_blend_mode == 1:
            own = (masked + own) * np.float32(0.5)
        elif mask_blend_mode == 2:
            own = np.where(masked < own, masked, own)
        else:
            own = np.where(masked > own, masked, own)
        features[..., channel] = half(own)
    features[..., 12] = 1
    return features

try:
    import nr_image                      # the same passes in C, when they are built
except ImportError:                      # pragma: no cover - the fallback is the point
    nr_image = None

# half(blend_scale) of the recovered package, `notes/phase12-temporal.md`: preset A's.
BLEND_SCALE = 0.73974609375


def blend_scale_of(weights):
    """The history gate's ceiling of the model these logical weights are: its head block's
    `blend_scale`, BLEND_SCALE for preset A and 0.76708984375 for preset B."""
    head = nr_topology.Topology.from_weights(weights).head
    return float(np.asarray(weights[f"block{head}.layer0.blend_scale"]).reshape(-1)[0])


def build_features(colour, *, geometry, history=None, frame_index=0,
                   normalized_style=0.0, local_tone_strength=1.0,
                   local_structure_strength=1.0, automatic_mask=None, out=None):
    """`make_features` with the history folded in, natively where that is available.

    The two steps are one pass in C: the mirror onto the network extent, the three FP16
    roundings of `scaled_color` and the history that replaces channels 7-9 all happen
    while each pixel is in a register. In NumPy they are a full-frame gather each, and
    feature assembly was the largest single host pass in the frame (`notes/phase57`).

    The native pass takes the five conditioning lanes as they are, so the automatic mask, a
    profile's or the caller's, goes native too (`feature_lanes`); a control mask has no place
    here. Either way the bits are the same.

    `out`, when given, is where the features are written — the graph's own mapped input
    (`ResidentBackend.input_view`), float32 or half — and is returned.
    """
    values = dict(normalized_style=normalized_style,
                  local_tone_strength=local_tone_strength,
                  local_structure_strength=local_structure_strength,
                  automatic_mask=automatic_mask)
    native = None
    if nr_image is not None:
        controls = np.array(feature_lanes(**values), np.float32)
        native = nr_image.features(
            colour, geometry.source_rows(), geometry.source_columns(),
            deterministic_noise(geometry.network_height, geometry.network_width,
                                frame_index),
            controls, history=history, out=out)
    if native is not None:
        return native
    features = make_features(colour, geometry=geometry, frame_index=frame_index, **values)
    if history is not None:
        apply_history(features, history, geometry)
    if out is not None:
        # NumPy's half rounding is the GPU's to_half, to the bit (test_input_fp16.py)
        np.copyto(out, features, casting="unsafe")
        return out
    return features


def apply_history(features, history, geometry=None):
    """Put a previous output into feature channels 7-9, reprojected by the identity.

    MLX-DLSS's `make_temporal_features` samples the history along engine motion
    vectors. A Vulkan layer at `vkQueuePresentKHR` has no motion vectors — it sees a
    finished frame and nothing that produced it — so the reprojection is the identity,
    which `sample_history` computes bit-exactly at pixel centres (checked: max |delta|
    is 0 over a random image, so the five-tap collapses to its middle tap).

    That is the correct history wherever the scene stood still, and the *wrong* history
    wherever it moved. Rejecting the wrong one is the learned gate's job, and it does
    discriminate: 0.705 with correct history against 0.032 with wrong motion
    (`notes/phase12-temporal.md`). The first-frame layout already writes the current
    colour into these channels, so this is a drop-in replacement at the same scaling.
    """
    history = np.asarray(history, dtype=np.float32)
    if history.ndim != 3 or history.shape[2] != 3:
        raise ValueError("history must be (height, width, 3)")
    if geometry is not None and not geometry.is_identity:
        history = history[geometry.source_rows()[:, None],
                          geometry.source_columns()[None, :], :]
    if history.shape[:2] != features.shape[:2]:
        raise ValueError("history must match the feature extent")
    features[..., 7:10] = scaled_color(history)
    return features


def history_weight(head, *, blend_scale=BLEND_SCALE):
    """The per-pixel history weight the model asked for, from head channel 4.

    The logit is rounded to half before anything else, so the gate has 65536 possible
    inputs. NumPy's own expression — `gate_formula`, below — is evaluated once on every one
    of them, and each frame indexes that table: the same expression on the same values, so
    bit-identical, where it used to run an exp, a reciprocal and a clip on every pixel of
    the output. 3.1 ms of a 1280x720 frame. `test_nr_model.py` checks the table against the
    formula on all 65536 inputs.
    """
    bits = np.asarray(head, dtype=np.float32)[..., 3:4].astype(np.float16).view(np.uint16)
    return gate_table(float(blend_scale))[bits]


def gate_formula(logit, blend_scale=BLEND_SCALE):
    """The gate as the model defines it, on a logit already rounded to half."""
    return np.clip(1.0 / (1.0 + np.exp(-logit)) * half(blend_scale), 0, 1)


HOLD_RAMP = np.float32(4.0)


def release_slope(levels):
    """The folded constant of the release: the gate's share falls from all of it at no
    change to none by `levels` of 255. 0 turns it off."""
    return float(np.float32(-255.0 / levels)) if levels > 0 else 0.0


def release_factor(current, previous, slope):
    """How much of the model's gate a pixel keeps, from what the game did to it: the
    largest step of its three channels, `clip(1 + moved * slope, 0, 1)` in the order the
    native composition computes it."""
    moved = np.abs(np.subtract(current[..., 0], previous[..., 0], dtype=np.float32))
    scratch = np.empty_like(moved)
    for channel in (1, 2):
        np.subtract(current[..., channel], previous[..., channel], out=scratch)
        np.maximum(moved, np.abs(scratch, out=scratch), out=moved)
    np.multiply(moved, np.float32(slope), out=moved)
    np.add(moved, np.float32(1.0), out=moved)
    return np.clip(moved, 0, 1, out=moved)[..., None]


def hold_floor(current, previous, strength):
    """Per-pixel lower bound on the history weight, from what the *game* did: full where
    the game handed back the same pixel, gone by `HOLD_RAMP` levels of 255 (notes/phase54).

    Channel by channel and in place — the obvious `max(abs(a - b), axis=2)` builds two
    temporaries and runs seven full-frame passes at the output resolution — and folded
    into one multiply-add-clip, which is the order the native composition transcribes.
    """
    floor = np.abs(np.subtract(current[..., 0], previous[..., 0], dtype=np.float32))
    scratch = np.empty_like(floor)
    for channel in (1, 2):
        np.subtract(current[..., channel], previous[..., channel], out=scratch)
        np.maximum(floor, np.abs(scratch, out=scratch), out=floor)
    # clip(1 - moved * 255 / ramp, 0, 1) * strength, folded into one multiply-add-clip
    np.multiply(floor, np.float32(-255.0 * strength / HOLD_RAMP), out=floor)
    np.add(floor, np.float32(strength), out=floor)
    return np.clip(floor, 0, strength, out=floor)[..., None]


_GATE_TABLES = {}


def gate_table(blend_scale):
    """`gate_formula` on every half value, indexed by the value's sixteen bits."""
    table = _GATE_TABLES.get(blend_scale)
    if table is None:
        every = np.arange(1 << 16, dtype=np.uint32).astype(np.uint16).view(np.float16)
        with np.errstate(over="ignore", invalid="ignore"):
            table = gate_formula(every.astype(np.float32), blend_scale).astype(np.float32)
        _GATE_TABLES[blend_scale] = table
    return table


# What the vendor's own panel starts at, which is not what MLX-DLSS's profiles use:
# structure 1.5 rather than 1.0, skin structure 2.0, and the automatic mask on.
# Recovered from the shipped control surface, not from the DLL —
# `notes/phase30-control-atlas.md`.
VENDOR_DEFAULTS = {"normalized_style": 0.0, "local_tone_strength": 1.0,
                   "local_structure_strength": 1.5}
VENDOR_AUTOMATIC_MASK = (2.0, -1.0)     # skin structure, automatic-mask structure


def controls(profile="standard", style_index=None, local_tone=None,
             local_structure=None):
    """The three conditioning scalars, a profile plus any explicit overrides.

    `style_index` is the vendor's integer style, normalised by 1/128; the profiles
    are style 0 / 1 / 2 with tone and structure at 1, and `neutral` is style 0 with
    both at 0. `vendor` is what the shipped panel starts at.
    """
    values = dict(VENDOR_DEFAULTS if profile == "vendor" else PROFILES[profile])
    if style_index is not None:
        values["normalized_style"] = style_index / 128.0
    if local_tone is not None:
        values["local_tone_strength"] = local_tone
    if local_structure is not None:
        values["local_structure_strength"] = local_structure
    return values


# The colour grade the vendor applies after the network for each style: an exposure in
# EV, a smoothstep contrast and a saturation offset, each the preset times the local tone
# strength clamped to [0, 1]. Read out of the DLL itself — the per-style table at
# 0x1800b0de4 (style 1's mask 0x34 is exposure, contrast and saturation, style 2's 0x20
# saturation alone), its lerp from neutral at 0x18001d5f0, and the operator in
# `cg2r_post_process_kernel` (`notes/phase70-post-process.md`). Style 0 has no entry and
# no grade. The hosts that name the styles call 1 `natural` and 2 `cinematic`.
STYLE_GRADES = {1: (-0.1, -0.25, -0.1), 2: (0.0, 0.0, -0.15)}


def grade_for(normalized_style=0.0, local_tone_strength=1.0, **_):
    """The grade a set of conditioning scalars asks for, as the three float32 factors
    `style_grade` takes — the exposure as a multiplier, the contrast, the saturation as a
    multiplier — or None where there is none. The vendor scales the preset in float32 and
    skips its pass when every value is neutral, which is a tone of 0."""
    preset = STYLE_GRADES.get(int(round(float(normalized_style) * 128)))
    tone = np.float32(min(max(float(local_tone_strength), 0.0), 1.0))
    if preset is None or tone == 0:
        return None
    exposure, contrast, saturation = (np.float32(value) * tone for value in preset)
    return (np.float32(2.0 ** float(exposure)), np.float32(contrast),
            saturation + np.float32(1.0))


_THIRD, _SIXTH, _TWO_THIRDS = np.float32(1 / 3), np.float32(1 / 6), np.float32(2 / 3)


def _hue_channel(p, q, t):
    t = np.where(t < 0, t + np.float32(1), t)
    t = np.where(t > 1, t - np.float32(1), t)
    rising = p + t * ((q - p) * np.float32(6))
    falling = p + (_TWO_THIRDS - t) * (q - p) * np.float32(6)
    return np.where(t < _SIXTH, rising,
                    np.where(t < np.float32(0.5), q, np.where(t < _TWO_THIRDS, falling, p)))


def style_grade(rgb, grade):
    """`cg2r_post_process_kernel`'s grade on RGB in [0, 1], for the parameters a style
    sets. Per channel an exposure and a smoothstep contrast; then in HSL a saturation
    multiplier. The kernel's other stages — levels, temperature and tint, a five-zone tone
    curve, a gamma and a saturation power — are identities at the values no style changes
    and are left out; the vendor evaluates two of them as `ex2(lg2(x))`, which the
    hardware's approximations make a 1e-7 wobble rather than an identity. The operations
    and their order are `nr_image.c`'s `style_grade`, so the two are the same bytes."""
    exposure, contrast, saturation = grade
    c = np.clip(np.asarray(rgb, dtype=np.float32), 0, 1)
    c = np.clip(c * exposure, 0, 1)
    c = np.clip(c + contrast * (c * c * (np.float32(3) - (c + c)) - c), 0, 1)
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    high = np.maximum(np.maximum(r, g), b)
    low = np.minimum(np.minimum(r, g), b)
    light = (high + low) * np.float32(0.5)
    delta = high - low
    grey = ~(high > low)
    with np.errstate(divide="ignore", invalid="ignore"):
        chroma = np.where(light > np.float32(0.5), delta / ((np.float32(2) - high) - low),
                          delta / (high + low))
        hue = np.where(high == r,
                       ((g - b) / delta + np.where(g < b, np.float32(6), np.float32(0)))
                       / np.float32(6),
                       np.where(high == g, ((b - r) / delta + np.float32(2)) / np.float32(6),
                                ((r - g) / delta + np.float32(4)) / np.float32(6)))
    chroma = np.clip(np.where(grey, np.float32(0), chroma) * saturation, 0, 1)
    hue = np.where(grey, np.float32(0), hue)
    q = np.where(light < np.float32(0.5), light * (chroma + np.float32(1)),
                 (light + chroma) - light * chroma)
    p = (light + light) - q
    out = np.stack((_hue_channel(p, q, hue + _THIRD), _hue_channel(p, q, hue),
                    _hue_channel(p, q, hue - _THIRD)), axis=-1)
    out = np.where((chroma <= 0)[..., None], light[..., None], out)
    return np.clip(out, 0, 1).astype(np.float32)


def truncate_half(value):
    """float32 toward zero onto the half grid: what the vendor's RGBA16F history keeps of
    the prediction, as OpenDLSS-NR captured it — truncated, not rounded."""
    value = np.asarray(value, dtype=np.float32)
    bits = value.view(np.uint32)
    kept = (bits & np.uint32(0xFFFFE000)).view(np.float32)
    small = np.floor(np.abs(value) * np.float32(2 ** 24)) / np.float32(2 ** 24)
    return np.where((bits & np.uint32(0x7FFFFFFF)) >= np.uint32(0x38800000), kept,
                    np.copysign(small, value)).astype(np.float32)


class ResidentBackend:
    """Runs the graph on the GPU, keeping activations in device buffers.

    Retains the most recently used extents, one by default. Evicted frames are
    closed, releasing their GPU allocations and captured commands. A caller must
    not keep using a frame after asking this backend for a different extent.
    """

    def __init__(self, weights_path=None, *, max_cached_frames=1):
        if not isinstance(max_cached_frames, int) or max_cached_frames < 1:
            raise ValueError("max_cached_frames must be a positive integer")
        sys.path.insert(0, str(ROOT / "src" / "gpu"))
        import xmxres
        import nr_frame_resident
        self._module = nr_frame_resident
        self.runtime = xmxres.Runtime()
        self.weights, _ = nr_model.load_logical(weights_path or WEIGHTS)
        self.topology = nr_topology.Topology.from_weights(self.weights)
        self.blend_scale = blend_scale_of(self.weights)
        # one upload for as long as the backend is open: the extent changes under a knob
        # and the weights do not depend on it. Built on the first frame, not here, so that
        # constructing a backend still allocates nothing on the device.
        self.device_weights = None
        nr_model.FUSE_BRANCHED = True
        self._frames = {}
        self.split = (0.0, 0.0, 0.0)
        self.max_cached_frames = max_cached_frames

    def frame(self, height, width, *, device_image_io=False):
        key = (height, width, bool(device_image_io))
        if key in self._frames and self._frames[key]._closed:
            del self._frames[key]
        if key in self._frames:
            frame = self._frames.pop(key)
        else:
            while len(self._frames) >= self.max_cached_frames:
                self._frames.pop(next(iter(self._frames))).close()
            if self.device_weights is None:
                self.device_weights = self._module.DeviceWeights(self.runtime, self.weights)
            frame = self._module.ResidentFrame(self.runtime, self.device_weights,
                                               height, width, device_image_io=device_image_io)
        self._frames[key] = frame
        return frame

    def close(self):
        for frame in self._frames.values():
            frame.close()
        self._frames.clear()
        # closing means everything, including the weights; the next frame uploads them
        # again, which is what this cost before they were shared
        if self.device_weights is not None:
            self.device_weights.close()
            self.device_weights = None

    def input_view(self, height, width):
        """The mapped input of the frame at this extent, to build the features in: then
        `run_features` has nothing to copy."""
        return self.frame(height, width).input_view()

    def run_features(self, features):
        height, width = features.shape[:2]
        frame = self.frame(height, width)
        head = frame.run(features)
        self.split = frame.split          # host write, graph, host read
        return head


def run_head(model, color, *, profile="standard", frame_index=0, style_index=None,
             local_tone=None, local_structure=None, automatic_mask=None,
             control_mask=None, mask_blend_mode=0, verbose=False):
    """The network alone: (H, W, 3) colour -> the cropped (H, W, 4) head.

    Everything that reaches the network is decided here — the conditioning scalars
    land in feature channels 10-14, so changing any of them costs a forward pass.
    The head can then be composed at any `intensity` for free.
    """
    height, width = color.shape[:2]
    geometry = network_geometry(width, height)
    values = controls(profile, style_index, local_tone, local_structure)
    # A profile's own automatic mask, unless the caller gave one.
    profile_mask = values.pop("automatic_mask", None)
    if automatic_mask is None:
        automatic_mask = profile_mask
    features = make_features(color, frame_index=frame_index, geometry=geometry,
                             automatic_mask=automatic_mask, control_mask=control_mask,
                             mask_blend_mode=mask_blend_mode, **values)
    if verbose:
        print(f"  network extent {geometry.network_width}x{geometry.network_height} "
              f"for output {width}x{height}", flush=True)

    started = time.perf_counter()
    marks = [started]

    def progress(name):
        marks.append(time.perf_counter())
        if verbose:
            print(f"    {name:<12} {marks[-1] - marks[-2]:6.2f}s "
                  f"(total {marks[-1] - started:7.2f}s)", flush=True)

    if isinstance(model, ResidentBackend):
        head = model.run_features(features)
    else:
        head = model.forward(features[None], progress=progress if verbose else None)[0]
    return geometry.crop(head), time.perf_counter() - started


def _vendor_prediction(predicted, grade, neural):
    """The prediction after the history's blend, as the vendor goes on with it: kept in
    `neural` truncated to half, and that half graded — or untouched without either."""
    if grade is None and neural is None:
        return predicted
    kept = truncate_half(predicted)
    if neural is not None:
        np.copyto(neural, kept)
    return predicted if grade is None else style_grade(kept, grade)


def compose(head, color, *, intensity=1.0, detail_strength=1.0, colour_strength=1.0,
            detail_radius=4.0, control_mask=None, history=None,
            history_confidence=1.0, history_floor=None, history_previous=None,
            history_hold=0.0, history_release=0.0, blend_scale=BLEND_SCALE,
            grade=None, neural=None):
    """The head over the frame. Post-network and cheap: sweep it without re-running.

    With a `history` image this is MLX-DLSS's `compose_temporal` instead of its
    `compose_head`: the model's own gate decides, per pixel, how much of the previous
    output survives into this one. `history_confidence` scales that gate globally —
    0 is the still-frame path exactly, 1 is the model's own answer.

    The vendor reads the history back out of feature channels 7-9, which pins it to the
    network's extent; a live frame composes at the *output* extent, so it arrives here
    as an image instead. That skips one `scaled_color` round trip through fp16, worth
    at most 0.06 of a 0-255 level, and keeps the game's own pixels off the resampler.

    `history_floor` raises the gate per pixel rather than lowering it, which the vendor
    has no need of and we do. The gate is not local: on a frame where most things move it
    reads 0.12 even over pixels that did not move, against 0.705 on a scene where the
    history was correct everywhere (`notes/phase54-flicker-fix.md`, `phase12`). A caller
    that can prove the history is correct for a pixel — because the game handed back the
    same bytes — can say so here. The floor is still bounded by `blend_scale`, so no
    pixel is held harder than the model itself ever holds one.

    `history_release` is the other half of the same signal: where the game's pixel changed
    by `history_release` levels of 255 or more, none of the gate survives, and by less, a
    share that falls linearly with the change. Without motion vectors the history at a
    pixel something moved across is what was there before, and a gate that reads 0.6 over
    a whole Tekken frame kept a trail of it behind everything that moved. It lowers only
    the gate — never the floor, which is zero wherever the game changed a pixel anyway.

    `intensity` blends the model's picture against the source, per pixel when a
    ControlMask supplies its red channel. `detail_strength` and `colour_strength`
    then re-weight the high and low frequencies of whatever change remains.

    Above 1 the blend extrapolates past the model's own picture. MLX-DLSS's
    `compose_head` clamps it, so anything over 1 was silently a no-op here; the
    vendor's panel goes to 2 and ships screenshots at 1.66
    (`notes/phase30-control-atlas.md`). At or below 1 this is bit-identical to the
    clamped path — the extrapolation only replaces the final blend, and the residual,
    its half rounding and the [0,1] clamp on the result are unchanged.

    `grade`, from `grade_for`, is the vendor's colour grade for a style, and `neural`, an
    array of the colour's shape, receives what the vendor keeps as the next frame's
    history: the prediction after the history's blend and before the grade, the intensity
    or anything else, truncated to half. The grade reads that same half value, as the
    vendor's post-process reads it back out of its RGBA16F scratch
    (`notes/phase70-post-process.md`). With neither, nothing here changes by a bit.
    """
    composed = None
    if history is not None or intensity > 1.0:
        head = np.asarray(head, dtype=np.float32)
        color = np.asarray(color, dtype=np.float32)
        alpha = None
        if history is not None:
            if head.shape[2] < 4:
                raise ValueError("the temporal gate is head channel 4; pass all four")
            history = np.asarray(history, dtype=np.float32)
            if history.shape != color.shape:
                raise ValueError("history must match the colour image shape")
            previous = (history_previous if history_previous is not None
                        and (history_hold > 0 or history_release > 0) else None)
            slope_release = release_slope(history_release)
            if nr_image is not None and history_floor is None:
                # Everything temporal in the one native pass: the gate from its table (the
                # exp that kept it in NumPy is inside the table), the confidence, and the
                # floor from the game's previous frame in the same folded multiply-add-clip
                # as `hold_floor`. 7 ms of NumPy at a 1280x720 output.
                confidence = (float(np.clip(np.float32(history_confidence), 0, 1))
                              if history_confidence != 1.0 else 1.0)
                composed = nr_image.compose_temporal(
                    head, color, history, previous, None, control_mask,
                    intensity=intensity, blend_scale=blend_scale,
                    hold=float(history_hold) if previous is not None else 0.0,
                    slope=(float(np.float32(-255.0 * history_hold / HOLD_RAMP))
                           if previous is not None else 0.0),
                    table=gate_table(float(blend_scale)), confidence=confidence,
                    release=slope_release if previous is not None else 0.0,
                    grade=grade, neural=neural)
            if previous is not None and history_floor is None and composed is None:
                history_floor = hold_floor(color, previous, history_hold)
            alpha = (history_weight(head, blend_scale=blend_scale)
                     if composed is None else None)
            if composed is None and history_confidence != 1.0:
                alpha = alpha * np.clip(np.float32(history_confidence), 0, 1)
            if composed is None and previous is not None and slope_release != 0.0:
                alpha = alpha * release_factor(color, previous, slope_release)
            if composed is None and history_floor is not None:
                floor = np.clip(np.asarray(history_floor, dtype=np.float32), 0, 1)
                np.maximum(alpha, floor * np.float32(blend_scale), out=alpha)
            if composed is None and nr_image is not None:
                composed = nr_image.compose_temporal(
                    head, color, history, None, alpha, control_mask,
                    intensity=intensity, blend_scale=blend_scale, hold=0.0, slope=0.0,
                    grade=grade, neural=neural)
        elif nr_image is not None and control_mask is None:
            composed = nr_image.compose(head, color, intensity, grade=grade, neural=neural)
        if composed is None:
            residual = features_mod.half(head[..., :3]) * np.float32(0.25)
            predicted = np.clip(color + residual, 0, 1)
            if history is not None:
                predicted += alpha * (history - predicted)
            predicted = _vendor_prediction(predicted, grade, neural)
            blend = np.float32(intensity)
            if control_mask is not None:
                blend = np.asarray(control_mask, dtype=np.float32)[..., :1] * blend
            composed = np.clip(color + blend * (predicted - color), 0, 1).astype(np.float32)
    else:
        # The still frame natively too: nr_compose clamps the blend to [0, 1] below 1 as
        # compose_head does, and is byte-identical to it (test_native_image.py). It was
        # only reached above 1, so photo mode and every cut paid 3.9 ms of NumPy at
        # 512x288 for the same bytes.
        if nr_image is not None and control_mask is None:
            composed = nr_image.compose(head, color, intensity, grade=grade, neural=neural)
        if composed is None and (grade is not None or neural is not None):
            # compose_head, with the prediction the vendor keeps and grades in the middle
            head = np.asarray(head, dtype=np.float32)
            color = np.asarray(color, dtype=np.float32)
            predicted = np.clip(color + features_mod.half(head[..., :3]) * np.float32(0.25),
                                0, 1)
            predicted = _vendor_prediction(predicted, grade, neural)
            blend = np.float32(intensity)
            if control_mask is not None:
                blend = np.asarray(control_mask, dtype=np.float32)[..., :1] * blend
            blend = np.clip(blend, 0, 1)
            composed = np.clip(color + blend * (predicted - color), 0, 1).astype(np.float32)
        if composed is None:
            composed = compose_head(head, color, control_mask=control_mask,
                                    intensity=intensity)
    return compose_detail(color, composed, detail_strength=detail_strength,
                          colour_strength=colour_strength, radius=detail_radius)


def compose_encode(head, color, encoded, *, top=0, left=0, bgra=True, intensity=1.0,
                   control_mask=None, history=None, history_confidence=1.0,
                   history_previous=None, history_hold=0.0, history_release=0.0,
                   blend_scale=BLEND_SCALE, samples=None, grade=None, neural=None):
    """`compose` of the head brought up to the colour's size, encoded into `encoded` at
    (`top`, `left`), in one native pass; `None` where that pass does not apply, and then
    nothing has been written.

    The same composition as `resample` then `compose` then `nr_daemon.encode`, byte for
    byte (`test_native_image.py`), for the cases the daemon meets live: a history with or
    without a control mask, or a still frame without one, at detail and colour strength 1
    — where `compose_detail` hands the composition back untouched. The parameters are
    derived exactly as `compose`'s native branches derive them. With `samples`, a step,
    returns `(composition, head samples)` — see `nr_image.compose_encode`. `grade` and
    `neural` are `compose`'s.
    """
    if nr_image is None or (history is None and control_mask is not None):
        return None
    if history is None:
        return nr_image.compose_encode(head, color, None, None, None, encoded, top=top,
                                       left=left, bgra=bgra, intensity=intensity,
                                       samples=samples, grade=grade, neural=neural)
    previous = (history_previous if history_previous is not None
                and (history_hold > 0 or history_release > 0) else None)
    confidence = (float(np.clip(np.float32(history_confidence), 0, 1))
                  if history_confidence != 1.0 else 1.0)
    return nr_image.compose_encode(
        head, color, history, previous, control_mask, encoded, top=top, left=left,
        bgra=bgra, intensity=intensity, blend_scale=blend_scale,
        hold=float(history_hold) if previous is not None else 0.0,
        slope=(float(np.float32(-255.0 * history_hold / HOLD_RAMP))
               if previous is not None else 0.0),
        table=gate_table(float(blend_scale)), confidence=confidence,
        release=release_slope(history_release) if previous is not None else 0.0,
        samples=samples, grade=grade, neural=neural)


def run_frame(model, color, *, intensity=1.0, detail_strength=1.0, colour_strength=1.0,
              detail_radius=4.0, control_mask=None, **head_options):
    """color: (H, W, 3) float32 in [0,1] -> (H, W, 3) float32, with the style's grade."""
    head, elapsed = run_head(model, color, control_mask=control_mask, **head_options)
    grade = grade_for(**controls(head_options.get("profile", "standard"),
                                 head_options.get("style_index"), head_options.get("local_tone"),
                                 head_options.get("local_structure")))
    output = compose(head, color, intensity=intensity, detail_strength=detail_strength,
                     colour_strength=colour_strength, detail_radius=detail_radius,
                     control_mask=control_mask, grade=grade)
    return output, head, elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--weights", default=str(WEIGHTS))
    parser.add_argument("--size", help="resize input first, as HxW")
    parser.add_argument("--profile", default="standard", choices=sorted(PROFILES),
                        help="preset for style/tone/structure; overridden by the flags below")
    parser.add_argument("--style-index", type=int,
                        help="vendor style index, normalised by 1/128 (0, 1, 2 are the presets)")
    parser.add_argument("--local-tone", type=float,
                        help="local tone strength, 0..2 (the vendor's range; default 1.0)")
    parser.add_argument("--local-structure", type=float,
                        help="local structure strength, 0..2 (vendor default 1.5)")
    parser.add_argument("--skin-structure", type=float,
                        help="skin structure strength; enables automatic masking, -1 to follow local structure")
    parser.add_argument("--auto-mask", type=float,
                        help="automatic-mask structure strength; -1 to follow local structure")
    parser.add_argument("--control-mask", help="per-pixel RGB mask: red intensity, green tone, blue structure")
    parser.add_argument("--mask-blend-mode", type=int, default=0, choices=sorted(MASK_BLEND_MODES),
                        help="with a control mask and the automatic mask, 310.9.16's "
                             "DLSSNR.MaskBlendMode: 0 off, 1 average, 2 minimum, 3 maximum")
    parser.add_argument("--intensity", type=float, default=1.0,
                        help="blend of the model's picture against the source, 0..2; "
                             "above 1 extrapolates past the model's own picture")
    parser.add_argument("--intensity-ladder",
                        help="comma-separated intensities; renders once and writes one file each")
    parser.add_argument("--detail-strength", type=float, default=1.0)
    parser.add_argument("--colour-strength", type=float, default=1.0)
    parser.add_argument("--detail-radius", type=float, default=4.0)
    parser.add_argument("--frame-index", type=int, default=0)
    parser.add_argument("--gpu", action="store_true",
                        help="run the weight GEMMs on the Xe2 XMX units")
    parser.add_argument("--resident", action="store_true",
                        help="run the whole graph on the GPU, activations never "
                             "returning to the host (the fastest path)")
    parser.add_argument("--accel", action="store_true",
                        help="use torch's SIMD half and E4M3 conversions (bit-identical)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.accel:
        import nr_accel
        if nr_accel.install():
            print("accel   torch rounding, verified bit-identical", flush=True)
        else:
            print("accel   requested but torch is not installed", flush=True)

    if args.resident:
        started = time.perf_counter()
        model = ResidentBackend(args.weights)
        print(f"resident backend ready in {time.perf_counter() - started:.1f}s", flush=True)

    backend = None
    if args.gpu and not args.resident:
        sys.path.insert(0, str(ROOT / "src" / "gpu"))
        import nr_xmx
        backend = nr_xmx
        print(f"backend {backend.install()}", flush=True)

    size = None
    if args.size:
        size = tuple(int(part) for part in args.size.lower().split("x"))
    color = image_io.load(args.input, size=size)
    print(f"input {color.shape[1]}x{color.shape[0]}", flush=True)

    if not args.resident:
        started = time.perf_counter()
        model = nr_model.NeuralRenderingModel.from_safetensors(args.weights)
        print(f"weights loaded in {time.perf_counter() - started:.1f}s "
              f"({len(model.weights)} tensors)", flush=True)

    mask = image_io.load(args.control_mask) if args.control_mask else None
    automatic = None
    if args.skin_structure is not None or args.auto_mask is not None:
        automatic = AutomaticMask(
            skin_structure_strength=(args.skin_structure
                                     if args.skin_structure is not None else -1.0),
            automatic_mask_structure_strength=(args.auto_mask
                                               if args.auto_mask is not None else -1.0))

    head, elapsed = run_head(
        model, color, profile=args.profile, frame_index=args.frame_index,
        style_index=args.style_index, local_tone=args.local_tone,
        local_structure=args.local_structure, automatic_mask=automatic,
        control_mask=mask, mask_blend_mode=args.mask_blend_mode, verbose=args.verbose)
    print(f"network {elapsed:.1f}s")
    if backend is not None:
        print(backend.report())
    print(f"head    min {head.min():+.4f} max {head.max():+.4f} sd {head.std():.4f}")
    grade = grade_for(**controls(args.profile, args.style_index, args.local_tone,
                                 args.local_structure))
    if grade is not None:
        print(f"style grade: exposure x{grade[0]:.4f}, contrast {grade[1]:+.3f}, "
              f"saturation x{grade[2]:.3f}")

    def emit(path, intensity):
        output = compose(head, color, intensity=intensity,
                         detail_strength=args.detail_strength,
                         colour_strength=args.colour_strength,
                         detail_radius=args.detail_radius, control_mask=mask, grade=grade)
        residual = output - color
        print(f"intensity {intensity:5.2f}  change mean|d| {np.abs(residual).mean():.5f} "
              f"max|d| {np.abs(residual).max():.5f}")
        image_io.save(output, path)
        print(f"wrote {path}", flush=True)

    if args.intensity_ladder:
        stem = pathlib.Path(args.output)
        for value in (float(part) for part in args.intensity_ladder.split(",")):
            emit(str(stem.with_name(f"{stem.stem}_i{value:g}{stem.suffix}")), value)
    else:
        emit(args.output, args.intensity)


if __name__ == "__main__":
    main()
