"""Which weight models an install has, and which one a setting selects.

One place for the model table, the manifest format and the file layout, because four callers need
the same answers and disagreeing copies of this would be the whole feature failing quietly:

  scripts/get_weights.py   installs: detect in the DLL, extract, write the manifest
  scripts/nr_weights.py    the CLI over the same
  src/layer/nr_daemon.py   picks the file to load, and the panel's `weights` knob is validated here
  src/layer/nr_menu_bridge.c  offers the same names in the panel (as text; this is the authority)

Nothing here imports numpy or Vulkan: the installer reaches it before any runtime is ready, and the
daemon reaches it on every settings change.

A model is one weight resource inside a user-supplied `nvngx_dlssnr.dll`:

  name          what the settings file and the panel call it
  resource      the PE resource it comes from
  logical       the file under `work/mlxw/` this tree loads
  supported     whether the loader can read it - not a preference, see the block contracts

Compatibility is the point of the fallbacks: an install with no manifest (every install before this
file existed) resolves to the single `dlssnr-logical.safetensors` the project has always used, and a
settings file naming a model that is not here falls back to the one that is.
"""
from __future__ import annotations

import json
import pathlib

MANIFEST_FORMAT = "nr-weights-manifest-v1"
MANIFEST_NAME = "weights.json"
LEGACY_LOGICAL = "dlssnr-logical.safetensors"


class Model:
    """One weight resource: its resource name, the file it becomes, and whether we can load it."""

    def __init__(self, name, resource, logical, *, supported, expected_tensors=0, note="",
                 packed=None):
        self.name = name
        self.resource = resource
        self.logical = logical
        self.supported = supported
        self.expected_tensors = expected_tensors
        self.note = note
        # The intermediate file. HT keeps the name it has always had, so an install that only ever
        # had `work/mlxw/dlssnr-packed.safetensors` keeps working, `--skip-extract` included.
        self.packed = packed or f"{name}-packed.safetensors"

    def as_dict(self):
        return {"name": self.name, "resource": self.resource, "logical": self.logical,
                "packed": self.packed, "supported": self.supported,
                "expected_tensors": self.expected_tensors, "note": self.note}

    def __repr__(self):
        return f"Model({self.name!r}, {self.resource!r}, supported={self.supported})"


MODELS = (
    # The model this project is built around, and the only file name the rest of the tree knows
    # (`nr_frame.WEIGHTS`). It keeps that name so an install with one model is unchanged.
    Model("ht", "WEIGHTS_HT", LEGACY_LOGICAL, supported=True, expected_tensors=649,
          packed="dlssnr-packed.safetensors"),
    # 310.9.16's second model: 77 packed tensors, 30 blocks, FP8. Extractable, not loadable: the
    # unpacker's block contracts are HT's 71 blocks at 32/64/128/256 wide.
    Model("cc_small_fp8", "WEIGHTS_CC_SMALL_FP8", "dlssnr-cc-small-fp8-logical.safetensors",
          supported=False, expected_tensors=77, note="different block table; not loadable yet"),
)
BY_NAME = {model.name: model for model in MODELS}
BY_RESOURCE = {model.resource: model for model in MODELS}


def default_name():
    return next((model.name for model in MODELS if model.supported), MODELS[0].name)


def names():
    """Every model name this build knows, supported or not - what a combo can offer."""
    return [model.name for model in MODELS]


def manifest_path(work_dir):
    return pathlib.Path(work_dir) / "mlxw" / MANIFEST_NAME


def read_manifest(work_dir):
    path = manifest_path(work_dir)
    if not path.is_file():
        return None
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if manifest.get("format") != MANIFEST_FORMAT:
        return None
    return manifest


def available(work_dir):
    """[{name, logical, supported, present, note}] - what this install can run, best first.

    The manifest is the authority when there is one. Without it - an install that predates this
    file, which is most of them - the answer is the single HT file the project has always used, so
    nothing that works today starts reporting itself as missing.
    """
    work = pathlib.Path(work_dir)
    manifest = read_manifest(work)
    models = []
    if manifest:
        for entry in manifest.get("models", []):
            name = entry.get("name")
            if not name:
                continue
            logical = entry.get("logical")
            models.append({"name": name, "logical": logical,
                           "supported": bool(entry.get("supported")),
                           "note": entry.get("note", ""),
                           "present": bool(logical) and (work / "mlxw" / logical).is_file()})
    if not models:
        legacy = work / "mlxw" / LEGACY_LOGICAL
        models = [{"name": "ht", "logical": LEGACY_LOGICAL, "supported": True,
                   "note": "", "present": legacy.is_file()}]
    return models


def resolve(work_dir, name):
    """(name, path) of the model a caller should load, or (None, None).

    `name` is what the settings file says. An unknown, unsupported or absent one falls back to the
    default supported model: a settings file written where two models exist has to load where one
    does, rather than leave the game with no model at all.
    """
    work = pathlib.Path(work_dir)
    models = {entry["name"]: entry for entry in available(work)}
    chosen = models.get(name) if name else None
    if chosen is None or not chosen.get("supported") or not chosen.get("present"):
        chosen = next((entry for entry in models.values()
                       if entry.get("supported") and entry.get("present")), None)
    if chosen is None:
        return None, None
    return chosen["name"], work / "mlxw" / chosen["logical"]


def describe_available(work_dir):
    """One line per model, for a log line or the panel's tooltip."""
    parts = []
    for entry in available(work_dir):
        state = "loadable" if entry["supported"] else "not loadable"
        if not entry["present"]:
            state += ", file missing"
        parts.append(f"{entry['name']} ({state})")
    return ", ".join(parts)
