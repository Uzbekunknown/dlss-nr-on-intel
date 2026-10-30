"""Which weight file the daemon should load, decided by `NR_PRESET` and nothing else.

**This replaced a model table, a manifest and a name-based lookup, and the reason is a bug the
maintainer hit.** The tree used to carry two ways to choose the network:

  - `NR_PRESET` (A or B), written beside the game by the setup window, read by the daemon;
  - a `weights` knob in the panel, which named a MODEL (`ht`, `cc_small_fp8`) and resolved it
    through `work/mlxw/weights.json` - a manifest that only this project's own extractor wrote.

Two selection paths for one fact is what produced "network preset A" logged beside a B model, and
what made `NR_PRESET=B` run A on Linux, where nothing writes that manifest: `resolve()` found no
manifest, listed HT alone, and silently fell back. Silent, because a fallback is a legitimate
outcome for "the model you named is not installed".

There is one choice now, it is `NR_PRESET`, and it maps to a file by name:

    A -> work/mlxw/dlssnr-logical.safetensors        (the model this project reproduces)
    B -> work/mlxw/dlssnr-b-logical.safetensors      (310.9.16's second network, 31 blocks)

Both are produced by `src/tools/unpack_preset.py`, which is what the installer runs. No manifest, no
model names, and nothing to fall back to: if the file for the preset is not there, the daemon says
which file it wanted and how to make it, rather than quietly running the other network.
"""
import pathlib

#: The preset names, as `NR_PRESET` spells them.
PRESETS = ("A", "B")

#: The logical weight file each preset runs. Both live in `work/mlxw/`, both are extracted by
#: `src/tools/unpack_preset.py`, and neither is renamed by this module.
PRESET_WEIGHTS = {
    "A": "dlssnr-logical.safetensors",
    "B": "dlssnr-b-logical.safetensors",
}

#: What each one is, for the line the daemon logs and for the setup window's description.
PRESET_NOTES = {
    "A": "the model this project reproduces",
    "B": "the faster model a 310.9.16-or-later DLL also carries",
}

#: The manifest and the model table are gone. Kept as names so a stale import fails with a sentence
#: that names the change rather than with a TypeError from calling nothing.
REMOVED = ("MANIFEST_NAME", "MODELS", "BY_NAME", "BY_RESOURCE", "resolve", "available",
           "default_name", "names", "describe_available", "manifest_path", "read_manifest",
           "Model", "LEGACY_LOGICAL", "MANIFEST_FORMAT")


def preset_of(value):
    """The preset `value` names, or "A".

    Anything unrecognised is A, which is the shipped default: a settings file from before the
    presets, or a hand-edited one, must start a working daemon rather than refuse to start.
    """
    text = str(value or "").strip().upper()
    return text if text in PRESETS else "A"


def weights_path(work_dir, preset):
    """The logical weight file for `preset`, whether or not it exists.

    Returning the path even when the file is absent is deliberate: the caller's error message names
    the exact file it wanted, and "work/mlxw/dlssnr-b-logical.safetensors is missing" is a better
    sentence than "no weights".
    """
    return pathlib.Path(work_dir) / "mlxw" / PRESET_WEIGHTS[preset_of(preset)]


def installed(work_dir, preset):
    """Whether the file for `preset` is there."""
    return weights_path(work_dir, preset).is_file()


def describe(work_dir):
    """Which presets this install can run, for the setup window's check and the daemon's log.

    A dict of preset -> bool in `PRESETS` order, so both readers say "B is not extracted" with the
    same words.
    """
    return {name: installed(work_dir, name) for name in PRESETS}


def missing_message(work_dir, preset):
    """What to tell someone whose install has no file for the preset they asked for."""
    preset = preset_of(preset)
    path = weights_path(work_dir, preset)
    return (f"preset {preset} needs {path.name}, which is not in {path.parent}. "
            f"Run src/tools/unpack_preset.py on a 310.9.16-or-later nvngx_dlssnr.dll to extract "
            f"it - the installer does this when it is given one.")


def __getattr__(name):
    if name in REMOVED:
        raise AttributeError(
            f"nr_weight_models.{name} was removed: the network is chosen by NR_PRESET alone now "
            f"(see this module's docstring). Use preset_of/weights_path/installed instead.")
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
