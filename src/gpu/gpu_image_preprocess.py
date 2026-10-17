"""Record exact SDR image preparation into the resident Vulkan command stream.

``record`` consumes and returns device buffers. It never maps/downloads pixels.
The only host array is the deterministic noise table, built once at construction;
rebuilding it per frame would defeat the image path and lose reference parity.
The caller owns source/target/history buffers and waits for completion before
reusing/freeing them. This module does not submit or hide a synchronous readback.
"""
from __future__ import annotations

import ctypes
import math
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
RGBA8_FORMATS = frozenset((37, 43))
BGRA8_FORMATS = frozenset((44, 50))
FORMATS = RGBA8_FORMATS | BGRA8_FORMATS
BGRA, HISTORY, SOURCE_RGB32, OUTPUT_RGB32 = 1, 2, 4, 8
UINT32_MAX = (1 << 32) - 1


def _positive_int(value, name):
    if not isinstance(value, (int, np.integer)) or isinstance(value, (bool, np.bool_)):
        raise ValueError(f"{name} must be a positive integer")
    value = int(value)
    if value < 1 or value > UINT32_MAX:
        raise ValueError(f"{name} must fit positive uint32")
    return value


class ImagePreprocessPush(ctypes.Structure):
    """96-byte scalar layout consumed by image_preprocess.comp."""
    _fields_ = [
        ("raw", ctypes.c_uint64), ("history", ctypes.c_uint64),
        ("features", ctypes.c_uint64), ("noise", ctypes.c_uint64),
        *((name, ctypes.c_uint32) for name in
          ("source_w", "source_h", "left", "top", "active_w", "active_h",
           "inner_w", "inner_h", "network_w", "network_h", "flags")),
        *((name, ctypes.c_float) for name in ("ratio_y", "style", "tone", "structure", "ratio_x")),
    ]


assert ctypes.sizeof(ImagePreprocessPush) == 96


