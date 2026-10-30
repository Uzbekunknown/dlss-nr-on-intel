#!/usr/bin/env python3
"""
nr_daemon — runs DLSS-NR for the Vulkan layer.

The layer is a shared object inside the game's process and the implementation is
Python, so the frame crosses a Unix socket rather than a function call. For a photo
mode that costs nothing: the game is meant to stall while the frame is being looked
at. A per-frame pass would need the graph ported to C.

    python3 src/layer/nr_daemon.py                        # then, to trigger:
    touch /tmp/nr_trigger        # hold the enhanced frame
    rm /tmp/nr_trigger           # release it

    ENABLE_NR_LAYER=1 VK_LAYER_PATH=... VK_INSTANCE_LAYERS=VK_LAYER_dlssnr_intel \\
    NR_LAYER_SOCKET=/tmp/nr_layer.sock NR_LAYER_TRIGGER=/tmp/nr_trigger <game>

Protocol: 16 bytes of header — magic 'NRN0', width, height, VkFormat — then
width*height*4 bytes of pixels; the same number of bytes come back.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import re
import socket
import stat
import struct
import subprocess
import sys
import time

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# --root has to be honoured before nr_frame is imported (it reads NR_ROOT at import
# time to find work/), so scan argv here and let the parser accept the flag later.
for _i, _arg in enumerate(sys.argv):
    if _arg == "--root" and _i + 1 < len(sys.argv):
        os.environ["NR_ROOT"] = sys.argv[_i + 1]
        break
    if _arg.startswith("--root="):
        os.environ["NR_ROOT"] = _arg.split("=", 1)[1]
        break
if os.environ.get("NR_ROOT"):
    ROOT = pathlib.Path(os.environ["NR_ROOT"])
sys.path.insert(0, str(ROOT / "src" / "ref"))
sys.path.insert(0, str(ROOT / "src" / "gpu"))

import nr_frame  # noqa: E402
import nr_weight_models  # noqa: E402  (which model is installed, and which one the settings want)
import xmx  # noqa: E402  (for the device name, once the model is up)
from xmxres import DeviceLost  # noqa: E402
try:
    import nr_image  # the host passes in C, when they are built
except ImportError:  # pragma: no cover - the NumPy path is the fallback
    nr_image = None

MAGIC = 0x304E524E
MASK_COVERAGE_LIMIT = 0.55   # above this the mask is the scene, not the interface
MAGIC_MASKED = 0x314E524E     # the same, with a one-byte-per-pixel interface mask after the colour
# The same, then for each pixel two float32: the offset in pixels to where its surface was on
# the frame before, NaN where it was not on screen. From a client that has the game's own
# motion vectors (src/xess/nr_xess.c); the history then follows them (`History.follow`).
MAGIC_MOTION = 0x324E524E
MAGIC_GPU = 0x3147524E       # control metadata for same-device external GPU resources

# The swapchain formats a compositor or VKD3D actually hands out. A game writes
# sRGB-encoded values into a UNORM swapchain just as it does into an SRGB one, so both
# decode the same way; what differs is only how the display reads them back.
FORMATS = {
    37: ("rgba8", "R8G8B8A8_UNORM"),
    43: ("rgba8", "R8G8B8A8_SRGB"),
    44: ("bgra8", "B8G8R8A8_UNORM"),
    50: ("bgra8", "B8G8R8A8_SRGB"),
    64: ("a2b10g10r10", "A2B10G10R10_UNORM_PACK32"),
}


def decode(raw, width, height, vk_format):
    kind, _ = FORMATS[vk_format]
    if kind == "a2b10g10r10":
        packed = np.frombuffer(raw, dtype=np.uint32).reshape(height, width)
        channels = [((packed >> shift) & 0x3FF).astype(np.float32) / np.float32(1023.0)
                    for shift in (0, 10, 20)]
        return np.stack(channels, axis=-1)
    if nr_image is not None:
        native = nr_image.decode8(raw, width, height, kind == "bgra8")
        if native is not None:
            return native
    pixels = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 4)
    # `pixels[..., [2, 1, 0]]` gathers into a new array, then `.astype` copies it again,
    # then the divide copies a third time — 170 ms for a 1080p frame. A reversed slice is
    # a *view*, and one ufunc does the widening and the scale together: 3 passes to 1.
    channels = pixels[..., 2::-1] if kind == "bgra8" else pixels[..., :3]
    # `divide`, not a multiply by 1/255: that reciprocal is not representable and the two
    # disagree in the last bit, which a test caught.
    return np.divide(channels, np.float32(255.0), dtype=np.float32)


def encode(image, raw, vk_format):
    """Write `image` back into a copy of `raw`, leaving alpha as the game left it."""
    kind, _ = FORMATS[vk_format]
    if kind == "a2b10g10r10":
        image = np.clip(image, 0.0, 1.0)
        packed = np.frombuffer(raw, dtype=np.uint32).copy().reshape(image.shape[:2])
        quantised = (image * np.float32(1023.0) + 0.5).astype(np.uint32)
        packed &= np.uint32(0xC0000000)
        for index, shift in enumerate((0, 10, 20)):
            packed |= np.minimum(quantised[..., index], 1023) << np.uint32(shift)
        return packed.tobytes()
    if nr_image is not None:
        native = nr_image.encode8(image, raw, kind == "bgra8")
        if native is not None:
            return native
    pixels = np.frombuffer(raw, dtype=np.uint8).copy().reshape(*image.shape[:2], 4)
    # The old form built an int32 intermediate — four bytes a channel for a value that
    # ends up in one — and then copied each channel separately, seven full-frame passes
    # for 424 ms at 1080p. In place, into a strided view, is three.
    quantised = np.multiply(image, np.float32(255.0), dtype=np.float32)
    np.add(quantised, np.float32(0.5), out=quantised)
    np.clip(quantised, 0.0, 255.0, out=quantised)
    target = pixels[..., 2::-1] if kind == "bgra8" else pixels[..., :3]
    target[:] = quantised.astype(np.uint8)
    return pixels.tobytes()


# The buffers a frame arrives in, kept for the next frame of the same size. A fresh one every
# frame is fresh pages every frame — 3.7 MB at 1280x720, page faults on Windows and a new mmap
# on Linux — and nothing holds a request past its answer: the daemon answers one frame at a
# time, decode and encode make arrays of their own, and the answer written into the request's
# bytes is gone, sent, before the next frame arrives.
_INBOX = {}


def receive(connection, count, probe_ok=False, keep=None):
    # Straight into one buffer where the socket allows it: collecting the chunks and
    # joining them copied the whole frame once more, 0.3 ms at 1280x720.
    into = getattr(connection, "recv_into", None)
    buffer = None
    if into is not None:
        buffer = _INBOX.get(keep)
        if buffer is None or len(buffer) != count:
            buffer = bytearray(count)
            if keep is not None:
                _INBOX[keep] = buffer
    view = memoryview(buffer) if buffer is not None else None
    chunks, got = [], 0
    while got < count:
        if into is not None:
            size = into(view[got:], min(1 << 20, count - got))
        else:
            chunk = connection.recv(min(1 << 20, count - got))
            size = len(chunk)
        if not size:
            if probe_ok and not got:
                # Connected and closed without a byte: that is `nr-ctl` or `nr-toggle`
                # asking whether anything is listening. Every status line and every press
                # of the toggle does it, and reporting each as a failed frame buries the
                # daemon's own log in noise. A truncated frame still reports.
                return None
            raise EOFError("the layer closed the connection")
        if into is None:
            chunks.append(chunk)
        got += size
    return buffer if buffer is not None else b"".join(chunks)


SOLID_RADIUS = 3


class Settings:
    """The knobs, re-read from a JSON file whenever it changes.

    The daemon holds the model, so restarting it to try another profile costs a second
    and loses the layer's connection. These are all post-network or extent choices —
    nothing here invalidates the weights — so they can move between frames. `nr-ctl`
    writes the file; anything may, it is one flat object.
    """

    # The panel's knobs. **`preset` is not one of them**: the network is chosen in the setup window
    # (PresetChoice writes `NR_PRESET`) and switched live by the panel's Network row, and it is not
    # a settings-file knob with a default a Reset would restore. It is read in `__init__` below.
    KNOBS = ("profile", "intensity", "detail_strength", "colour_strength", "render_scale",
             "temporal", "cut_limit", "hold", "release", "min_extent")

    def __init__(self, args):
        self.path = args.settings
        self.stamp = None
        # The menu's live preview, beside the settings file.
        #
        # The menu edits in memory and writes the user's settings file only on Save; while they are
        # dragging, what they are trying arrives here instead. It is a partial object - only the
        # knobs they have touched - and it is applied over the saved values, never merged back into
        # them: the saved file is the record and this is a message about right now.
        self.live_path = f"{args.settings}.live" if args.settings else None
        self.live_stamp = None
        # The last bytes read from each file, so a write that shares its predecessor's mtime is
        # still noticed. See `_read_json`.
        self.stamp_bytes = None
        self.live_stamp_bytes = None
        # Whether this daemon has read the preview file yet. The first reading is the one where an
        # inherited, unconfirmed preview has to be refused; after that the user is watching the
        # values change and the flag is no longer the question. See `refresh`.
        self._started = False
        # The last settings object read, kept so the preview can be applied over it on a frame where
        # the saved file itself did not change. See `refresh`.
        self.saved_values = {}
        for knob in self.KNOBS:
            setattr(self, knob, getattr(args, knob, None))
        # **The network is one setting, and it is this one.**
        #
        # `preset` is A or B. It starts from `--preset`, which the proxy fills from `NR_PRESET`;
        # the setup window writes that, and the panel's Network row switches it live through the
        # preview file. There was a `weights` knob beside it that named a MODEL FILE and resolved
        # it through a manifest - two settings for one fact, which is how `NR_PRESET=B` came to run
        # A and how "network preset A" got logged for a B model. Both are gone; see
        # `nr_weight_models` for what replaced them.
        if not getattr(self, "preset", None):
            self.preset = nr_weight_models.preset_of(os.environ.get("NR_PRESET"))
        else:
            self.preset = nr_weight_models.preset_of(self.preset)
        # Whether the `weights`-is-gone notice has been printed. Once, not every refresh.
        self._said_weights_gone = False

    def _apply(self, given, source):
        """The knob values in `given`, validated. Returns the names it changed."""
        changed = []
        for knob in self.KNOBS:
            if knob not in given:
                continue
            value = given[knob]
            if knob == "profile":
                if value not in nr_frame.PROFILES:
                    print(f"settings: no profile {value!r}", flush=True)
                    continue
            elif knob == "weights":
                # **Gone as a knob.** It named a model file (`ht`, `cc_small_fp8`) and resolved it
                # through a manifest; the network is `NR_PRESET` now, one setting, chosen in the
                # setup window and switched live by the panel's Network row.
                #
                # A settings file written before that still carries this key. Skipping it is what
                # keeps such a file starting a daemon: the preset the author chose in the setup
                # window is what runs, and it names the same network. Saying so once is worth it -
                # silence would leave someone editing a knob that does nothing.
                if not getattr(self, "_said_weights_gone", False):
                    self._said_weights_gone = True
                    print(f"settings: 'weights' is no longer a knob; {self.preset} runs "
                          f"(NR_PRESET decides)", flush=True)
                continue
            else:
                try:
                    value = float(value)
                except (TypeError, ValueError):
                    print(f"settings: {knob} is not a number", flush=True)
                    continue
                if not math.isfinite(value):
                    print(f"settings: {knob} must be finite", flush=True)
                    continue
                if knob == "render_scale":
                    if not 0.05 <= value <= 1.0:
                        print("settings: render_scale must be between 0.05 and 1", flush=True)
                        continue
                elif knob in ("temporal", "cut_limit", "hold"):
                    # `temporal` is the vendor's history confidence, which multiplies a
                    # gate that is already in [0, 1]; above 1 it would extrapolate past
                    # the previous frame, which is ringing, not stability.
                    if not 0.0 <= value <= 1.0:
                        print(f"settings: {knob} must be between 0 and 1", flush=True)
                        continue
                elif knob == "release":
                    # levels of 255; 0 is off
                    if not 0.0 <= value <= 255.0:
                        print("settings: release must be between 0 and 255", flush=True)
                        continue
                elif knob == "min_extent":
                    # the graph's own floor is 128; above 4096 nothing is left to pad
                    if not 128.0 <= value <= 4096.0:
                        print("settings: min_extent must be between 128 and 4096", flush=True)
                        continue
                elif not 0.0 <= value <= 2.0:
                    # the vendor's own panel stops at 2 (notes/phase30-control-atlas.md)
                    print(f"settings: {knob} must be between 0 and 2", flush=True)
                    continue
            if getattr(self, knob) != value:
                setattr(self, knob, value)
                changed.append(f"{knob}={value}")
        if changed:
            print(f"settings ({source}): " + ", ".join(changed), flush=True)
        return changed

    def refresh(self):
        """Recompute the knob values from the saved settings, with the menu's preview on top.

        Read as a pair rather than as two independent updates, and that is the whole reason this is
        not two calls to a per-file reader. The saved file is the baseline and the preview is an
        override; the preview is a partial object that exists only while the menu has something
        unsaved. Layering them incrementally would leave a knob stuck at a previewed value the
        moment the preview stopped mentioning it - which is exactly what Reset, or a panel closed
        without saving, produces. Recomputing from the baseline every time makes "no longer in the
        preview" mean "back to what is saved", which is the behaviour the user asked for.
        """
        saved = self._read_json(self.path, "saved", "stamp")
        preview = self._read_json(self.live_path, "preview", "live_stamp")

        # Nothing to do until one of them has actually changed.
        if saved is None and preview is None:
            return

        # A preview whose author is gone, and that no Save confirmed, is not a preview.
        #
        # This is the rule that settles the reported bug: "save, then reset, then restart shows the
        # picture from before the save". Reset writes its defaults to the preview so the change is
        # visible, the game then exits with the panel open - the normal way to leave - and the file
        # survives into the next session, where it overrode the saved settings.
        #
        # Neither timestamp can separate that from a legitimate preview: "Reset ten minutes after
        # Save" and "a real preview ten minutes after Save" are identical in time, and what differs
        # is whether the user ever accepted the values. Nor can it be "the first reading", which an
        # earlier attempt used and which broke the live effect - a daemon starts, the user is
        # already dragging a slider, and their change arrives before the first poll.
        #
        # What separates them is whether the process that wrote the preview is still running. The
        # layer writes its PID for exactly this, and `_owner_alive` reports a missing PID as dead:
        # every current layer writes one, so a preview without it came from a session that has
        # already ended - which was the shape of the file that caused the report.
        #
        # `_confirmed` is the belt to that braces: a preview a Save did write stays valid even
        # after its process ends, because the values in it are the ones in the settings file.
        #
        # There is deliberately NO timestamp comparison between the two files. An earlier version
        # had one - "a preview older than the save it would override is stale" - and it was wrong in
        # both directions. Save writes BOTH files, so their order within one save says nothing; and
        # a preview written before a save is still the user's current intent when they have since
        # moved another knob. The PID answers the only question that matters - is anyone still
        # standing behind this file - and a second, weaker rule on top of it produced false
        # rejections that turned the live effect off.
        if preview is not None and not self._owner_alive(preview) and not self._confirmed(preview):
            preview = None

        # The saved values are KEPT, not just read when they change.
        #
        # This is the part a first version got wrong, and the test caught it: when only the preview
        # changed, the saved file read back as "unchanged" and there was nothing to fall back to -
        # so a knob the preview stopped mentioning kept its previewed value forever. Remembering the
        # last saved object means "no longer previewed" can always mean "back to what is saved",
        # which is what Reset and a panel closed without saving both require.
        if saved is not None:
            self.saved_values = saved

        merged = dict(self.saved_values)
        if preview:
            merged.update(preview)
        self._apply(merged, "preview" if preview else "saved")

        # **The network, which is not a knob and so is not in `KNOBS`.**
        #
        # `_apply` walks that tuple and this key is deliberately outside it - the network is not a
        # picture parameter and a Reset must not move it. Leaving the read to `_apply` meant the
        # merged value was dropped on the floor: the panel switched, the preview file said so, and
        # `refresh_model` kept comparing against a `live.preset` that never changed.
        #
        # **Only from the preview, and only while the game that wrote it runs.** The durable choice
        # is `NR_PRESET`, which the setup window and the panel's Save both write into `nr-env.txt`
        # and the daemon starts on. A `preset` in the saved file - round ten's Save wrote one - is
        # ignored: the setup window never sees that file, so it would win over the window's own
        # choice. A preview left behind by a game that has ended is ignored too, even a confirmed
        # one, because its Save already went into `NR_PRESET` and a later choice must win over it.
        #
        # Anything but A or B leaves the running network alone: a typo in a file must not load a
        # network nobody asked for.
        wanted = preview.get("preset") if preview and self._owner_alive(preview) else None
        if wanted in nr_weight_models.PRESETS and wanted != self.preset:
            self.preset = wanted
            print(f"settings: network preset {wanted} (from the panel)", flush=True)

    @staticmethod
    def _confirmed(preview):
        """Whether a Save confirmed the values this preview shows.

        An object with no `menu_confirmed` is treated as UNconfirmed. That inverts an earlier
        reading of this function, and it is the right way round: a preview written by an older layer
        is a preview from a session that is over, and this daemon's first look at it is exactly the
        case where a leftover Reset must not win. Treating the missing key as confirmed is what let
        the reported bug through.
        """
        return bool(preview.get("menu_confirmed"))

    @staticmethod
    def _owner_alive(preview):
        """Whether the process that wrote this preview is still running.

        A preview is a message from a live menu to a live daemon. When the menu's process ends, the
        values it was showing stop being anyone's intent and the saved settings must take over.

        A preview with NO `menu_pid` is reported as dead, and that is deliberate. Every layer built
        with the current source writes one; a preview without it therefore came from a session that
        has already ended, which is exactly the leftover this function exists to refuse. Reporting
        it alive - the first version's choice, made to stay compatible with an older layer - is what
        let the reported bug through, because the real leftover file had no PID in it either.

        The cost is that an older layer's live previews are ignored. That is the acceptable side of
        the trade: the user's saved settings are honoured, and the mismatched pair is a build that
        should be reinstalled rather than a combination to keep working.
        """
        pid = preview.get("menu_pid")
        if not isinstance(pid, int) or pid <= 0:
            return False
        if os.name == "nt":
            import ctypes
            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            STILL_ACTIVE = 259
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not handle:
                return False          # no such process
            try:
                code = ctypes.c_ulong()
                if kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                    return code.value == STILL_ACTIVE
                return True
            finally:
                kernel32.CloseHandle(handle)
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except (PermissionError, OSError):
            return True

    def _read_json(self, path, source, attr):
        """The object in `path`, or None when there is nothing new to read.

        None means "unchanged or unreadable", which is distinct from an empty dict - the caller
        uses it to tell "no new information" from "this file now says nothing".

        The stamp is compared first because it is one stat call, but a stamp match is NOT the
        answer on its own: this filesystem gives consecutive writes the SAME `st_mtime_ns` when
        they land within one tick, which was measured at about half a millisecond here. A held
        arrow key writes at 20 Hz, so consecutive changes can share a stamp - and treating that as
        "unchanged" drops the second one. The bytes are compared as well, which costs one read on
        the frames where the file did change and nothing at all when it did not.
        """
        if not path:
            return None
        try:
            stamp = os.stat(path).st_mtime_ns
        except OSError:
            # A missing preview is the normal case: it appears when the menu is first moved and is
            # not required to exist. A missing settings file is equally normal on a first run.
            return None

        same_stamp = (stamp == getattr(self, attr))
        setattr(self, attr, stamp)
        try:
            with open(path, "rb") as handle:
                raw = handle.read()
        except OSError:
            return None

        # The bytes decide, not the stamp. See the note above.
        if same_stamp and raw == getattr(self, attr + "_bytes", None):
            return None
        setattr(self, attr + "_bytes", raw)

        try:
            given = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as error:
            print(f"settings ({source}): {error}; keeping the current ones", flush=True)
            return {}
        if not isinstance(given, dict):
            print(f"settings ({source}): expected a JSON object; keeping the current ones",
                  flush=True)
            return {}
        return given


def resample(image, size):
    """Bilinear resize of (H, W, C) to `size`, area-averaging when it divides evenly.

    Pure numpy because this runs per frame: ImageMagick through a subprocess, which is
    what `image_io.load` uses, costs more than the network does at these extents.

    Separable — one pass down the rows, then one across the columns. The obvious
    two-dimensional form, `image[y0][:, x0] * ... + image[y1][:, x1] * ...`, materialises
    four full-size gathers and measured **193 ms** on an 854x480 head against 20 ms for
    this, which made it 55 % of the whole frame. Splitting the axes leaves two gathers
    of the intermediate size instead.

    Downscaling by a whole factor takes the area mean instead of point-sampling, since
    the alternative feeds the network aliasing it would then try to enhance.
    """
    height, width = image.shape[:2]
    new_height, new_width = int(size[0]), int(size[1])
    if (new_height, new_width) == (height, width):
        return image
    if (new_height and new_width and height % new_height == 0 and width % new_width == 0
            and height > new_height and width > new_width):
        # The area mean as NumPy's own `mean((1, 3))` computes it — each block's samples
        # added one at a time in row-major order, then divided by their count — written as
        # whole-image adds. Byte-identical to it (test_daemon.py), and 0.5 ms at 640x360
        # where the multi-axis reduction took 4.8 — 1.9 against 16.5 at 1024x768 — which
        # every scale of exactly 0.5 paid.
        fy, fx = height // new_height, width // new_width
        if nr_image is not None:
            # The same adds in C, one pass instead of five: 1.5 -> 0.2 ms at 640x360.
            native = nr_image.area_mean(image, (fy, fx))
            if native is not None:
                return native
        blocks = np.asarray(image, np.float32).reshape(new_height, fy, new_width, fx, -1)
        total = blocks[:, 0, :, 0].copy()
        for dy in range(fy):
            for dx in range(fx):
                if dy or dx:
                    total += blocks[:, dy, :, dx]
        total /= np.float32(fy * fx)
        return total

    if nr_image is not None:
        # The bilinear branch only: the area mean above is a different filter on purpose,
        # and is what keeps aliasing out of what the network is asked to enhance.
        native = nr_image.bilinear(image, (new_height, new_width))
        if native is not None:
            return native

    def axis(source, count, along):
        """One bilinear pass along `along` (0 rows, 1 columns)."""
        extent = source.shape[along]
        if extent == count:
            return source
        centres = (np.arange(count, dtype=np.float32) + 0.5) * (extent / count) - 0.5
        low = np.clip(np.floor(centres), 0, extent - 1).astype(np.int32)
        high = np.clip(low + 1, 0, extent - 1)
        weight = np.clip(centres - low, 0.0, 1.0).astype(np.float32)
        weight = weight.reshape((-1, 1, 1) if along == 0 else (1, -1, 1))
        return (np.take(source, low, along) * (1.0 - weight)
                + np.take(source, high, along) * weight)

    return axis(axis(np.asarray(image, np.float32), new_height, 0), new_width, 1)


LETTERBOX_TOLERANCE = np.float32(2.0 / 255.0)


def active_region(colour, tolerance=LETTERBOX_TOLERANCE, limit=0.45):
    """The rows and columns a letterboxed game did not draw into.

    Dead or Alive 5's smallest window is 1024x768, but it renders 16:9 inside that and
    leaves two black bars — a quarter of the frame. Every stage downstream is measured at
    the *output* resolution (`notes/phase51`), so those bars cost feature assembly,
    composition, the head upscale and the codec for pixels that carry nothing.

    A bar is a run of rows or columns whose brightest channel is at or below `tolerance`
    everywhere. Scanning stops at `limit` of the extent, so a genuinely dark scene cannot
    eat the frame, and the bars are only trusted when both sides agree to within a row —
    a letterbox is symmetric and a dark sky is not.
    """
    height, width = colour.shape[:2]
    rows = colour.max(axis=(1, 2))
    columns = colour.max(axis=(0, 2))

    def run(values, extent):
        cap = int(extent * limit)
        lead = tail = 0
        while lead < cap and values[lead] <= tolerance:
            lead += 1
        while tail < cap and values[extent - 1 - tail] <= tolerance:
            tail += 1
        # asymmetric bars are not a letterbox; a single row of slack covers odd extents
        return (lead, extent - tail) if lead and tail and abs(lead - tail) <= 1 else (0, extent)

    top, bottom = run(rows, height)
    left, right = run(columns, width)
    # A black frame — a fade, a loading screen — is bars all the way in from both sides
    # and would leave a sliver in the middle. 16:9 inside 4:3 keeps 75 %; anything under
    # half is not a letterbox, it is a dark frame.
    if (bottom - top) * (right - left) < height * width // 2:
        return 0, height, 0, width
    return top, bottom, left, right


class Letterbox:
    """The bars, found once and afterwards only checked.

    `active_region` reduces the whole frame twice, and once everything around it went
    native that made it the most expensive host pass in the frame — 21 ms of a 233 ms
    frame, more than the feature assembly, the composition and both resizes together
    (`notes/phase57`). A letterbox does not move: the game chose it when it opened the
    swapchain.

    What is checked each frame is eight lines. A bar that stopped being black, or a first
    active line that started being black, means the answer has changed and the full scan
    runs again. Either mistake is a slower frame, never a wrong crop.
    """

    def __init__(self):
        self.key = None
        self.bounds = None

    def region(self, colour, key):
        if self.key == key and self.bounds is not None and self._holds(colour):
            return self.bounds
        self.key = key
        self.bounds = active_region(colour)
        return self.bounds

    def _holds(self, colour):
        top, bottom, left, right = self.bounds
        height, width = colour.shape[:2]
        # The key is the caller's word that this is the same swapchain; the bounds fitting
        # is ours. Without this a mismatched key reads other rows and crops to them.
        if bottom > height or right > width:
            return False

        def dark(line):
            return float(line.max()) <= LETTERBOX_TOLERANCE

        for line, wanted_dark in ((colour[top - 1] if top else None, True),
                                  (colour[bottom] if bottom < height else None, True),
                                  (colour[:, left - 1] if left else None, True),
                                  (colour[:, right] if right < width else None, True),
                                  (colour[top], False), (colour[bottom - 1], False),
                                  (colour[:, left], False), (colour[:, right - 1], False)):
            if line is not None and dark(line) != wanted_dark:
                return False
        return True


def solid_regions(held, radius=SOLID_RADIUS, majority=0.75):
    """Keep only the parts of the interface mask that sit inside a solid held-still block.

    The layer marks single pixels, and that is right for an interface: a health bar is a
    slab of pixels that do not move. But a nearly static scene — a round transition, a
    slow replay — makes the *subject* half-hold too, and the mask comes back as a fine
    speckle over the character. Composing that interleaves original and re-rendered
    pixels, and since the pass moves skin by 20-30 levels the two populations differ by
    up to 100, which reads as mottled crust. Measured in `notes/phase43`.

    A majority filter separates them: an interface keeps ~85% of its pixels across a
    whole window and survives, speckle at ~60% or less does not. It is a majority rather
    than an erosion because a real interface does lose scattered pixels — an animated
    shine, bloom from the fighters — and an all-or-nothing test would erase the bar along
    with the noise.
    """
    r = int(radius)
    if r < 1:
        return held
    height, width = held.shape
    padded = np.zeros((height + 2 * r, width + 2 * r), np.int32)
    padded[r:r + height, r:r + width] = held
    integral = np.pad(padded.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    k = 2 * r + 1
    counted = (integral[k:k + height, k:k + width] - integral[0:height, k:k + width]
               - integral[k:k + height, 0:width] + integral[0:height, 0:width])
    return counted >= majority * k * k


class History:
    """The previous frame the game was handed, kept for the network's temporal path.

    The flicker this fixes is not noise in the input: 2.3 % of a live frame is
    byte-identical between presents, and the network still moves those pixels by 3.3
    levels of 255, while pixels that actually moved come back amplified 1.01x. The
    graph is global — five downsamples into a ViT-1D bottleneck whose attention sees
    the whole frame — so a fighter moving in the middle moves the decoder's answer over
    a crowd that did not move at all. Vendor stability comes from the temporal path,
    not from the network (`notes/phase53-why-it-flickers.md`).

    Held at the output extent and resampled down at use, so a render-scale change needs
    no invalidation. Dropped when the geometry moves or the shot cuts, because identity
    reprojection is a claim about a *continuing* shot.
    """

    def __init__(self):
        self.key = None
        self.output = None      # (active_h, active_w, 3), what the game last received
        self.source = None      # (inner_h, inner_w, 3), the game's own last frame
        self.pixels = None      # (active_h, active_w, 3), the game's own last frame, whole
        self.inner = None       # `output` at the network's input extent, taken ahead
        self.frames = 0
        self.cut = 0.0
        self.grids = {}         # each pixel's centre, normalised, per extent

    def take(self, key, source, limit, motion=None, colour=None):
        """History for this frame: at the network's extent, at the output's, and the
        game's own frame behind it. `(None, None, None)` when there is nothing to trust.
        With `motion` (and this frame's `colour`), carried along it: `follow`."""
        previous, self.cut = self.output, 0.0
        if key != self.key or previous is None or self.source.shape != source.shape:
            self.key, self.output, self.source, self.pixels, self.frames = key, None, None, None, 0
            self.inner = None
            return None, None, None
        if motion is not None:
            return self.follow(source, previous, motion, colour, limit)
        self.cut = float(np.abs(source - self.source).mean())
        if self.cut > limit:
            # A round transition, a replay cut or a menu. The gate rejects wrong history
            # per pixel, but it was characterised at full scale on a pan (phase12); a
            # whole-frame replacement is the one case worth refusing outright.
            self.output = self.source = self.pixels = self.inner = None
            self.frames = 0
            return None, None, None
        if self.inner is not None and self.inner.shape[:2] == source.shape[:2]:
            inner = self.inner
        else:
            inner = previous if previous.shape[:2] == source.shape[:2] else resample(previous, source.shape[:2])
        return inner, previous, self.pixels

    def grid(self, height, width):
        grid = self.grids.get((height, width))
        if grid is None:
            yy, xx = np.indices((height, width), dtype=np.float32)
            grid = ((xx + np.float32(0.5)) / np.float32(width),
                    (yy + np.float32(0.5)) / np.float32(height))
            self.grids[(height, width)] = grid
        return grid

    def drop(self):
        self.output = self.source = self.pixels = self.inner = None
        self.frames = 0
        return None, None, None

    def follow(self, source, previous, motion, colour, limit):
        """History carried along motion the client measured, which is what the network's
        temporal path was trained on (`notes/phase12-temporal.md`): for each pixel of this
        frame, the offset in pixels to where its surface was on the frame before, NaN where
        it was not on screen. The identity history stands in for this where there is no
        motion. The hold floor comes along: its "the game handed back the same pixel" is
        asked of the game's last frame moved here, so a surface that kept its look as it
        moved holds its history. Where the frame moved here does not look like this one at
        all — a surface uncovered behind something that moved, which the vectors trace back
        to the thing in front — the history is of something else, and the model is given
        none there rather than trusted to see it. Where nothing was, the frame itself is
        the history, so nothing old is blended in there; with nothing known anywhere, there
        is no history at all."""
        known = np.isfinite(motion[..., 0]) & np.isfinite(motion[..., 1])
        if not known.any():
            return self.drop()
        height, width = previous.shape[:2]
        dx = np.where(known, motion[..., 0], np.float32(0)) / np.float32(width)
        dy = np.where(known, motion[..., 1], np.float32(0)) / np.float32(height)
        u, v = self.grid(height, width)
        full = sample_history(previous, u + dx, v + dy)
        full[~known] = colour[~known]
        pixels = None
        if self.pixels is not None and self.pixels.shape == colour.shape:
            pixels = sample_history(self.pixels, u + dx, v + dy)
            pixels[~known] = colour[~known]
            other = np.abs(colour - pixels).max(axis=-1) > STALE
            full[other] = colour[other]
        # The network's extent reads the motion of the nearest full-size pixel.
        rows, columns = source.shape[:2]
        near = np.ix_(
            np.minimum(((np.arange(rows) + 0.5) * (height / rows)).astype(np.intp), height - 1),
            np.minimum(((np.arange(columns) + 0.5) * (width / columns)).astype(np.intp), width - 1))
        known_inner = known[near]
        u, v = self.grid(rows, columns)
        u, v = u + dx[near], v + dy[near]
        earlier = (self.inner if self.inner is not None and self.inner.shape[:2] == source.shape[:2]
                   else resample(previous, source.shape[:2]))
        inner = sample_history(earlier, u, v)
        inner[~known_inner] = source[~known_inner]
        moved = sample_history(self.source, u, v)
        other = np.abs(source - moved).max(axis=-1) > STALE
        inner[other] = source[other]
        # A camera move is not a cut: the test is against the last frame moved here.
        self.cut = float(np.abs(source - moved)[known_inner].mean()) if known_inner.any() else 0.0
        if self.cut > limit:
            return self.drop()
        return inner, full, pixels

    def keep(self, key, output, source, pixels):
        self.key, self.output, self.source, self.pixels = key, output, source, pixels
        self.frames += 1
        # The next frame's history at the network's extent, resampled now — the daemon
        # calls this after the answer has gone, while the game draws its next frame —
        # instead of on that frame's own path: the same resample of the same array, so the
        # same bytes. A different extent next time falls back to resampling then.
        self.inner = (output if output.shape[:2] == source.shape[:2]
                      else resample(output, source.shape[:2]))


# How far, in any channel, the game's last frame moved here may differ from this one before
# what is there counts as another surface and its history is dropped (`History.follow`).
# Jitter and the reprojection's own filter move textured pixels by a few levels; a surface
# uncovered behind something that moved differs by far more.
STALE = 0.12


def sample_history(history, u, v):
    """The vendor's five-tap Catmull-Rom reprojection at normalised coordinates: in C where
    the library is built, MLX-DLSS's NumPy otherwise, the same bytes either way
    (`src/ref/test_native_image.py`)."""
    sampled = nr_image.history_sample(history, u, v) if nr_image is not None else None
    if sampled is None:
        sampled = nr_frame.load_mlx_numpy_modules(("temporal",))[0].sample_history(history, u, v)
    return sampled


# Over how many levels of 255 the hold lets go. A pixel the game handed back unchanged
# has provably correct history; one level of change is still almost certainly the same
# surface, and by four it is something else and the model's own gate decides alone.
HOLD_RAMP = nr_frame.HOLD_RAMP


# The floor's definition lives in nr_frame now, beside the gate it bounds, so that the
# native composition and the NumPy one read the same thing; kept here by name.
hold_floor = nr_frame.hold_floor


class Meter:
    """How much the network is inventing, live, over pixels the game did not move.

    The same statistic as `src/bench/flicker.py`, but from inside a running game, because
    a replay cannot reproduce one thing that matters: `--dump` drops the daemon to about
    1.28 fps, so consecutive frames in a capture are four times further apart in game time
    than they are in play, and the history is correspondingly less correct. Keeps its own
    two frames rather than the temporal path's, so it reads the same with the path off.
    """

    def __init__(self):
        self.input = None
        self.output = None
        self.seen = 0
        self.still = 0.0
        self.invented = 0.0
        self.moving = 0.0

    def add(self, colour, output):
        if self.input is not None and self.input.shape == colour.shape:
            moved = np.max(np.abs(colour - self.input), axis=2)
            change = np.mean(np.abs(output - self.output), axis=2) * np.float32(255.0)
            still = moved == 0
            if still.any():
                self.seen += 1
                self.still += float(still.mean())
                self.invented += float(change[still].mean())
                self.moving += float(change[~still].mean()) if (~still).any() else 0.0
        self.input, self.output = np.array(colour), np.array(output)

    def report(self):
        if not self.seen:
            return "  meter: no two frames alike yet"
        return (f"  meter: {100 * self.still / self.seen:.0f}% of the frame held still, "
                f"invented {self.invented / self.seen:.2f} levels there, "
                f"{self.moving / self.seen:.2f} where it moved  "
                f"({self.seen} frames)")


DEVICE_LOST = (
    "GPU lost, stopping: the Vulkan device went away under the model ({error}), normally "
    "the driver resetting a hung GPU, and nothing on it can run again in this process. "
    "Turn the effect off and on to start a fresh daemon. If it keeps happening, lower the "
    "render scale and the game's window size, and look for a hang or reset in `sudo dmesg`.")


# Vulkan says what went wrong as a number, and the person playing sees only a frame that did
# not change. These are the two a knob can cause.
FAILURES = {
    -1: ("  — out of host memory. The daemon and the game share one pool on an integrated "
         "GPU: lower the render scale, or the game's resolution."),
    -2: ("  — the buffers for this extent do not fit in the GPU's memory. Lower the render "
         "scale, or the game's resolution; the cost follows the extent."),
}


def explain(error):
    """A sentence for the failures a player can do something about."""
    code = re.search(r"\((-\d+)\)\s*$", str(error))
    return FAILURES.get(int(code.group(1)), "") if code else ""


def refresh_model(args, backend):
    """The model to answer this frame with, swapped when the settings asked for another one.

    A swap is not a restart. `ResidentBackend.close` drops the weights uploaded to the device and
    the next frame uploads the next model's; the GPU image engine caches the backend, so it goes
    first, and the temporal history belongs to the model that made it, so it is dropped too. What
    the layer sees is one present that waits for a model load.

    Nothing here runs on a frame where the setting did not change: the name is remembered as
    `requested`, so a value that cannot be honoured - a model this DLL does not carry, or one the
    loader cannot read - is resolved once and explained once, not looked up once a frame.
    """
    model = getattr(args, "model", None)
    live = getattr(args, "live", None)
    if not isinstance(model, dict) or live is None:
        return backend
    want = nr_weight_models.preset_of(getattr(live, "preset", None))
    if want == model.get("requested"):
        return backend
    model["requested"] = want
    path = nr_weight_models.weights_path(ROOT / "work", want)
    if not path.is_file():
        # **Refused, not answered with the other network.** The old table fell back to A here,
        # which is how `NR_PRESET=B` came to run A silently on an install whose B had never been
        # extracted. The panel asked for B; the honest reply is that B is not here.
        print(nr_weight_models.missing_message(ROOT / "work", want), flush=True)
        return backend
    if want == model.get("preset"):
        return backend
    # The engine follows the model; the transport's imports are the game's and stay.
    close_gpu_images(args, keep_sessions=True)
    args.history = History()
    previous = model.get("backend")
    if previous is not None and previous is not backend:
        try:
            previous.close()
        except Exception as error:  # noqa: BLE001 - a failed close must not cost the swap
            print(f"weights: closing {model.get('name')} failed: {error}", flush=True)
    started = time.perf_counter()
    fresh = nr_frame.ResidentBackend(path)
    model.update(backend=fresh, name=path.name, path=path, preset=want)
    # The transport keeps the game's imports but must follow the model: its engine is rebuilt
    # from `self.backend` on the next frame, so without this it would rebuild on the model it
    # just replaced. `rebind_backend` drops only the engine, never the contexts.
    shared = getattr(args, "gpu_transport", None)
    if shared is not None:
        shared.rebind_backend(fresh)
    print(f"preset: {want} ({path.name}) in {time.perf_counter() - started:.1f}s",
          flush=True)
    return fresh


def serve(server, backend, args):
    """Answer frames until interrupted, or until the GPU is gone.

    A frame that fails costs that frame: the game keeps its own picture and the next one is
    tried. A lost device is not that. Every frame after it fails the same way, at the full
    cost of the copy, and the log fills with one line repeated — so it is said once and the
    daemon exits, which also lets `nr-toggle` and the panel see that there is no model.
    """
    # On Windows a frame's large arrays go back to the system as they are freed, and the next
    # frame pays a page fault for every 4 KB of them again; this keeps them (nr_alloc.py).
    # Here, in the thread that runs the frames, because NumPy's allocator is per context.
    keeping = None
    if os.name == "nt":
        import nr_alloc
        keeping = nr_alloc if nr_alloc.install() else None
    if keeping:
        print("keeping NumPy's large blocks from frame to frame (NR_KEEP_BLOCKS=0 to stop)",
              flush=True)
    while True:
        connection, _ = server.accept()
        try:
            connection.settimeout(args.timeout)
            backend = refresh_model(args, backend)
            process_connection(connection, backend, args)
        except DeviceLost as error:
            print(DEVICE_LOST.format(error=error), flush=True)
            raise SystemExit(1) from error
        except (EOFError, OSError, ValueError, RuntimeError) as error:
            print(f"frame rejected/failed; game keeps original: {error}{explain(error)}",
                  flush=True)
        finally:
            connection.close()
            if keeping:
                keeping.frame_done()


def process_connection(connection, backend, args):
    """One request. Reject invalid extents before allocating/receiving the body.

    Closing a rejected exchange makes the Vulkan layer retain its original frame.
    """
    header = receive(connection, 16, probe_ok=True)
    if header is None:
        return
    magic, width, height, vk_format = struct.unpack("<4I", header)
    if os.environ.get("NR_GPU_ONLY") == "1" and magic != MAGIC_GPU:
        raise ValueError("GPU-only: legacy host frame rejected before pixel payload")
    shared = getattr(args, "gpu_transport", None)
    if shared is not None:
        shared.cleanup_stale()
    if magic == MAGIC_GPU:
        import nr_gpu_transport
        args.live.refresh()
        transport = getattr(args, "gpu_transport", None)
        if transport is None:
            enabled = (os.environ.get("NR_GPU_IMAGE") == "1" and
                       os.environ.get("NR_GPU_TRANSPORT") == "1")
            transport = args.gpu_transport = nr_gpu_transport.GPUSharedTransport(
                backend, enabled=enabled)
        if getattr(args, "image_transport", None) != "gpu-shared":
            transport.reset_history()
        metrics = transport.handle(connection, width, height, vk_format, receive, args.live,
                                   diagnostic=bool(args.dump or args.meter))
        if metrics:
            args.history = History()
            args.image_transport = "gpu-shared"
            mode = "gpu-shared-fence" if metrics.get("sync") == "fence" else "gpu-shared"
            print_gpu_frame(width, height, vk_format, args.live, metrics, mode)
        return
    if magic not in (MAGIC, MAGIC_MASKED, MAGIC_MOTION):
        raise ValueError(f"bad magic {magic:#x}")
    return process(connection, backend, args, width, height, vk_format,
                   masked=magic == MAGIC_MASKED, moving=magic == MAGIC_MOTION)


def process(connection, backend, args, width, height, vk_format, *, masked=False, moving=False):
    """Legacy host image processing, guarded even when invoked directly."""
    if os.environ.get("NR_GPU_ONLY") == "1":
        raise ValueError("GPU-only: host frame processing is forbidden")
    # No pixel ceiling, on purpose. There used to be one (`--max-pixels`, 1 << 22) and a real
    # 3.5K swapchain - 3360x2100 - exceeded it, so every frame of a correctly configured game was
    # refused and the layer kept the original picture: the menu worked, NR silently did nothing,
    # and the log repeated "rejected extent ... limit ..." once a frame. A ceiling on a frame the
    # game actually presents can only be wrong; the guard that matters is that the header has an
    # extent at all, since `width * height * 4` is the read that follows.
    if not width or not height:
        raise ValueError(f"rejected extent {width}x{height}; the header has no extent")
    payload = receive(connection, width * height * 4, keep="frame")
    interface = (receive(connection, width * height, keep="interface")
                 if masked else None)
    motion = (np.frombuffer(receive(connection, width * height * 8, keep="motion"),
                            np.float32).reshape(height, width, 2)
              if moving else None)
    if vk_format not in FORMATS:
        print(f"unsupported VkFormat {vk_format}; passing the frame through",
              flush=True)
        connection.sendall(payload)
        return

    live = args.live
    live.refresh()
    if os.environ.get("NR_GPU_IMAGE") == "1":
        from gpu_image_pipeline import GPUImagePipeline
        engine = getattr(args, "gpu_images", None)
        if engine is None:
            engine = args.gpu_images = GPUImagePipeline(backend)
        reason = engine.supported(width, height, vk_format, live, interface=interface is not None,
                                  diagnostic=bool(args.dump or args.meter))
        if reason is None:
            if getattr(args, "image_transport", None) != "gpu-image-host":
                engine.reset_history()
                args.history = History()
                print("GPU image path ready: transport=gpu-image-host", flush=True)
            args.image_transport = "gpu-image-host"
            started = time.perf_counter()
            answer = engine.process_host(payload, width, height, vk_format, live)
            connection.sendall(answer)
            metrics = {**engine.last, "total_seconds": time.perf_counter() - started}
            print_gpu_frame(width, height, vk_format, live, metrics, "gpu-image-host")
            return
        if getattr(args, "gpu_fallback", None) != reason:
            print("GPU image fallback: " + reason, flush=True)
            args.gpu_fallback = reason
        engine.reset_history()
    if getattr(args, "image_transport", None) not in (None, "cpu"):
        args.history = History()
        args.letterbox = Letterbox()
    args.image_transport = "cpu"
    clock = time.perf_counter()
    whole = decode(payload, width, height, vk_format)
    # A letterboxed game — DoA5's smallest window is 1024x768 with 16:9 inside it — leaves
    # a quarter of the frame black, and every stage below is measured at the output
    # resolution (`notes/phase51`). Working on the active region alone skips that, and the
    # bars are handed back untouched: `encode(decode(v)) == v` for all 256 values, so
    # leaving them in the output array is byte-exact.
    top, bottom, left, right = args.letterbox.region(whole, (width, height, vk_format))
    boxed = (top, bottom, left, right) != (0, height, 0, width)
    colour = whole[top:bottom, left:right] if boxed else whole
    if motion is not None and boxed:
        motion = motion[top:bottom, left:right]
    active_height, active_width = colour.shape[:2]
    # The network's cost follows the extent it is given and nothing else, so a smaller
    # internal frame is the only lever that changes the frame rate (notes/phase37,
    # phase45). What comes back up is the *head* — the detail the network drew — which
    # is then composed against the full-resolution original, so the game's own pixels
    # are never resampled and only the synthesised part is interpolated.
    inner = colour
    if live.render_scale < 1.0:
        # The scale's own frame, or the largest that lands on the same network field — the
        # same cost, more of the picture and less of its mirror image (`render_extent`).
        extent = nr_frame.render_extent(active_width, active_height, float(live.render_scale),
                                        int(live.min_extent))
        if extent != (active_width, active_height):
            inner = resample(colour, (extent[1], extent[0]))
    geometry = nr_frame.network_geometry(inner.shape[1], inner.shape[0],
                                         minimum=int(live.min_extent))
    # `inner` is a view of the decoded frame at scale 1; the history keeps it for the next
    # frame's cut test, and aliasing the decode buffer through it has caused two bugs in
    # this function already.
    shot = (width, height, vk_format, top, bottom, left, right, live.profile)
    history_inner, history_full, history_pixels = args.history.take(
        shot, inner, live.cut_limit if live.temporal > 0 else -1.0, motion=motion, colour=colour)
    # Built in the graph's own mapped input where the backend offers it, so nothing is
    # copied on the way in — on Linux that is measured and clean (test_input_fp16.py).
    #
    # Off on a discrete card under Windows: on the B580 the features built into the mapped
    # half buffer come back through the graph as NaN - every frame, reproducibly, while the
    # same features built on the host and copied in give a correct picture (measured
    # through the deployed layer: 163 clean frames with the host path, an all-black answer
    # through the mapped one, at the same resolution and scale). Discrete-card memory on
    # that driver is not behaving like the integrated one this was written on, and until
    # that is pinned down the host copy is the choice that works there. The Arc 140V under
    # Intel's own driver builds into it cleanly (notes/phase71), and there it also spares a
    # fresh features array a frame, which on Windows is page faults. NR_INPUT_VIEW=1 or 0
    # decides either way.
    discrete = getattr(getattr(getattr(backend, "runtime", None), "lib", None), "xmx_discrete", None)
    default = "0" if os.name == "nt" and discrete is not None and discrete() else "1"
    input_view = (getattr(backend, "input_view", None)
                  if os.environ.get("NR_INPUT_VIEW", default) == "1" else None)
    features = nr_frame.build_features(
        inner, geometry=geometry, history=history_inner,
        out=input_view(geometry.network_height, geometry.network_width) if input_view else None,
        **nr_frame.PROFILES[live.profile])
    head = geometry.crop(backend.run_features(features))
    # The fourth channel is the temporal gate. Without history it reaches nothing —
    # carrying it through the upscale would be a quarter of that pass for nothing
    # (notes/phase48) — and with history it is what decides the blend, per pixel.
    channels = 4 if history_full is not None else 3
    # The style's colour grade, which the vendor runs after the network, and where the
    # prediction the vendor keeps as the next frame's history is written — before the
    # grade, the intensity, the strengths and the interface restore
    # (`notes/phase70-post-process.md`).
    grade = nr_frame.grade_for(**nr_frame.PROFILES[live.profile])
    neural = (np.empty((active_height, active_width, 3), np.float32)
              if live.temporal > 0 else None)
    control = None
    held = None
    if interface is not None:
        # Red scales the blend per pixel, so an interface pixel comes back exactly as
        # the game drew it. The network still runs over the whole frame — masking the
        # composition rather than the input keeps the numerical path untouched.
        held = solid_regions(
            np.frombuffer(interface, np.uint8).reshape(height, width) > 127)
        if boxed:
            held = held[top:bottom, left:right]
        # An interface is a small part of the frame. When the held region is most of it,
        # the scene itself is standing still — a round transition, a replay pause — and
        # what is being protected is the subject, not the HUD. Composing that leaves the
        # character in patches of two different exposures, which is far worse than a
        # softened health bar. Measured in `notes/phase43`: 43% held gave a clean frame,
        # 69% and 75% both blotched. So the mask is dropped rather than trusted.
        if held.mean() > MASK_COVERAGE_LIMIT:
            print(f"  interface mask covers {100 * held.mean():.0f}% of the frame; "
                  f"that is the scene holding still, not a HUD — mask dropped",
                  flush=True)
            held = None
    if held is not None:
        control = np.ones((active_height, active_width, 3), np.float32)
        control[held, 0] = 0.0
    # What the game itself did to each pixel — the only motion signal a present-time
    # layer has, and an exact one.
    # The floor is computed inside the composition now, from the game's previous frame
    # (`nr_frame.compose`, `history_previous`); only its share for the log line is taken
    # here, on every eighth pixel of every eighth row — exactly the values the full floor
    # has there.
    previous = (history_pixels if history_pixels is not None
                and (live.hold > 0 or live.release > 0) else None)
    # The head's upscale, the composition and the encoder as one pass where the knobs
    # leave `compose_detail` a no-op: separately they were three passes over the full
    # frame, ~100 MB of memory traffic at 1280x720 where this moves half, and the bytes
    # are the same (`nr_frame.compose_encode`, `test_native_image.py`). The answer is
    # written into the request's own buffer when nothing needs the request's bytes
    # afterwards — the interface restore does.
    kind = FORMATS[vk_format][0]
    fused = None
    if (kind in ("bgra8", "rgba8") and live.detail_strength == 1 and live.colour_strength == 1
            and head.shape[0] <= active_height and head.shape[1] <= active_width):
        answer = (payload if isinstance(payload, bytearray) and held is None
                  else bytearray(payload))
        fused = nr_frame.compose_encode(
            head[..., :channels], colour,
            np.frombuffer(answer, np.uint8).reshape(height, width, 4),
            top=top, left=left, bgra=kind == "bgra8", intensity=live.intensity,
            control_mask=control, history=history_full, history_confidence=live.temporal,
            history_previous=previous, history_hold=live.hold,
            history_release=live.release, blend_scale=backend.blend_scale, samples=8,
            grade=grade, neural=neural)
    full = None
    if fused is not None:
        composed, samples = fused
        encoded = answer
    else:
        if head.shape[:2] != colour.shape[:2]:
            head = resample(head[..., :channels], colour.shape[:2])
        samples = head[::8, ::8]
        composed = nr_frame.compose(head, colour, intensity=live.intensity,
                                    detail_strength=live.detail_strength,
                                    colour_strength=live.colour_strength,
                                    control_mask=control, history=history_full,
                                    history_confidence=live.temporal,
                                    history_previous=previous, history_hold=live.hold,
                                    history_release=live.release,
                                    blend_scale=backend.blend_scale, grade=grade,
                                    neural=neural)
        if boxed:
            # A copy, not a write into `whole`: aliasing the input and the output through
            # one array has now caused two bugs in this function — `change` printed
            # 0.00000, and a `--dump` saved the composed frame as both the before and the
            # after. One frame copy is a millisecond against the round's 160.
            full = whole.copy()
            full[top:bottom, left:right] = composed
        encoded = encode(full if full is not None else composed, payload, vk_format)
    # Measure the composition itself, not what the write-back makes of it: putting the
    # result into `whole` and then differencing against `whole` compares an array with
    # itself, which reported change 0.00000. On every fourth row, like the log's other
    # figures: the whole frame took 5 ms of a 1080p frame for a number printed to five
    # places, and a quarter of the rows moves it by about a part in five hundred. Taken
    # after the answer has gone where nothing writes into `composed` before then.
    changed = None
    if held is not None:
        if full is None:
            # the restore below writes into the composition itself
            changed = float(np.abs(composed[::4] - colour[::4]).mean())
        # The control mask reaches `compose_head`, but `compose_detail` runs *after* it
        # and re-weights the whole frame: with either strength away from 1 it moved 89.6%
        # of the masked pixels, so the interface protection silently stopped working the
        # moment a knob was touched. Restoring the original wire bytes here is exact by
        # construction — it survives every later stage and does not depend on the codec
        # round-tripping. Taken from the parallel ProjectsCodex tree, which had it.
        in_place = isinstance(encoded, bytearray)
        protected = np.frombuffer(encoded, np.uint8)
        protected = (protected if in_place else protected.copy()).reshape(height, width, 4)
        original = np.frombuffer(payload, np.uint8).reshape(height, width, 4)
        if boxed:
            protected[top:bottom, left:right][held] = original[top:bottom, left:right][held]
        else:
            protected[held] = original[held]
        if not in_place:
            encoded = protected.tobytes()
        # so a --dump shows what was actually sent
        (full[top:bottom, left:right] if full is not None else composed)[held] = colour[held]
    connection.sendall(encoded)
    if changed is None:
        changed = float(np.abs(composed[::4] - colour[::4]).mean())
    # What the game was handed at the active region, for the dump below.
    active = full[top:bottom, left:right] if full is not None else composed
    if live.temporal > 0:
        # What is carried forward is the vendor's history, the prediction before anything
        # after the network — not what the game received, which the intensity, the grade,
        # the strengths and the interface restore have all moved. Kept as they are, not
        # copied: each is this frame's own — decode, the resample and the composition all
        # hand back fresh arrays, nothing writes them from here on, and History only reads
        # what it holds. Three frame copies a frame were 0.4 ms at 640x360 and 2-3 at
        # 1280x720.
        args.history.keep(shot, neural, inner, colour)
    if args.dump:
        import json as _json

        import image_io
        try:
            destination = pathlib.Path(args.dump)
            destination.mkdir(parents=True, exist_ok=True)
            # Numbered, so walking through a game and pressing the trigger repeatedly keeps
            # every shot instead of overwriting the last one, and so a restarted daemon
            # continues where the last one stopped. The number is the frame's position in the
            # dump, not a file counter -- which is what makes --dump-every legible: N=10 writes
            # 010, 020, 030, and the gaps are the frames it skipped.
            step = max(1, int(getattr(args, "dump_every", 1) or 1))
            if getattr(args, "dump_seq", None) is None:
                args.dump_seq = max((int(path.stem.split("_")[0])
                                     for path in destination.glob("*_in.*")
                                     if path.stem.split("_")[0].isdigit()), default=0)
                args.dump_written = 0
                args.dump_skipped = 0
            args.dump_seq += 1
            index = args.dump_seq
            wanted = (index % step == 0)
            if not wanted:
                args.dump_skipped += 1
            # Raw, not PNG: this block is inside the timed region (clock at :626, the report at
            # :851) and `image_io.save` spawns ImageMagick, 147 ms an image at 1280x720. Two of
            # those made every `in X s` and every fps taken with --dump on meaningless. See
            # `image_io.save_raw`; convert with `src/tools/dump_to_png.py`, outside the timing.
            #
            # NR_DUMP_SLOW=1 restores the old call, and exists only so the two can be measured
            # against each other in one session (the contributor's own notes on the dump fix and
            # the rebuilt baseline, which are not in this repository).
            # It is a debugging switch, not a supported mode.
            slow = os.environ.get("NR_DUMP_SLOW") == "1"
            write = image_io.save if slow else image_io.save_raw
            suffix = "png" if slow else "raw"
            if wanted:
                shown = full
                if shown is None and boxed:
                    shown = whole.copy()
                    shown[top:bottom, left:right] = active
                options = {} if slow else {"metadata": True}
                write(whole, destination / f"{index:03d}_in.{suffix}", **options)
                write(shown if shown is not None else active,
                      destination / f"{index:03d}_out.{suffix}", **options)
                args.dump_written += 1
            if not slow and wanted:
                # This is a session summary. Each raw has its own geometry sidecar: a window
                # resize must not reinterpret all earlier frames using these latest sizes.
                (destination / "dump.json").write_text(
                    _json.dumps({"width": width, "height": height, "channels": 3,
                                 "dtype": "uint8", "layout": "hwc",
                                 "per_frame_geometry": True,
                                 "sample_step": step,
                                 "frames_written": args.dump_written,
                                 "frames_skipped": args.dump_skipped,
                                 "last_frame_seq": index}) + "\n",
                    encoding="utf-8")
            if wanted:
                print(f"  -> {destination}/{index:03d}_{{in,out}}.{suffix}"
                      + ("" if slow else "  (convert with tools/dump_to_png.py)"), flush=True)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            print(f"frame returned, but dump failed: {error}", flush=True)
    if args.meter is not None:
        args.meter.add(colour, active)
    note = "" if held is None else f"  interface {100 * held.mean():.0f}% left alone"
    if live.temporal > 0:
        if history_full is None:
            note += f"  no history, cut {args.history.cut:.4f}"
        else:
            gate = float(nr_frame.history_weight(samples, blend_scale=backend.blend_scale).mean())
            hold = "" if previous is None else (
                f", held {100 * float((hold_floor(colour[::8, ::8], previous[::8, ::8], live.hold) > 0).mean()):.0f}%")
            if previous is not None and live.release > 0:
                kept = nr_frame.release_factor(colour[::8, ::8], previous[::8, ::8],
                                               nr_frame.release_slope(live.release))
                hold += f", released {100 * float((kept == 0).mean()):.0f}%"
            note += f"  gate {gate:.3f}{hold}, cut {args.history.cut:.4f}"
    box = "" if not boxed else (f"  letterbox {height - (bottom - top)}px of rows and "
                               f"{width - (right - left)}px of columns skipped")
    # in+run+out: the two host transfers around the graph. They are a memcpy where the GPU
    # shares this memory and a trip across PCIe where it does not, and a single frame time
    # cannot tell those apart — which is exactly the question on a discrete card.
    carried, ran, read = getattr(backend, "split", (0.0, 0.0, 0.0))
    split = f"  gpu {1000 * carried:.0f}+{1000 * ran:.0f}+{1000 * read:.0f}ms" if ran else ""
    print(f"{width}x{height} {FORMATS[vk_format][1]} in "
          f"{time.perf_counter() - clock:.2f}s  "
          f"change {changed:.5f}{note}{split}{box}"
          f"  network {geometry.network_width}x{geometry.network_height}"
          f" scale {live.render_scale:g}"
          + (f" (runs as {inner.shape[1] / active_width:.3g})"
             if inner.shape[1] != round(active_width * live.render_scale)
             and live.render_scale < 1.0 else ""), flush=True)
    if args.meter is not None:
        print(args.meter.report(), flush=True)


def check_half_rounding(tolerance=0):
    """Fail loudly if the driver stopped rounding float32 to half where the graph needs it.

    `float(float16_t(x))` is correct only while the compiler keeps the conversion. Mesa
    folds it away - 360 428 of 360 704 values came back unrounded when this was written
    (src/bench/half_probe.py) - and every vendor rounding point in publish.glsl then moves,
    silently, per frame. The graph has been through that once; a driver update must not be
    able to put it back without anyone noticing.

    Three spellings are compared against numpy's float16 on values that cover ordinary
    numbers, half subnormals, overflow and the edges, and the one libxmx compiles into
    `half_round` for this driver (`xmx_half_by_cast`, `XMX_HALF_ROUND` to override) has to
    match exactly. `NR_SKIP_HALF_CHECK=1` skips it for a machine where the probe
    itself misbehaves, and says so in the log.

    Runs on whatever XMX_UNARY_SPV names, the same entry point the graph's own unary
    passes use, so it exercises the real path rather than a special case.
    """
    if os.environ.get("NR_SKIP_HALF_CHECK") == "1":
        print("half rounding: NOT CHECKED (NR_SKIP_HALF_CHECK=1)", flush=True)
        return
    spv = str(ROOT / "work" / "half_probe.spv")
    if not pathlib.Path(spv).exists():
        # Not fatal: a deployment that left the probe out still runs the graph.
        print(f"half rounding: NOT CHECKED (no {spv})", flush=True)
        return
    # In a child process, deliberately. libxmx.c keeps its Vulkan state in process
    # globals, so a probe that builds a runtime inside this one resets state the graph is
    # about to build on - on the B580 every frame then comes back NaN (measured here:
    # all-black answers through the daemon, clean with NR_SKIP_HALF_CHECK=1, which is how
    # this was found). A child keeps the probe's Vulkan lifetime out of the graph's.
    # Its stdin is NUL: a daemon the layer spawned has no valid one, and asking Windows for
    # the parent's then fails with WinError 6 before the probe starts.
    r = subprocess.run(
        [sys.executable, str(pathlib.Path(__file__).resolve()), "--half-probe"],
        env={**os.environ, "NR_ROOT": str(ROOT),
             "PYTHONPATH": os.pathsep.join([str(ROOT / "src" / "gpu"),
                                            str(ROOT / "src" / "ref"),
                                            str(ROOT / "src" / "layer")])},
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=180,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    for line in (r.stdout or "").splitlines():
        print(line, flush=True)
    if r.returncode != 0:
        raise SystemExit((r.stderr or "the half-rounding probe failed").strip())


def half_probe_child():
    """The probe as its own process: compare, print, exit 0 or 1."""
    spv = str(ROOT / "work" / "half_probe.spv")
    if not pathlib.Path(spv).exists():
        print(f"half rounding: NOT CHECKED (no {spv})", flush=True)
        return 0
    import xmxres
    os.environ["XMX_UNARY_SPV"] = spv
    probe = xmxres.Runtime()
    rng = np.random.default_rng(3)
    values = np.concatenate([
        rng.standard_normal(1 << 16).astype(np.float32) * 4.0,      # ordinary
        rng.standard_normal(1 << 14).astype(np.float32) * 1e-5,     # half subnormals
        rng.standard_normal(1 << 12).astype(np.float32) * 1e-7,     # far below
        rng.standard_normal(1 << 12).astype(np.float32) * 1e5,      # overflow range
        np.float32([0.0, -0.0, 65504.0, 65520.0, 65519.0, 6.09e-05, 5.96e-08, 2.98e-08]),
    ]).astype(np.float32)
    n = (values.size + 255) // 256 * 256
    padded = np.zeros(n, np.float32)
    padded[:values.size] = values
    src = probe.buffer_from(padded)
    outs = [probe.buffer(n) for _ in range(3)]
    probe.begin()
    probe.unary(0, src, outs[1], n, second=outs[0], third=outs[2])
    probe.submit()
    with np.errstate(over="ignore"):
        want = padded.astype(np.float16).astype(np.float32)
    mismatches = {}
    for name, buf in zip(("bit-twiddled", "packHalf2x16", "float16_t"), outs):
        # download, not view: on a card whose memory the host cannot map (XMX_STAGING=1
        # here) a graph buffer has no view, and the daemon died at its first start
        got = buf.download(np.float32, n)
        bad = ~((got == want) | (np.isnan(got) & np.isnan(want)))
        mismatches[name] = int(bad.sum())
    print("half rounding: " + ", ".join(f"{k} {v}/{n}" for k, v in mismatches.items()),
          flush=True)
    # What publish.glsl's half_round runs on this driver: libxmx chose it when it built
    # the runtime above, from the driver (xmx_init).
    used = "float16_t" if probe.lib.xmx_half_by_cast() else "packHalf2x16"
    if mismatches[used] > 0:
        print(f"FAIL: {used} disagrees with float16 on {mismatches[used]} of {n} values; "
              "every rounding point in publish.glsl would move, per frame, silently.",
              flush=True)
        return 1
    if mismatches["packHalf2x16"] > 0 and used != "packHalf2x16":
        # The round trip is folded on this driver, and half_round uses the cast here. The
        # attention shaders' softmax uses packHalf2x16 only as a bit trick on the packed
        # word, which the fold does not touch (publish.glsl).
        print(f"  note: packHalf2x16's round trip is folded on this driver "
              f"({mismatches['packHalf2x16']}/{n} values); half_round uses {used} here",
              flush=True)
    return 0


def end_with_game():
    """End this daemon when the game that started it exits.

    The layer starts the daemon from inside the game and puts the game's process id in
    NR_LAYER_SPAWNED. Steam counts every process a game starts as the game, so a daemon that
    outlived it kept the game running for Steam, and the model in memory until something
    killed the daemon: on Windows Restore Steam refused, and on Linux Steam's reaper, which
    waits for every descendant, waited for the daemon. A daemon started any other way has no
    id there and runs until it is stopped, as before; so does one marked 1, as Linux's layer
    marked it until 2026-10-14.
    """
    try:
        pid = int(os.environ.get("NR_LAYER_SPAWNED", ""))
    except ValueError:
        return None
    if pid <= 1:
        return None
    if os.name != "nt":
        return _end_with_game_posix(pid)
    import ctypes
    import threading
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    synchronize, infinite, no_such_process = 0x00100000, 0xFFFFFFFF, 87
    # The handle is held from here on, so the id cannot be reused under it.
    handle = kernel32.OpenProcess(synchronize, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == no_such_process:
            print(f"the game that started this daemon (process {pid}) has already exited; "
                  "stopping", flush=True)
            os._exit(0)
        print(f"cannot watch the game that started this daemon (process {pid}, WinError "
              f"{error}); it runs until it is stopped", flush=True)
        return None

    def wait():
        kernel32.WaitForSingleObject(handle, infinite)
        print(f"the game that started this daemon (process {pid}) has exited; stopping",
              flush=True)
        os._exit(0)

    print(f"ends with the game that started it (process {pid})", flush=True)
    thread = threading.Thread(target=wait, name="end-with-game", daemon=True)
    thread.start()
    return thread


def print_gpu_frame(width, height, vk_format, live, metrics, transport):
    nw, nh = metrics["network"]
    print(f"{width}x{height} {FORMATS[vk_format][1]} in {metrics['total_seconds']:.3f}s  "
          f"change {metrics['change']:.5f}  cut {metrics['cut']:.4f}  "
          f"gpu 0+{metrics['network_ms']:.0f}+0ms  network {nw}x{nh} scale {live.render_scale:g} "
          f"transport={transport} image prep={metrics['preprocess_ms']:.3f}ms "
          f"compose={metrics['compose_ms']:.3f}ms", flush=True)


def _end_with_game_posix(pid):
    """`end_with_game` on Linux: a pidfd, which is readable once the process has exited.

    The descriptor is held from here on, so the id cannot be reused under it, as the handle
    is on Windows."""
    import select
    import threading
    try:
        watched = os.pidfd_open(pid)
    except ProcessLookupError:
        print(f"the game that started this daemon (process {pid}) has already exited; "
              "stopping", flush=True)
        os._exit(0)
    except (AttributeError, OSError) as error:
        print(f"cannot watch the game that started this daemon (process {pid}, {error}); "
              "it runs until it is stopped", flush=True)
        return None

    def wait():
        poll = select.poll()
        poll.register(watched, select.POLLIN)
        while not poll.poll():
            pass
        print(f"the game that started this daemon (process {pid}) has exited; stopping",
              flush=True)
        os._exit(0)

    print(f"ends with the game that started it (process {pid})", flush=True)
    thread = threading.Thread(target=wait, name="end-with-game", daemon=True)
    thread.start()
    return thread


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--half-probe":
        sys.exit(half_probe_child())

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", default="/tmp/nr_layer.sock")
    parser.add_argument("--root", default=None,
                        help="deployment root: work/ and src/ are looked for under it "
                             "(also read from NR_ROOT before this parser runs)")
    parser.add_argument("--profile", default="standard", choices=sorted(nr_frame.PROFILES))
    # Which weight model to run, by name. The choices are the table's, not a second list: a model
    # added to `nr_weight_models` is nameable here the same day, and one this build cannot load is
    # still allowed as a value because the settings file may name it and `refresh_model` falls back.
    #
    # Default None, not "ht": the installer's network choice (`NR_PRESET`, written beside the
    # game) names a preset, and this knob names the model file that preset's weights are. The
    # mapping is one line below the parse - preset B's weights and the panel's `cc_small_fp8` are
    # the same file - so an install chosen as B starts on B without the two selection paths
    # disagreeing. An explicit --weights, and a settings file's `weights` knob, still win.
    parser.add_argument("--preset", default=None, choices=nr_weight_models.PRESETS,
                        help="which network to run; NR_PRESET also sets this")
    parser.add_argument("--intensity", type=float, default=1.0)
    parser.add_argument("--detail-strength", type=float, default=1.0)
    parser.add_argument("--colour-strength", type=float, default=1.0)
    parser.add_argument("--settings", default=None,
                        help="a JSON file of knobs, re-read whenever it changes; "
                             "`nr-ctl` writes it")
    parser.add_argument("--render-scale", type=float, default=1.0,
                        help="run the network on this fraction of each side; the head is "
                             "scaled back and composed against the full-size frame")
    parser.add_argument("--temporal", type=float, default=1.0,
                        help="how much of the model's own history gate to trust, 0-1; "
                             "0 turns the temporal path off entirely")
    parser.add_argument("--hold", type=float, default=1.0,
                        help="how hard to hold pixels the game handed back unchanged, "
                             "0-1; this is the floor under the model's own gate")
    parser.add_argument("--release", type=float, default=24.0,
                        help="levels of 255 of change in the game's own pixel by which none "
                             "of the model's history gate survives there, 0-255; 0 is off")
    parser.add_argument("--min-extent", type=float, default=320.0,
                        help="the smallest side the network's frame is padded to, 128-4096; "
                             "320 is what NVIDIA's own driver does, the graph runs down to 128")
    parser.add_argument("--cut-limit", type=float, default=0.15,
                        help="mean absolute frame-to-frame change above which the shot is "
                             "taken to have cut and the history is dropped")
    parser.add_argument("--dump", help="write frames in and out as raw, for a look; "
                                       "convert with src/tools/dump_to_png.py")
    parser.add_argument("--dump-every", type=int, default=1, metavar="N",
                        help="write only every Nth processed frame (default 1, every frame). "
                             "Raw does not compress: 2.64 MiB an image at 1280x720, 5.27 MiB a "
                             "frame, so a few minutes of play is gigabytes. The frames written "
                             "keep their own sequence numbers, so N=10 writes 010, 020, 030 and "
                             "the gaps are visible in the names.")
    parser.add_argument("--meter", action="store_true",
                        help="report, every frame, how much the network invents over "
                             "pixels the game did not move; costs about 5 ms")
    parser.add_argument("--timeout", type=float, default=60,
                        help="socket inactivity timeout in seconds")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not 0.05 <= args.render_scale <= 1.0:
        parser.error("--render-scale must be between 0.05 and 1")
    for knob in ("temporal", "cut_limit", "hold"):
        if not 0.0 <= getattr(args, knob) <= 1.0:
            parser.error(f"--{knob.replace('_', '-')} must be between 0 and 1")
    if not 0.0 <= args.release <= 255.0:
        parser.error("--release must be between 0 and 255")
    if not 128.0 <= args.min_extent <= 4096.0:
        parser.error("--min-extent must be between 128 and 4096")
    from nr_daemon_owner import EndpointOwner
    owner = EndpointOwner(args.socket)
    if not owner.acquire():
        print(f"no daemon started: an owner already reserves {args.socket}", flush=True)
        return 0
    print("daemon session " + json.dumps(owner.metadata()), flush=True)
    try:
        return run_owned(args)
    finally:
        owner.release()


def run_owned(args):
    # Before the model loads: a game that is closed during the load takes the daemon with it.
    end_with_game()
    if getattr(args, "preset", None) is None:
        # No preset on the command line, so `NR_PRESET`: the proxy sets it in the game's process from
        # `nr-env.txt`, and the daemon inherits it. That file is the one place the network is kept -
        # the setup window writes it, and so does the panel's Save - so the daemon comes up on the
        # network the user last chose, with one model load. Anything unrecognised is A, the shipped
        # default.
        #
        # **Not the settings file.** Round ten read a `preset` key there first, which the panel's
        # Save wrote and the setup window never touched: after a panel Save of B, the window's A
        # changed nothing. A file from that round still carries the key, and it is ignored.
        args.preset = nr_weight_models.preset_of(os.environ.get("NR_PRESET"))
    args.live = Settings(args)
    args.history = History()
    args.letterbox = Letterbox()
    args.meter = Meter() if args.meter else None

    preset = nr_weight_models.preset_of(getattr(args.live, "preset", None))
    weights_path = nr_weight_models.weights_path(ROOT / "work", preset)
    name = weights_path.name
    if weights_path is None or not weights_path.exists():
        # The first thing a new clone hits, and a traceback is a poor way to say it.
        how = ("as the README's Build section describes" if nr_frame.PRESET == "A" else
               "with src/tools/unpack_preset.py, from a 310.9.16 or later DLL")
        raise SystemExit(
            f"no weights at {nr_frame.WEIGHTS}.\n"
            "They are NVIDIA's and are not distributed here: extract them from your own "
            f"copy of nvngx_dlssnr.dll {how}.")
    started = time.perf_counter()
    # Check the rounding the graph depends on before any frame is processed. Runs first,
    # while XMX_UNARY_SPV can still select the probe shader - the graph's own runtime is
    # built below and picks its shader then.
    check_half_rounding()
    backend = nr_frame.ResidentBackend(weights_path)
    # The model this daemon is running, and the one the settings asked for, so a change can be
    # told apart from a repeat. `refresh_model` swaps it; the panel's knob is the only writer.
    args.model = {"backend": backend, "name": name, "path": weights_path,
                  "requested": name}
    # Which presets this install could run, so "B is not extracted here" is visible without
    # trying B first. One line, and only when the answer is not "A is all there is".
    _run = nr_weight_models.describe(ROOT / "work")
    if len(_run) > 1:
        print(f"weights: {weights_path.name}, installed presets: "
              + ", ".join(f"{k}={'yes' if v else 'no'}" for k, v in _run.items()), flush=True)
    # which GPU: a machine can have more than one, or a different one than the person
    # assumes. libxmx chooses one that can run the network (XMX_DEVICE names another).
    print(f"model ready in {time.perf_counter() - started:.1f}s"
          f" on {xmx.device_name()}", flush=True)
    # **Neither half can disagree with the other any more.** This used to print
    # `nr_frame.PRESET` - a module constant - beside the model the table had chosen, so an install
    # running B on a settings file's `weights` printed "network preset A". There is one setting
    # now: `preset` is what was asked for, `weights_path` is the file that answers it, and a
    # mismatch is not expressible.
    print(f"network preset {preset} ({weights_path.name}),"
          f" {backend.topology.block_count} blocks, history ceiling {backend.blend_scale}",
          flush=True)
    if nr_image is None or nr_image.library() is None:
        # Silent otherwise, and slow: the history a client's motion carries is resampled in
        # MLX-DLSS's NumPy then, 1.1 s a frame at 835x522 against 51 ms in C (2026-10-18).
        print("host passes in NumPy: work/libnr_image did not load (a MinGW build needs"
              " libgomp-1.dll, found through NR_DLL_PATH)", flush=True)
    print(f"buffers in {xmx.memory_note()}", flush=True)
    if xmx.low_latency():
        # without it the GPU's governor clocks down between frames (libxmx, xmx_init)
        print("GPU clock: the driver's low-latency strategy (XMX_LOW_LATENCY=0 leaves its default)",
              flush=True)
    rt = backend.runtime
    print("runtime options " + " ".join(
        f"{name}={int(getattr(rt, attr))}" for name, attr in (
            ("NR_BATCH_FFN", "batch_ffn"), ("NR_FUSE_QK", "fuse_qk"),
            ("NR_JOINT_QKV", "joint_qkv"), ("NR_INPUT_FP16", "input_fp16"),
            ("NR_COMPACT_HEAD", "compact_head"), ("NR_FUSE_RESIDUAL", "fuse_residual"),
            ("NR_FUSE_WINDOW_RESIDUAL", "fuse_window_residual"),
            ("NR_FUSE_WINDOW_ATTENTION", "fuse_window_attention"),
            ("NR_FUSE_ATTENTION_MERGE", "fuse_attention_merge"),
            ("NR_QKV_EPILOGUE", "qkv_epilogue"), ("NR_FUSE_GLUE", "fuse_glue"),
            ("NR_FUSE_FFN", "fuse_ffn"), ("NR_FUSE_BRANCHED_FFN", "fuse_branched_ffn"),
            ("NR_FUSE_PARTITION", "fuse_partition"),
            ("NR_FUSE_TRANSITION", "fuse_transition"),
            ("NR_FUSE_MERGE_FFN", "fuse_merge_ffn"), ("NR_FUSE_STEM_FFN", "fuse_stem_ffn"),
            ("NR_FUSE_POOL", "fuse_pool"), ("NR_FUSE_WINDOW_BLOCK", "fuse_window_block"),
            ("NR_FUSE_HEAD", "fuse_head"),
            ("NR_FUSE_GLOBAL_ATTENTION", "fuse_global_attention"))),
          flush=True)

    if os.name == "nt":
        # Windows has no AF_UNIX in CPython, so the layer and the daemon meet on a
        # named pipe instead. nr_pipe gives the same accept/recv/sendall shape a
        # socket has, so serve() below does not care which one it got.
        import nr_pipe
        server = nr_pipe.NamedPipeServer(args.socket, backlog=4, timeout=None)
        print(f"listening on {args.socket}", flush=True)
        try:
            serve(server, backend, args)
        except KeyboardInterrupt:
            pass
        finally:
            server.close()
            close_gpu_images(args)
            backend.close()
        return

    if os.path.lexists(args.socket):
        if not stat.S_ISSOCK(os.lstat(args.socket).st_mode):
            backend.close()
            raise SystemExit(f"socket path is occupied by a non-socket: {args.socket}")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            try:
                probe.connect(args.socket)
            except ConnectionRefusedError:
                os.unlink(args.socket)
            else:
                backend.close()
                raise SystemExit(f"a daemon is already listening at {args.socket}")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(args.socket)
    server.listen(4)
    print(f"listening on {args.socket}", flush=True)

    try:
        serve(server, backend, args)
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
        close_gpu_images(args)
        backend.close()
        if os.path.exists(args.socket):
            os.unlink(args.socket)


def close_gpu_images(args, *, keep_sessions=False):
    """Give back the GPU image engine and, when the daemon is going, its transport.

    A model swap used to close both, and that is what made the panel's network chooser able to
    stop NR dead. The image engine (`gpu_images`) wraps a `ResidentBackend`, so a new model means
    a new engine. But the transport's `contexts` are the game process's OWN imports - buffers,
    semaphores and an authenticated process lifetime, all established by a HELLO the layer sent
    and all of which the layer is still using. Closing them silently leaves the layer holding
    handles to objects this daemon has already given back, and its next frame is answered
    `GPU frame has no matching imported session`. That failure is unrecoverable by design
    (`gpu_blocked` is set and, unless the reply said "unsupported", never cleared), so NR stops
    processing for the rest of the session - and the panel's row is the one thing that looks
    like it changed something.

    `keep_sessions=True` is a model swap: the engine is rebuilt lazily on the next frame
    (`handle` does `if self.images is None`), so dropping only `gpu_images` costs nothing and
    leaves the session standing. Without it the transport goes too, which is right at shutdown -
    nobody is going to send another frame - and wrong anywhere the daemon keeps serving. """
    for name in ("gpu_images", "gpu_transport"):
        resource = getattr(args, name, None)
        if resource is None:
            continue
        if keep_sessions and name == "gpu_transport":
            # The imports belong to the game, not to the model: drop only the engine they feed.
            engine = getattr(resource, "images", None)
            if engine is not None:
                engine.close()
                resource.images = None
            resource.active = None
            continue
        resource.close()
        setattr(args, name, None)


if __name__ == "__main__":
    main()
