"""Device head -> SDR pixels and pre-grade temporal history.

Only byte packing/alpha is format-dependent. The graph head is float32 with
four useful channels in either its compact or padded sixteen-channel layout.
All full-resolution images remain device buffers; ``record`` does no downloads.
The 256 KiB sigmoid table is generated/uploaded once, preserving CPU gate bits.
"""
from __future__ import annotations

import ctypes
import math
import os
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "ref"))
import nr_frame
import xmxres

SDR_FORMATS = {37: False, 43: False, 44: True, 50: True}
BGRA, HISTORY, PREVIOUS, KEEP, GRADE = 1, 2, 4, 8, 16


class _Push(ctypes.Structure):
    _fields_ = [("buffers", ctypes.c_uint64 * 8), ("params", ctypes.c_uint32 * 16)]


assert ctypes.sizeof(_Push) == 128


def fallback_reason(*, vk_format, detail_strength=1.0, colour_strength=1.0,
                    interface=False, **_):
    """A reason callers can log before choosing the existing CPU composition."""
    if vk_format not in SDR_FORMATS:
        return "GPU composition supports SDR RGBA8/BGRA8 only"
    if detail_strength != 1.0 or colour_strength != 1.0:
        return "GPU composition requires detail and colour strength 1"
    if interface:
        return "GPU composition does not yet support interface protection masks"
    return None