class GPUImageResampler:
    """Reusable device RGB decoder/resampler, with the daemon's exact filters.

    A missing ``vk_format`` selects contiguous RGB32 source storage. Otherwise
    the source is packed SDR RGBA/BGRA; destination is always RGB32. Cropping is
    explicit: automatic letterbox detection is not part of this pass.
    """

    def __init__(self, runtime, source_size, target_size, *, crop=None,
                 vk_format=None, shader_path=None):
        self.runtime, self.lib = runtime, runtime.lib
        self.height, self.width = self._size(source_size, "source")
        self.vk_format = vk_format
        if vk_format is not None and vk_format not in FORMATS:
            raise ValueError(f"unsupported GPU preprocess VkFormat {vk_format}; use CPU fallback")
        if self.width * self.height > UINT32_MAX // (4 if vk_format is not None else 3):
            raise ValueError("source image addressing exceeds uint32")
        self.crop = tuple(crop) if crop is not None else (0, self.height, 0, self.width)
        if (len(self.crop) != 4 or any(not isinstance(v, (int, np.integer)) for v in self.crop)):
            raise ValueError("crop must contain four integers")
        self.top, bottom, self.left, right = (int(v) for v in self.crop)
        if not (0 <= self.top < bottom <= self.height and 0 <= self.left < right <= self.width):
            raise ValueError("crop must be a nonempty region inside the source image")
        self.active_h, self.active_w = bottom - self.top, right - self.left
        self.inner_h, self.inner_w = self._size(target_size, "target")
        self.network_h, self.network_w = self.inner_h, self.inner_w
        # NumPy narrows its Python extent/count ratio to float32 before applying
        # it to pixel centres. GPU OpFDiv is allowed to differ in its last bits.
        self.ratio_y = float(np.float32(self.active_h / self.inner_h))
        self.ratio_x = float(np.float32(self.active_w / self.inner_w))
        if self.inner_h * self.inner_w > UINT32_MAX // 3:
            raise ValueError("output image addressing exceeds uint32")
        self.source_nbytes = self.width * self.height * (4 if vk_format is not None else 12)
        self.output_nbytes = self.inner_h * self.inner_w * 12
        import xmxres
        path = pathlib.Path(shader_path) if shader_path else ROOT / "work" / "image_preprocess.spv"
        self.pipeline = xmxres.ImagePipeline(path)

    @staticmethod
    def _size(size, name):
        if len(size) != 2:
            raise ValueError(f"{name}_size must be (height, width)")
        return tuple(_positive_int(v, f"{name}_{axis}") for v, axis in zip(size, ("height", "width")))

    def _buffer(self, buffer, nbytes, name):
        if (buffer is None or getattr(buffer, "id", -1) < 0
                or getattr(buffer, "nbytes", 0) < nbytes):
            raise ValueError(f"{name} must be a live device buffer of at least {nbytes} bytes")
        if getattr(buffer, "_lib", self.lib) is not self.lib:
            raise ValueError(f"{name} belongs to another Vulkan runtime")

    def _push(self, source, target, *, history=None, noise=None, flags=0, controls=(0, 0, 0)):
        return ImagePreprocessPush(
            source.address, history.address if history is not None else 0,
            target.address, noise.address if noise is not None else 0,
            self.width, self.height, self.left, self.top, self.active_w, self.active_h,
            self.inner_w, self.inner_h, self.network_w, self.network_h, flags,
            self.ratio_y, *controls, self.ratio_x)

    def record(self, source, target):
        import xmxres        # deferred, as in __init__: this module is imported on platforms with no libxmx
        self._buffer(source, self.source_nbytes, "source")
        self._buffer(target, self.output_nbytes, "target")
        if source.id == target.id:
            raise ValueError("resampling source and target must not alias")
        flags = OUTPUT_RGB32 | (SOURCE_RGB32 if self.vk_format is None else 0)
        if self.vk_format in BGRA8_FORMATS:
            flags |= BGRA
        self.pipeline.record(self._push(source, target, flags=flags),
                             *xmxres.linear_grid((self.network_w * self.network_h + 63) // 64))
        self.runtime.recorded += 1
        return target


class GPUImagePreprocessor(GPUImageResampler):
    """Fixed extents/crop with one cached device noise field.

    ``inner_size`` and ``network_size`` are (height, width), as NumPy images.
    ``crop`` is (top, bottom, left, right), as the daemon's letterbox contract.
    Select geometry with nr_frame.render_extent/network_geometry outside this
    class; a geometry/crop change constructs a new instance before recording.
    History is contiguous float32 RGB at the inner image extent, without padding.
    Control masks and automatic masks are intentionally unsupported here.
    """

    def __init__(self, runtime, width, height, vk_format, *, inner_size=None,
                 network_size=None, crop=None, frame_index=0, shader_path=None):
        width, height = _positive_int(width, "width"), _positive_int(height, "height")
        if vk_format not in FORMATS:
            raise ValueError(f"unsupported GPU preprocess VkFormat {vk_format}; use CPU fallback")
        selected_crop = tuple(crop) if crop is not None else (0, height, 0, width)
        # Validate the region in the resampler before deriving its extent.
        if (len(selected_crop) != 4
                or any(not isinstance(v, (int, np.integer)) for v in selected_crop)):
            raise ValueError("crop must contain four integers")
        active = (selected_crop[1] - selected_crop[0], selected_crop[3] - selected_crop[2])
        super().__init__(runtime, (height, width), inner_size or active, crop=selected_crop,
                         vk_format=int(vk_format), shader_path=shader_path)
        if self.inner_h > self.active_h or self.inner_w > self.active_w:
            raise ValueError("GPU preparation only downsamples the active image")
        self.network_h, self.network_w = self._size(network_size or (self.inner_h, self.inner_w), "network")
        if self.network_h < self.inner_h or self.network_w < self.inner_w:
            raise ValueError("network field must cover the inner image")
        # Shader pixel*16 and history pixel*3 indexing use uint32.
        if self.network_h * self.network_w > UINT32_MAX // 16:
            raise ValueError("feature addressing exceeds uint32")
        self.feature_nbytes = self.network_h * self.network_w * 16 * 2
        self.history_nbytes = self.inner_h * self.inner_w * 3 * 4
        if not isinstance(frame_index, (int, np.integer)):
            raise ValueError("frame_index must be an integer")
        self.frame_index = int(frame_index)
        self.noise = None
        # Match the existing native feature contract exactly. Transcendental
        # implementation differences otherwise affect the final FP16 feature bits.
        if str(ROOT / "src" / "ref") not in sys.path:
            sys.path.insert(0, str(ROOT / "src" / "ref"))
        import nr_frame
        values = nr_frame.deterministic_noise(self.network_h, self.network_w,
                                             self.frame_index).astype(np.float16)
        self.noise = runtime.buffer(values.size, np.float16)
        try:
            self.noise.upload(values)
        except Exception:
            self.noise.free()
            self.noise = None
            raise

    def allocate_features(self):
        """Allocate device storage suitable for ResidentFrame.run_device()."""
        import xmxres
        return self.runtime.buffer(self.feature_nbytes // 2, np.float16, kind=xmxres.DEVICE)

    def record(self, raw, target, *, history=None, normalized_style=0.0,
               local_tone_strength=1.0, local_structure_strength=1.0,
               control_mask=None, automatic_mask=None):
        """Record one pass in a begun command stream; return ``target`` unchanged.

        It is legal to change controls and the optional history each frame. The
        graph input must stay FP16; run_device can consume it without CPU copies.
        This wrapper checks buffer extents before dispatch. The recorder inserts
        a visibility barrier before the next resident pass consumes the features.
        """
        if control_mask is not None or automatic_mask is not None:
            raise ValueError("GPU feature masks are unsupported; use CPU fallback")
        import xmxres        # deferred, as in __init__
        if self.noise is None:
            raise RuntimeError("the GPU image preprocessor is closed")
        self._buffer(raw, self.width * self.height * 4, "raw")
        self._buffer(target, self.feature_nbytes, "target")
        if history is not None:
            self._buffer(history, self.history_nbytes, "history")
        inputs = {raw.id, self.noise.id}
        if history is not None:
            inputs.add(history.id)
        if target.id in inputs:
            raise ValueError("features must not alias raw pixels, history or noise")
        controls = (normalized_style, local_tone_strength, local_structure_strength)
        if not all(math.isfinite(float(v)) and abs(float(v)) <= 65504 for v in controls):
            raise ValueError("feature controls must be finite FP16 values")
        flags = (BGRA if self.vk_format in BGRA8_FORMATS else 0) | (HISTORY if history is not None else 0)
        self.pipeline.record(self._push(raw, target, history=history, noise=self.noise,
                                        flags=flags, controls=tuple(float(v) for v in controls)),
                             *xmxres.linear_grid((self.network_w * self.network_h + 63) // 64))
        self.runtime.recorded += 1
        return target

    def close(self):
        if self.noise is not None:
            self.noise.free()
            self.noise = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
