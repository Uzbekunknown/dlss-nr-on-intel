"""History carried along the client's motion, for the GPU image path.

`History.follow` (nr_daemon.py) pass for pass on the device (image_history.comp): the
network's last output and the game's last frame read where each pixel's surface was on the
frame before, the frame itself where the motion is not known or where what was there is
another surface. Nothing here reads an image back: whether any motion is known, and the
cut against the last frame moved here, come from GPUImageStats' reductions.
"""
from __future__ import annotations

import ctypes
import os
import pathlib

import numpy as np

import xmxres as X

ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTIVE, INNER = 0, 1
# nr_daemon.STALE, which test_image_history.py holds this to: how far in any channel the
# game's last frame moved here may differ from this one before its history is dropped there.
STALE = 0.12


class _Push(ctypes.Structure):
    _fields_ = [("buffers", ctypes.c_uint64 * 8), ("params", ctypes.c_uint32 * 16)]


assert ctypes.sizeof(_Push) == 128


class GPUImageFollower:
    """The follow passes and their outputs for one frame/crop/network geometry.

    ``frame_size`` and ``inner_size`` are (height, width) and ``crop`` is (top, bottom,
    left, right), as the pipeline has them. ``resampled`` is where the caller resamples the
    network's last output to the network's input extent before ``record``. After it: ``full``
    and ``pixels`` (active extent) are the composition's history and previous frame,
    ``inner`` the network's history.
    """

    def __init__(self, runtime, *, frame_size, crop, inner_size, shader_path=None):
        self.rt = runtime
        self.frame_height, self.frame_width = (int(v) for v in frame_size)
        self.crop = tuple(int(v) for v in crop)
        top, bottom, left, right = self.crop
        if not (0 <= top < bottom <= self.frame_height and 0 <= left < right <= self.frame_width):
            raise ValueError("crop must be inside the frame")
        self.height, self.width = bottom - top, right - left
        self.inner_height, self.inner_width = (int(v) for v in inner_size)
        if not (0 < self.inner_height <= self.height and 0 < self.inner_width <= self.width):
            raise ValueError("the network's extent must lie within the active one")
        path = shader_path or os.environ.get("NR_IMAGE_HISTORY_SPV") or ROOT / "work" / "image_history.spv"
        self.pipeline = X.ImagePipeline(str(path))
        active, inner = self.width * self.height, self.inner_width * self.inner_height
        self.buffers = []
        self.full, self.pixels = self._buffer(active * 3), self._buffer(active * 3)
        self.known = self._buffer(active)
        self.resampled, self.inner = self._buffer(inner * 3), self._buffer(inner * 3)
        self.moved, self.known_inner = self._buffer(inner * 3), self._buffer(inner)
        self.zeros = self._buffer(active)
        self.zeros.zero()
        self._closed = False

    def _buffer(self, count):
        buffer = self.rt.buffer(count, np.float32, kind=X.DEVICE)
        self.buffers.append(buffer)
        return buffer

    def _push(self, mode, motion, output, game, now, history, moved, known):
        """One pass: ``output`` (the network's) and ``game`` (the game's last frame) read along
        the motion, ``now`` this frame; into ``history``, ``moved`` and ``known``."""
        top, bottom, left, right = self.crop
        width, height = ((self.width, self.height) if mode == ACTIVE
                         else (self.inner_width, self.inner_height))
        push = _Push()
        push.buffers[:7] = (motion, output.address, game.address, now.address, history.address,
                            moved.address, known.address)
        push.params[:9] = (self.frame_width, left, top, self.width, self.height, mode, width, height,
                           int(np.float32(STALE).view(np.uint32)))
        self.pipeline.record(push, -(-width * height // 256))

    def record(self, motion, *, neural, previous, active, previous_inner, inner):
        """Record both passes. ``motion`` is the device address of the frame's offsets, two
        float32 a pixel at the frame's extent; the buffers are the pipeline's own, at the
        active extent (``neural``, ``previous``, ``active``) and the network's (the rest)."""
        if self._closed:
            raise RuntimeError("GPUImageFollower is closed")
        if not isinstance(motion, int) or motion <= 0 or motion % 4:
            raise ValueError("motion must be a float-aligned device address")
        self._push(ACTIVE, motion, neural, previous, active, self.full, self.pixels, self.known)
        self._push(INNER, motion, self.resampled, previous_inner, inner, self.inner, self.moved,
                   self.known_inner)

    def cut(self, stats, inner, limit):
        """The reference's cut against the last frame moved here, over the pixels whose motion
        is known; None when it is known nowhere, which drops the history."""
        share = stats.delta(self.known_inner, self.zeros, self.inner_width, self.inner_height,
                            channels=1)
        if not share > 0:
            # Each pixel at the network's extent reads an active pixel's motion, so only now
            # can the motion be known somewhere and still be known nowhere it reads.
            known = stats.delta(self.known, self.zeros, self.width, self.height, channels=1)
            return 0.0 if known > 0 else None
        apart = stats.delta(inner, self.moved, self.inner_width, self.inner_height,
                            decision_threshold=float(limit) * share)
        return apart / share

    def close(self):
        if not self._closed:
            for buffer in self.buffers:
                buffer.free()
            self.buffers = []
            self._closed = True
