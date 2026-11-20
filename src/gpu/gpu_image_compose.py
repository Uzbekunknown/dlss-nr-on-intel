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
BGRA, HISTORY, PREVIOUS, KEEP, GRADE, COMPOSED, MOVED = 1, 2, 4, 8, 16, 32, 64
PREVIOUS_RAW = 512
# Detail and colour strength (image_compose.comp): the composition hands on its change, then the
# Gaussian's rows and its columns with the strengths' sum, as `compose_detail` on the CPU.
DETAIL_ROWS, DETAIL_COLUMNS, DETAIL_CHANGE = 1024, 2048, 4096
# The CPU path's radius (`nr_frame.compose`'s `detail_radius`), the sigma of its Gaussian, and the
# most taps the weights' buffer holds: 2 * ceil(3 * sigma) + 1, so radii to 10.
DETAIL_RADIUS = 4.0
DETAIL_TAPS = 64


class _Push(ctypes.Structure):
    _fields_ = [("buffers", ctypes.c_uint64 * 8), ("params", ctypes.c_uint32 * 16)]


assert ctypes.sizeof(_Push) == 128


def fallback_reason(*, vk_format, detail_strength=1.0, colour_strength=1.0,
                    interface=False, **_):
    """A reason callers can log before choosing the existing CPU composition.

    Detail and colour strength away from 1 are composed here since 2026-11-21. Until then they
    were a reason, and in DirectX 12, which has no CPU path, the daemon declined every frame while
    either was moved: NR went off, and the panel's network graph emptied (the owner, Hogwarts
    Legacy, 2026-11-20)."""
    if vk_format not in SDR_FORMATS:
        return "GPU composition supports SDR RGBA8/BGRA8 only"
    if not (math.isfinite(float(detail_strength)) and math.isfinite(float(colour_strength))):
        return "GPU composition requires finite detail and colour strength"
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
        # the float32 composition, for the change metric alone: made at the first `record`
        # that asks for it (`composed`), so a daemon with the metric off never holds it
        self.composed = None
        self.settings = runtime.buffer(16 + 256 + 65536, kind=xmxres.DEVICE)
        self._blend_scale = None
        self._settings = np.zeros(16, np.float32)
        self._uploaded_settings = None
        # Detail and colour strength: the Gaussian's weights, and the rows' pass of the change,
        # made when a strength first moves away from 1.
        self.detail_strength = self.colour_strength = 1.0
        self.weights = self.rows = None
        self._radius = None
        self._taps = 0
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
                  grade=None, keep_neural=True, detail_strength=1.0, colour_strength=1.0,
                  detail_radius=DETAIL_RADIUS):
        """Upload changed controls; regenerate the gate LUT only if needed.

        Identical scalar bits need no staged transfer/fence. Keep/grade flags
        are push constants and still update independently of the cached bytes.
        `detail_strength` and `colour_strength` are push constants too; the Gaussian's weights
        for `detail_radius` are uploaded when a strength first leaves 1, and again only when
        the radius changes.
        """
        if self._closed:
            raise RuntimeError("GPUImageComposer is closed")
        values = (intensity, history_confidence, history_hold, history_release, blend_scale,
                  detail_strength, colour_strength, detail_radius)
        if not all(math.isfinite(float(v)) for v in values):
            raise ValueError("GPU composition settings must be finite")
        if not detail_radius > 0:
            raise ValueError("the detail radius must be positive")
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
        # The strengths as the CPU applies them, float32; the weights first, so a failed upload
        # leaves the strengths that were in force, never a pass without its weights.
        detail_strength = float(np.float32(detail_strength))
        colour_strength = float(np.float32(colour_strength))
        if (detail_strength != 1.0 or colour_strength != 1.0) and self._radius != float(detail_radius):
            kernel = np.asarray(nr_frame.composition_mod.gaussian_kernel(float(detail_radius)), np.float32)
            if kernel.size > DETAIL_TAPS:
                raise ValueError(f"a detail radius of {detail_radius} needs more than {DETAIL_TAPS} taps")
            if self.weights is None:
                self.weights = self.rt.buffer(DETAIL_TAPS, kind=xmxres.DEVICE)
            self.weights.upload(kernel)
            self._radius, self._taps = float(detail_radius), int(kernel.size)
        self.detail_strength, self.colour_strength = detail_strength, colour_strength

    @property
    def detailed(self):
        """Whether `compose_detail` changes anything: a strength away from 1."""
        return self.detail_strength != 1.0 or self.colour_strength != 1.0

    @staticmethod
    def _check_buffer(buffer, nbytes, name):
        if buffer is None or buffer.nbytes < nbytes or buffer.id < 0:
            raise ValueError(f"{name} buffer is missing, freed or too small")

    def record(self, raw, head, *, history=None, previous=None, target=None, composed=True,
               moved=None, previous_raw=False):
        """Record one dispatch; buffers/commands must outlive submit completion.

        `target`: where the encoded frame goes, every pixel of it, instead of this composer's
        own buffer - the transport's shared output, which then needs no copy. Returned.
        `composed`: also store the float32 composition in `self.composed`, which only the
        change metric reads (gpu_image_pipeline, NR_CHANGE_METRIC); without it the encoded
        frame and the neural output are the same bytes.
        `moved`: (the motion's device address, STALE), with `history` and `previous` the
        network's last output and the game's last frame where they were: each is read along
        the motion here, as GPUImageFollower's active-extent pass reads them into `full` and
        `pixels`, the same bits without those two images written and read back (MOVED). The
        motion is two float32 a pixel at the frame's extent.
        `previous_raw`: `previous` is the game's last frame as the bytes it came in, at the
        frame's extent, as `raw` is, decoded here exactly as the decode pass would (PREVIOUS_RAW);
        not with `moved`, whose reads of it are five-tap ones."""
        if self._closed:
            raise RuntimeError("GPUImageComposer is closed")
        self._check_buffer(raw, self.frame_width * self.frame_height * 4, "raw")
        self._check_buffer(head, self.head_height * self.head_row_width * self.head_channels * 4, "head")
        if previous_raw and (previous is None or moved is not None):
            raise ValueError("the last frame's bytes are read where it is, beside a history")
        for name, buf, nbytes in (("history", history, self.width * self.height * 3 * 4),
                                  ("previous", previous, self.frame_width * self.frame_height * 4
                                   if previous_raw else self.width * self.height * 3 * 4)):
            if buf is not None:
                self._check_buffer(buf, nbytes, name)
        if previous is not None and history is None:
            raise ValueError("previous game pixels require temporal history")
        if moved is not None:
            address, stale = moved
            if history is None or previous is None:
                raise ValueError("a history read along motion needs the last output and frame")
            if not isinstance(address, int) or address <= 0 or address % 4 or not math.isfinite(stale):
                raise ValueError("motion must be a float-aligned device address and STALE finite")
        if history is not None and history.id == self.neural.id:
            raise ValueError("history and next neural output must be distinct buffers")
        if target is not None:
            self._check_buffer(target, self.frame_width * self.frame_height * 4, "target")
            if any(buf is not None and buf.id == target.id
                   for buf in (raw, head, history, previous, self.neural, self.composed)):
                raise ValueError("the encoded target must not alias an operand")
        encoded = self.encoded if target is None else target
        detailed = self.detailed
        if (composed or detailed) and self.composed is None:
            self.composed = self.rt.buffer(self.width * self.height * 3, kind=xmxres.DEVICE)
        if detailed and self.rows is None:
            self.rows = self.rt.buffer(self.width * self.height * 3, kind=xmxres.DEVICE)
        flags = ((BGRA if self.bgra else 0) | (HISTORY if history is not None else 0)
                 | (PREVIOUS if previous is not None else 0) | (KEEP if self.keep_neural else 0)
                 | (GRADE if self.has_grade else 0)
                 | (DETAIL_CHANGE if detailed else COMPOSED if composed else 0)
                 | (MOVED if moved is not None else 0) | (PREVIOUS_RAW if previous_raw else 0))
        operands = (raw, head, history, previous, encoded, self.neural,
                    self.composed if composed or detailed else None, self.settings)
        push = _Push()
        push.buffers[:] = [b.address if b is not None else 0 for b in operands]
        top, bottom, left, right = self.crop
        push.params[:11] = (self.frame_width, self.frame_height, left, top, self.width,
                            self.height, self.head_width, self.head_height,
                            self.head_row_width, self.head_channels, flags)
        if moved is not None:
            push.params[11:14] = (int(np.float32(moved[1]).view(np.uint32)),
                                  moved[0] & 0xffffffff, moved[0] >> 32)
        self.pipeline.record(push, *xmxres.linear_grid(-(-self.frame_width * self.frame_height // 256)))
        if detailed:
            self._record_detail(raw, encoded)
        return encoded

    def _record_detail(self, raw, encoded):
        """`compose_detail` after the composition: the rows' pass of its change into `rows`, then
        the columns' pass and the strengths' sum, the composition into `composed` (what the
        change metric reads) and the bytes into `encoded`, the active rectangle only (the
        composition wrote the rest). Each pass behind a barrier, as every image pass is."""
        top, bottom, left, right = self.crop
        grid = xmxres.linear_grid(-(-self.width * self.height // 256))
        for mode in (DETAIL_ROWS, DETAIL_COLUMNS):
            push = _Push()
            operands = (raw, None, self.rows, None, encoded, self.weights, self.composed, self.settings)
            push.buffers[:] = [b.address if b is not None else 0 for b in operands]
            push.params[:6] = (self.frame_width, self.frame_height, left, top, self.width, self.height)
            push.params[10] = mode | (BGRA if self.bgra else 0)
            push.params[11] = int(np.float32(self.colour_strength).view(np.uint32))
            push.params[12] = int(np.float32(self.detail_strength).view(np.uint32))
            push.params[13] = self._taps
            push.params[14] = (self._taps - 1) // 2
            self.pipeline.record(push, *grid)

    def close(self):
        if not self._closed:
            for buffer in (self.encoded, self.neural, self.composed, self.settings, self.weights,
                           self.rows):
                if buffer is not None:
                    buffer.free()
            self._closed = True

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