class GPUImageComposer:
    """Composition resources for one active/output/head geometry.

    ``frame_size`` and ``head_size`` are (height,width); ``crop`` is
    (top,bottom,left,right), just like the daemon. ``head_row_width`` is the
    padded network field width. History/previous buffers are active-size RGB32.
    Update settings outside recording; then record after the graph has produced
    its head. There is no full-frame host conversion or readback.
    """

    def __init__(self, runtime, *, frame_size, head_size, vk_format,
                 crop=None, head_row_width=None, head_channels=4,
                 shader_path=None):
        reason = fallback_reason(vk_format=vk_format)
        if reason:
            raise ValueError(reason)
        self.rt = runtime
        self.frame_height, self.frame_width = self._size(frame_size)
        self.head_height, self.head_width = self._size(head_size)
        self.crop = tuple(crop or (0, self.frame_height, 0, self.frame_width))
        if len(self.crop) != 4 or any(type(v) is not int for v in self.crop):
            raise ValueError("crop must contain four integer bounds")
        top, bottom, left, right = self.crop
        if not (0 <= top < bottom <= self.frame_height and
                0 <= left < right <= self.frame_width):
            raise ValueError("crop must be inside the frame")
        self.height, self.width = bottom - top, right - left
        if self.head_height > self.height or self.head_width > self.width:
            raise ValueError("GPU composition upscales a head to the active frame")
        self.head_row_width = int(head_row_width or self.head_width)
        self.head_channels = int(head_channels)
        if self.head_row_width < self.head_width or self.head_channels not in (4, 16):
            raise ValueError("head must be compact four or padded sixteen channels")
        self.bgra = SDR_FORMATS[vk_format]
        path = shader_path or os.environ.get("NR_IMAGE_COMPOSE_SPV") or ROOT / "work" / "image_compose.spv"
        self.pipeline = xmxres.ImagePipeline(str(path))
        self.encoded = runtime.buffer(self.frame_width * self.frame_height, np.uint32, kind=xmxres.DEVICE)
        self.neural = runtime.buffer(self.width * self.height * 3, kind=xmxres.DEVICE)
        self.composed = runtime.buffer(self.width * self.height * 3, kind=xmxres.DEVICE)
        self.settings = runtime.buffer(16 + 256 + 65536, kind=xmxres.DEVICE)
        self._blend_scale = None
        self._settings = np.zeros(16, np.float32)
        self._uploaded_settings = None
        self._closed = False
        self.configure()

    @staticmethod
    def _size(size):
        if len(size) != 2 or any(type(v) is not int or v <= 0 for v in size):
            raise ValueError("size must be two positive integers")
        if size[0] * size[1] > 0x7fffffff // 16:
            raise ValueError("image extent exceeds shader indexing capacity")
        return tuple(size)

    def configure(self, *, intensity=1.0, history_confidence=1.0, history_hold=0.0,
                  history_release=0.0, blend_scale=nr_frame.BLEND_SCALE,
                  grade=None, keep_neural=True):
        """Upload changed controls; regenerate the gate LUT only if needed.

        Identical scalar bits need no staged transfer/fence. Keep/grade flags
        are push constants and still update independently of the cached bytes.
        """
        if self._closed:
            raise RuntimeError("GPUImageComposer is closed")
        values = (intensity, history_confidence, history_hold, history_release, blend_scale)
        if not all(math.isfinite(float(v)) for v in values):
            raise ValueError("GPU composition settings must be finite")
        if not 0 <= history_hold <= 1 or not 0 <= history_release <= 255 or not 0 <= blend_scale <= 1:
            raise ValueError("hold/blend must be 0..1 and release 0..255")
        grade = None if grade is None else np.asarray(grade, np.float32)
        if grade is not None and (grade.shape != (3,) or not np.isfinite(grade).all()):
            raise ValueError("grade must be three finite float32 factors")
        self._settings[:] = 0
        self._settings[:6] = [intensity, np.clip(np.float32(history_confidence), 0, 1),
                             blend_scale, history_hold,
                             np.float32(-255.0 * history_hold / nr_frame.HOLD_RAMP),
                             nr_frame.release_slope(history_release)]
        self._settings[6:9] = grade if grade is not None else (1, 0, 1)
        self._settings[9:11] = [self.head_width / self.width, self.head_height / self.height]
        self._settings[11] = 6
        if self._blend_scale != float(blend_scale):
            table = np.empty(16 + 256 + 65536, np.float32)
            table[:16] = self._settings
            table[16:272] = np.arange(256, dtype=np.float32) / np.float32(255)
            table[272:] = nr_frame.gate_table(float(blend_scale))
            self.settings.upload(table)
            self._blend_scale = float(blend_scale)
            self._uploaded_settings = self._settings.copy()
        elif (self._uploaded_settings is None or
              not np.array_equal(self._settings.view(np.uint32),
                                 self._uploaded_settings.view(np.uint32))):
            self.settings.upload(self._settings)
            # Commit only after a successful transfer, so a failed upload can
            # be retried even when the next configure receives the same values.
            self._uploaded_settings = self._settings.copy()
        self.keep_neural = bool(keep_neural)
        self.has_grade = grade is not None

    @staticmethod
    def _check_buffer(buffer, nbytes, name):
        if buffer is None or buffer.nbytes < nbytes or buffer.id < 0:
            raise ValueError(f"{name} buffer is missing, freed or too small")

    def record(self, raw, head, *, history=None, previous=None):
        """Record one dispatch; buffers/commands must outlive submit completion."""
        if self._closed:
            raise RuntimeError("GPUImageComposer is closed")
        self._check_buffer(raw, self.frame_width * self.frame_height * 4, "raw")
        self._check_buffer(head, self.head_height * self.head_row_width * self.head_channels * 4, "head")
        for name, buf in (("history", history), ("previous", previous)):
            if buf is not None:
                self._check_buffer(buf, self.width * self.height * 3 * 4, name)
        if previous is not None and history is None:
            raise ValueError("previous game pixels require temporal history")
        if history is not None and history.id == self.neural.id:
            raise ValueError("history and next neural output must be distinct buffers")
        flags = ((BGRA if self.bgra else 0) | (HISTORY if history is not None else 0)
                 | (PREVIOUS if previous is not None else 0) | (KEEP if self.keep_neural else 0)
                 | (GRADE if self.has_grade else 0))
        operands = (raw, head, history, previous, self.encoded, self.neural,
                    self.composed, self.settings)
        push = _Push()
        push.buffers[:] = [b.address if b is not None else 0 for b in operands]
        top, bottom, left, right = self.crop
        push.params[:11] = (self.frame_width, self.frame_height, left, top, self.width,
                            self.height, self.head_width, self.head_height,
                            self.head_row_width, self.head_channels, flags)
        self.pipeline.record(push, *xmxres.linear_grid(-(-self.frame_width * self.frame_height // 256)))
        return self.encoded

    def close(self):
        if not self._closed:
            for buffer in (self.encoded, self.neural, self.composed, self.settings):
                buffer.free()
            self._closed = True

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
