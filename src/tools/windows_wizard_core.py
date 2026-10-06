"""Local Windows release configuration, installation and observations.

Importing this module needs only Python's standard library. Checks never create a
Vulkan instance or run inference. Game/Steam process ownership belongs to the
launcher; this module never starts or kills either one.
"""
from __future__ import annotations

from collections import deque
import base64
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time


SCHEMA_VERSION = 1
OWNER_NAME = ".windows-wizard-owner.json"
LIBRARIES = ("libxmx.dll", "libnr_image.dll", "libnr_alloc.dll")
SHADERS = frozenset((
    "attention.spv", "attention_rows.spv", "ffn_fused.spv", "gemm_batched.spv",
    "gemm_coopmat.spv", "gemm_resident.spv", "gemm_staged.spv", "gemm_staged32.spv",
    "gemm_staged32_deep.spv", "gemm_staged_int8.spv", "gemm_tiled.spv",
    "global_attention.spv", "half_probe.spv", "history.spv", "resident.spv",
    "window_attention.spv", "window_block.spv",
))
SOURCE_DIRS = ("layer", "ref", "gpu", "bench")
RUNTIME_MODULES = (
    "src/layer/nr_daemon.py", "src/layer/nr_alloc.py", "src/layer/nr_pipe.py",
    "src/layer/nr_paths.py", "src/layer/nr_knobs.py", "src/ref/nr_frame.py",
    "src/ref/nr_model.py", "src/ref/nr_image.py", "src/ref/image_io.py",
    "src/gpu/xmx.py", "src/gpu/xmxres.py", "scripts/get_weights.py",
    "work/mlx-dlss/python/mlxdlss/features.py",
    "work/mlx-dlss/python/mlxdlss/tools/extract_dlssnr_weights.py",
    "work/mlx-dlss/python/mlxdlss/tools/unpack_dlssnr_weights.py",
    "LICENSE", "NOTICE", "work/mlx-dlss/LICENSE",
)
MAX_OUTPUT = 256 * 1024
MAX_LOG_READ = 1024 * 1024
MAX_LOG_TAIL = 64 * 1024
_LOG_STATES = {}
_LOG_LOCK = threading.Lock()


@dataclass
class Profile:
    root: Path
    python: Path
    game_exe: Path
    dll: Path | None = None
    api: str = "vulkan"
    game_args: list[str] = field(default_factory=list)
    disable_fossilize: bool = True
    launch_mode: str = "direct"
    steam_app_id: str = ""

    def __post_init__(self):
        self.root = Path(self.root).expanduser().resolve()
        self.python = Path(self.python).expanduser().resolve()
        self.game_exe = Path(self.game_exe).expanduser().resolve()
        self.dll = Path(self.dll).expanduser().resolve() if self.dll else None
        self.game_args = list(self.game_args)

    def to_dict(self):
        return {"root": str(self.root), "python": str(self.python),
                "game_exe": str(self.game_exe), "dll": str(self.dll) if self.dll else None,
                "api": self.api, "game_args": list(self.game_args),
                "disable_fossilize": self.disable_fossilize,
                "launch_mode": self.launch_mode, "steam_app_id": self.steam_app_id}

    @classmethod
    def from_dict(cls, values):
        if not isinstance(values, dict):
            raise ValueError("Profile must be a JSON object")
        arguments = values.get("game_args", [])
        if not isinstance(arguments, list) or any(not isinstance(arg, str) for arg in arguments):
            raise ValueError("game_args must be a list of strings")
        fossilize = values.get("disable_fossilize", True)
        if not isinstance(fossilize, bool):
            raise ValueError("disable_fossilize must be a boolean")
        return cls(root=Path(values.get("root") or Path(__file__).resolve().parents[2]),
                   python=Path(values.get("python") or sys.executable),
                   game_exe=Path(values.get("game_exe") or "."),
                   dll=Path(values["dll"]) if values.get("dll") else None,
                   api=values.get("api", "vulkan"), game_args=arguments,
                   disable_fossilize=fossilize, launch_mode=values.get("launch_mode", "direct"),
                   steam_app_id=str(values.get("steam_app_id") or ""))


def profile_path(root):
    return Path(root) / "work/windows-profile.json"


def _read_json(path, limit=1024 * 1024):
    with Path(path).open("rb") as handle:
        raw = handle.read(limit + 1)
    if len(raw) > limit:
        raise ValueError(f"JSON file is too large: {path}")
    return json.loads(raw.decode("utf-8-sig"))


def _atomic_json(path, values):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".nr-json-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(values, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
        Path(temporary).replace(path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def load_profile(root):
    path = profile_path(root)
    if not path.exists():
        return None
    profile = Profile.from_dict(_read_json(path))
    if _canonical(profile.root) != _canonical(root):
        raise ValueError("Saved profile belongs to a different release root")
    return profile


def save_profile(profile):
    allowed, detail = _profile_change_allowed(profile)
    if not allowed:
        raise ValueError(detail)
    _atomic_json(profile_path(profile.root), profile.to_dict())
    return {"ok": True, "error": None, "path": str(profile_path(profile.root)),
            "profile": profile.to_dict()}


def _canonical(path):
    return os.path.normcase(str(Path(path).resolve())).replace("\\", "/").rstrip("/")


def _profile_change_allowed(profile):
    backup = profile.root / "work/windows-wizard/steam-backup.json"
    if not backup.exists():
        return True, "No pending Steam configuration"
    try:
        state = _read_json(backup, limit=64 * 1024)
        if state.get("restored"):
            return True, "Steam configuration was restored"
        existing = load_profile(profile.root)
        if existing is None:
            return False, "Restore the pending Steam configuration before replacing its missing profile"
        before, after = existing.to_dict(), profile.to_dict()
        before.pop("dll", None)
        after.pop("dll", None)
        if before != after:
            return False, "Restore Steam launch options before changing the selected game or launch profile"
        return True, "The pending Steam configuration uses this unchanged profile"
    except (OSError, ValueError, AttributeError):
        return False, "The pending Steam backup could not be verified; restore it before changing the profile"


def installed_path(profile):
    return profile.game_exe.parent / "dlss-nr"


def _clean_environment():
    values = {key: value for key, value in os.environ.items()
              if not key.upper().startswith(("NR_", "XMX_", "VK_"))
              and key.upper() not in ("ENABLE_NR_LAYER", "DISABLE_NR_LAYER")}
    # A release's compatibility choice is explicit; other Steam layers retain
    # their normal implicit-loader configuration, including the overlay.
    values.pop("DISABLE_VK_LAYER_VALVE_steam_fossilize_1", None)
    return values


def runtime_env(profile):
    values = _clean_environment()
    token = hashlib.sha256(_canonical(profile.root).encode("utf-8")).hexdigest()[:8]
    work = profile.root / "work"
    values.update({
        "NR_ROOT": str(profile.root), "NR_PYTHON": str(profile.python),
        "NR_DAEMON": str(profile.root / "src/layer/nr_daemon.py"),
        "NR_SETTINGS": str(work / "nr_settings.json"),
        "NR_LAYER_LOG": str(work / "nr_daemon.log"),
        "NR_LAYER_TRIGGER": str(work / "nr_trigger"),
        "NR_LAYER_SOCKET": rf"\\.\pipe\nr_windows_{token}",
        "NR_LAYER_LIVE": "1", "NR_LAYER_SPAWN": "1", "NR_KEEP_BLOCKS": "1",
        "ENABLE_NR_LAYER": "1", "VK_LAYER_PATH": str(installed_path(profile)),
        "VK_INSTANCE_LAYERS": "VK_LAYER_dlssnr_intel",
        "PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1",
    })
    if profile.disable_fossilize:
        values["DISABLE_VK_LAYER_VALVE_steam_fossilize_1"] = "1"
    return values


def _hidden_options():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def _run(command, timeout=10, env=None, emit=None, cwd=None):
    """Capture both bounded streams, emitting progress without shell expansion."""
    command = [str(value) for value in command]
    result = {"command": command, "returncode": None, "stdout": "", "stderr": "",
              "output": "", "timed_out": False}
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=env or _clean_environment(),
                                   cwd=cwd, **_hidden_options())
    except OSError as error:
        result["stderr"] = result["output"] = str(error)
        return result
    events = queue.Queue(maxsize=256)

    def read_stream(name, handle):
        try:
            while True:
                chunk = handle.read1(4096)
                if not chunk:
                    break
                events.put((name, chunk))
        finally:
            handle.close()
            events.put((name, None))

    threads = [threading.Thread(target=read_stream, args=(name, handle), daemon=True)
               for name, handle in (("stdout", process.stdout), ("stderr", process.stderr))]
    for worker in threads:
        worker.start()
    chunks = {"stdout": bytearray(), "stderr": bytearray()}
    completed = set()
    deadline = time.monotonic() + timeout
    while len(completed) < 2:
        if result["timed_out"] and time.monotonic() > deadline + 2:
            break
        remaining = deadline - time.monotonic()
        if remaining <= 0 and not result["timed_out"]:
            result["timed_out"] = True
            if process.poll() is None:
                process.kill()  # only the subprocess this invocation created
        try:
            name, chunk = events.get(timeout=0.1)
        except queue.Empty:
            if result["timed_out"] and time.monotonic() > deadline + 2:
                break
            continue
        if chunk is None:
            completed.add(name)
            continue
        room = MAX_OUTPUT - len(chunks[name])
        if room > 0:
            chunks[name].extend(chunk[:room])
        if emit:
            try:
                emit(chunk.decode("utf-8", errors="replace"))
            except Exception:
                pass  # a UI progress callback must not abandon a running child
    try:
        result["returncode"] = process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        result["returncode"] = process.wait(timeout=2)
    for name, content in chunks.items():
        result[name] = bytes(content).decode("utf-8", errors="replace")
    result["output"] = result["stdout"] + result["stderr"]
    if result["timed_out"]:
        result["output"] += f"\nCommand timed out after {timeout:g}s"
    return result


_PYTHON_PROBE = """
import ctypes, importlib, json, os, struct, sys, sysconfig
result = {'platform': sys.platform, 'os_name': os.name, 'bits': struct.calcsize('P') * 8,
          'executable': sys.executable, 'version': sys.version.split()[0],
          'abi_platform': sysconfig.get_platform(),
          'native_apis': hasattr(ctypes, 'WinDLL') and hasattr(os, 'add_dll_directory'),
          'base_prefix': sys.base_prefix, 'packages': {}, 'package_errors': {}}
for name in ('numpy', 'safetensors'):
    try:
        module = importlib.import_module(name)
        result['packages'][name] = getattr(module, '__version__', 'present')
    except Exception as error:
        result['package_errors'][name] = str(error)
print(json.dumps(result))
"""


def _python_info(python, timeout=4):
    got = _run([python, "-I", "-c", _PYTHON_PROBE], timeout=timeout)
    try:
        value = json.loads(got["stdout"].strip().splitlines()[-1])
    except (ValueError, IndexError):
        value = {}
    value["probe_ok"] = got["returncode"] == 0 and not got["timed_out"]
    value["output"] = got["output"]
    return value


def _native_python(info):
    return bool(info.get("probe_ok") and info.get("platform") == "win32"
                and info.get("os_name") == "nt" and info.get("bits") == 64
                and info.get("abi_platform") == "win-amd64" and info.get("native_apis"))


def pe_architecture(path):
    """Read PE headers only; no executable or vendor DLL is loaded here."""
    try:
        with Path(path).open("rb") as handle:
            dos = handle.read(64)
            if len(dos) != 64 or dos[:2] != b"MZ":
                return None
            offset = struct.unpack_from("<I", dos, 60)[0]
            if not 64 <= offset <= 1024 * 1024:
                return None
            handle.seek(offset)
            header = handle.read(26)
        if len(header) != 26 or header[:4] != b"PE\0\0":
            return None
        machine = struct.unpack_from("<H", header, 4)[0]
        magic = struct.unpack_from("<H", header, 24)[0]
        if machine == 0x8664 and magic == 0x20B:
            return "x64"
        if machine == 0x14C and magic == 0x10B:
            return "x86"
        return f"machine-0x{machine:x}"
    except OSError:
        return None


def discover_python():
    """Prefer native CPython over the MinGW toolchain, within a ten-second budget."""
    deadline = time.monotonic() + 9.5
    candidates = [os.environ.get("NR_PYTHON"), sys.executable]
    for name in ("python", "python3"):
        candidates.append(shutil.which(name))
    launcher = shutil.which("py")
    if launcher:
        got = _run([launcher, "-0p"], timeout=2)
        for line in got["stdout"].splitlines():
            match = re.search(r"([A-Za-z]:[\\/].*\.exe)\s*$", line)
            if match:
                candidates.append(match.group(1).strip().strip('"'))
    if os.name == "nt":
        try:
            import winreg
            for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
                try:
                    with winreg.OpenKey(hive, r"Software\Python\PythonCore") as versions:
                        for index in range(winreg.QueryInfoKey(versions)[0]):
                            name = winreg.EnumKey(versions, index)
                            try:
                                with winreg.OpenKey(versions, name + r"\InstallPath") as key:
                                    candidates.append(str(winreg.QueryValue(key, None)) + "\\python.exe")
                            except OSError:
                                continue
                except OSError:
                    continue
        except ImportError:
            pass
    existing = {}
    for value in candidates:
        if value and Path(value).is_file() and pe_architecture(value) == "x64":
            existing.setdefault(_canonical(value), str(Path(value).resolve()))
    ordered = sorted(existing.values(), key=lambda value: (
        any(word in value.lower() for word in ("msys", "mingw", "cygwin")),
        _canonical(value) != _canonical(os.environ.get("NR_PYTHON") or sys.executable),
        value.lower()))
    found = []
    for value in ordered:
        remaining = deadline - time.monotonic()
        if remaining < 0.2:
            break
        if _native_python(_python_info(value, timeout=min(2, remaining))):
            found.append(value)
    return found


def _shader_names(root):
    names = set(SHADERS)
    for name in ("src/gpu/libxmx.c", "src/gpu/xmx.py", "src/gpu/xmxres.py"):
        path = root / name
        if path.is_file():
            names.update(re.findall(r"([A-Za-z0-9_]+\.spv)", path.read_text(encoding="utf-8")))
    return sorted(names)


def _weights_state(profile):
    path = profile.root / "work/mlxw/dlssnr-logical.safetensors"
    try:
        with path.open("rb") as handle:
            size_bytes = handle.read(8)
            if len(size_bytes) != 8:
                raise ValueError("missing safetensors header")
            length = int.from_bytes(size_bytes, "little")
            if not 2 <= length <= 16 * 1024 * 1024:
                raise ValueError("invalid safetensors header length")
            header = json.loads(handle.read(length))
        data_size = path.stat().st_size - 8 - length
        tensors = {key: value for key, value in header.items() if key != "__metadata__"}
        if len(tensors) != 649 or data_size < 0:
            raise ValueError("expected 649 logical tensors")
        for value in tensors.values():
            offsets = value.get("data_offsets", [])
            if (len(offsets) != 2 or not all(isinstance(v, int) for v in offsets)
                    or not 0 <= offsets[0] <= offsets[1] <= data_size):
                raise ValueError("invalid tensor data offsets")
        return {"ready": True, "path": str(path), "detail": "649 local logical tensors"}
    except (OSError, ValueError, TypeError, AttributeError) as error:
        return {"ready": False, "path": str(path), "detail": str(error)}


def _owned_installation(profile):
    path = installed_path(profile)
    if not path.exists():
        return True, "New game-local installation"
    if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
        return False, "Installation directory cannot be a link or junction"
    if not path.is_dir():
        return False, "A file blocks the installation directory"
    try:
        owner = _read_json(path / OWNER_NAME, limit=16 * 1024)
        matches = owner.get("schema_version") == SCHEMA_VERSION and (
            _canonical(owner.get("root", "")) == _canonical(profile.root))
    except (OSError, ValueError, AttributeError):
        matches = False
    return matches, "Owned by this release" if matches else "Existing dlss-nr directory is not owned by this release"


def _load_dependencies(profile):
    code = """
import ctypes, json, os, sys
paths = json.loads(sys.argv[1]); result = {'loaded': [], 'failures': {}}
handles = [os.add_dll_directory(os.path.dirname(path)) for path in paths]
for path in ['vulkan-1.dll'] + paths:
    try:
        ctypes.WinDLL(path)
        result['loaded'].append(path)
    except OSError as error:
        result['failures'][path] = str(error)
print(json.dumps(result))
"""
    got = _run([profile.python, "-I", "-c", code,
                json.dumps([str(profile.root / "work" / name) for name in LIBRARIES])], timeout=4)
    try:
        result = json.loads(got["stdout"].strip().splitlines()[-1])
    except (ValueError, IndexError):
        result = {"failures": {"dependency_probe": got["output"] or "No probe output"}}
    result["ok"] = got["returncode"] == 0 and not got["timed_out"] and not result.get("failures")
    return result


def _has_dxvk(path):
    try:
        with path.open("rb") as handle:
            overlap = b""
            for _ in range(256):
                chunk = handle.read(256 * 1024)
                if not chunk:
                    break
                if b"DXVK" in overlap + chunk:
                    return True
                overlap = chunk[-3:]
    except OSError:
        pass
    return False


def validate(profile):
    checks = []

    def check(name, ok, detail):
        checks.append({"name": name, "ok": bool(ok), "detail": str(detail)})

    check("release_root", profile.root.is_dir() and str(profile.root).isascii(),
          "The permanent release root must exist and use an ASCII path")
    check("api", profile.api in ("vulkan", "dxvk"), "Native Vulkan or an existing x64 DXVK configuration")
    check("launch_mode", profile.launch_mode in ("direct", "steam") and (
        profile.launch_mode != "steam" or not profile.steam_app_id or profile.steam_app_id.isdecimal()),
        "Direct launch, or Steam with an optional numeric app ID; blank uses autodetection")
    check("python_file", profile.python.is_file(), profile.python)
    info = _python_info(profile.python) if profile.python.is_file() else {}
    check("python_native_x64", _native_python(info),
          f"{info.get('platform', 'unknown')} / {info.get('bits', '?')} bit; {info.get('version', '')}")
    check("python_packages", not info.get("package_errors") and all(
        name in info.get("packages", {}) for name in ("numpy", "safetensors")),
        info.get("package_errors") or info.get("packages") or "NumPy and safetensors are required")
    check("game_x64", profile.game_exe.is_file() and pe_architecture(profile.game_exe) == "x64",
          f"{profile.game_exe}: {pe_architecture(profile.game_exe) or 'not a PE executable'}")
    shader_names = _shader_names(profile.root)
    required = ["nr_layer.dll", *RUNTIME_MODULES, *["work/" + name for name in (*LIBRARIES, *shader_names)]]
    missing = [name for name in required if not (profile.root / name).is_file()]
    missing.extend("src/" + name for name in SOURCE_DIRS if not (profile.root / "src" / name).is_dir())
    check("runtime_files", not missing, "Missing: " + ", ".join(missing) if missing else "Complete local release runtime and extractors")
    bad_arch = [name for name in ("nr_layer.dll", *["work/" + value for value in LIBRARIES])
                if pe_architecture(profile.root / name) != "x64"]
    check("runtime_x64", not bad_arch, "Not x64 PE: " + ", ".join(bad_arch) if bad_arch else "Layer and native libraries are x64")
    dependency = _load_dependencies(profile) if _native_python(info) and not bad_arch else {"ok": False, "failures": {"probe": "Requires native x64 Python and x64 runtime"}}
    check("runtime_dependencies", dependency.get("ok"), dependency.get("failures") or "Vulkan loader and native DLL dependencies load; no GPU work performed")
    weights = _weights_state(profile)
    dll_ok = bool(profile.dll and profile.dll.is_file() and profile.dll.name.lower() == "nvngx_dlssnr.dll"
                  and pe_architecture(profile.dll) == "x64")
    check("local_model_input", weights["ready"] or dll_ok,
          weights["detail"] if weights["ready"] else "Supply your own x64 nvngx_dlssnr.dll or 649-tensor local logical weights")
    owns, detail = _owned_installation(profile)
    check("installation_target", owns, detail)
    allowed, detail = _profile_change_allowed(profile)
    check("steam_profile_lock", allowed, detail)
    for name in ("windows-profile.json", "nr_settings.json", "nr_trigger"):
        path = profile.root / "work" / name
        check("state_path_" + name, not path.exists() or (path.is_file() and not path.is_symlink()), path)
    settings_path = profile.root / "work/nr_settings.json"
    try:
        settings_ok = not settings_path.exists() or isinstance(_read_json(settings_path), dict)
    except (OSError, ValueError):
        settings_ok = False
    check("settings_format", settings_ok, "Existing settings must be a JSON object")
    template = profile.root / "VkLayer_dlss_nr.json"
    if not template.is_file():
        template = profile.root / "src/layer/VkLayer_dlss_nr.json"
    try:
        manifest = _read_json(template)
        manifest_ok = isinstance(manifest.get("layer"), dict) and manifest["layer"].get("name") == "VK_LAYER_dlssnr_intel"
    except (OSError, ValueError, AttributeError):
        manifest_ok = False
    check("manifest", manifest_ok, "A valid layer manifest must be present before installation")
    if profile.api == "dxvk":
        candidates = (("d3d11.dll", "dxgi.dll"), ("d3d9.dll",))
        valid = [names for names in candidates if all(
            pe_architecture(profile.game_exe.parent / name) == "x64" and _has_dxvk(profile.game_exe.parent / name)
            for name in names)]
        check("existing_dxvk", bool(valid),
              "Existing x64 DXVK: " + ", ".join(valid[0]) if valid else "No complete existing x64 DXVK d3d11/dxgi or d3d9 pair; the wizard does not install DXVK")
    ok = all(item["ok"] for item in checks)
    return {"ok": ok, "error": None if ok else "Preflight checks failed", "checks": checks,
            "python_info": info, "weights": weights, "runtime_dependencies": dependency}


def ensure_dependencies(profile, emit=None):
    info = _python_info(profile.python)
    if not _native_python(info):
        return {"ok": False, "error": "Choose native x64 Windows Python before preparing dependencies", "output": info.get("output", "")}
    directory = profile.root / "work/windows-python"
    interpreter = directory / "Scripts/python.exe"
    commands = []
    try:
        if directory.exists() and (directory.is_symlink() or not (directory / "pyvenv.cfg").is_file()):
            raise ValueError("Existing windows-python directory is not a local Python virtual environment")
        if not interpreter.is_file():
            directory.parent.mkdir(parents=True, exist_ok=True)
            if emit:
                emit("Creating release-local Python environment\n")
            got = _run([profile.python, "-I", "-m", "venv", str(directory)], timeout=120, emit=emit)
            commands.append(got)
            if got["returncode"] != 0 or got["timed_out"]:
                raise RuntimeError("Could not create the local virtual environment")
        if emit:
            emit("Installing NumPy and safetensors into the local environment\n")
        got = _run([interpreter, "-I", "-m", "pip", "install", "--disable-pip-version-check",
                    "--only-binary=:all:", "numpy", "safetensors"], timeout=600, emit=emit)
        commands.append(got)
        if got["returncode"] != 0 or got["timed_out"]:
            raise RuntimeError("Dependency installation failed; the global interpreter was not modified")
        chosen = replace(profile, python=interpreter)
        chosen_info = _python_info(chosen.python)
        if (not _native_python(chosen_info) or chosen_info.get("package_errors")
                or not all(name in chosen_info.get("packages", {}) for name in ("numpy", "safetensors"))):
            raise RuntimeError("The local interpreter did not pass its dependency checks")
        return {"ok": True, "error": None, "python": str(chosen.python),
                "profile": chosen.to_dict(), "output": "\n".join(value["output"] for value in commands),
                "commands": commands}
    except (OSError, ValueError, RuntimeError) as error:
        return {"ok": False, "error": str(error), "output": "\n".join(value["output"] for value in commands), "commands": commands}


def _copy_runtime(profile, stage, destination):
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", "*.safetensors", "nvngx_dlssnr.dll", ".git")
    for name in SOURCE_DIRS:
        shutil.copytree(profile.root / "src" / name, stage / "src" / name,
                        dirs_exist_ok=True, ignore=ignore)
    shutil.copytree(profile.root / "work/mlx-dlss", stage / "work/mlx-dlss",
                    dirs_exist_ok=True, ignore=ignore)
    shutil.copy2(profile.root / "nr_layer.dll", stage / "nr_layer.dll")
    for name in (*LIBRARIES, *_shader_names(profile.root)):
        shutil.copy2(profile.root / "work" / name, stage / "work" / name)
    template = profile.root / "VkLayer_dlss_nr.json"
    if not template.is_file():
        template = profile.root / "src/layer/VkLayer_dlss_nr.json"
    manifest = _read_json(template)
    manifest["layer"]["library_path"] = str(destination / "nr_layer.dll")
    manifest["layer"]["library_arch"] = "64"
    _atomic_json(stage / "VkLayer_dlss_nr.json", manifest)
    _atomic_json(stage / OWNER_NAME, {"schema_version": SCHEMA_VERSION, "root": str(profile.root),
                                    "game_exe": str(profile.game_exe)})


def _process_snapshot():
    if os.name != "nt":
        raise ValueError("Windows process inspection is required before installation")
    executable = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    script = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $ErrorActionPreference='Stop'; @(Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath) | ConvertTo-Json -Compress"
    got = _run([executable, "-NoProfile", "-NonInteractive", "-Command", script], timeout=4)
    if got["returncode"] != 0 or got["timed_out"]:
        raise ValueError("Could not safely inspect running games: " + got["output"])
    parsed = json.loads(got["stdout"] or "[]")
    return parsed if isinstance(parsed, list) else [parsed]


def _launch_state(profile):
    path = profile.root / "work/windows-wizard/launch-state.json"
    if not path.exists():
        return {}
    value = _read_json(path, limit=128 * 1024)
    if not isinstance(value, dict):
        raise ValueError("Launch state must be a JSON object")
    return value


def _install_process_guard(profile):
    try:
        processes = _process_snapshot()
        state = _launch_state(profile)
        game_directory = _canonical(profile.game_exe.parent)
        for process in processes:
            executable = process.get("ExecutablePath")
            if executable and _canonical(executable).startswith(game_directory + "/"):
                return False, "Close the selected game before changing its installation"
            if not executable and str(process.get("Name", "")).casefold() == profile.game_exe.name.casefold():
                return False, "A matching game process could not be inspected; close it before installation"
            if state.get("pid") == process.get("ProcessId") and state.get("game_exe") and executable:
                if _canonical(executable) == _canonical(state["game_exe"]):
                    return False, "Another game is still using this release; close it before installation"
        return True, "No selected or release-owned game is running"
    except (OSError, ValueError, TypeError) as error:
        return False, str(error)


def install(profile, emit=None):
    allowed, detail = _install_process_guard(profile)
    if not allowed:
        return {"ok": False, "error": detail}
    checked = validate(profile)
    failed = {item["name"] for item in checked["checks"] if not item["ok"]}
    if failed == {"python_packages"}:
        prepared = ensure_dependencies(profile, emit=emit)
        if not prepared["ok"]:
            return prepared
        profile = Profile.from_dict(prepared["profile"])
        checked = validate(profile)
    if not checked["ok"]:
        return checked
    commands = []
    if not checked["weights"]["ready"]:
        if emit:
            emit("Extracting logical weights from your local NVIDIA DLL\n")
        extracted = _run([profile.python, "-I", str(profile.root / "scripts/get_weights.py"),
                          profile.dll, "--work-dir", str(profile.root / "work")],
                         timeout=900, cwd=profile.root, emit=emit)
        commands.append(extracted)
        if extracted["returncode"] != 0 or extracted["timed_out"] or not _weights_state(profile)["ready"]:
            return {"ok": False, "error": "Weight extraction failed or did not produce 649 logical tensors",
                    "commands": commands, "output": extracted["output"]}
    # Recheck every input and destination after extraction, before any game write.
    checked = validate(profile)
    if not checked["ok"]:
        return checked
    allowed, detail = _install_process_guard(profile)
    if not allowed:
        return {"ok": False, "error": detail}
    destination = installed_path(profile)
    stage = backup = None
    committed = False
    work = profile.root / "work"
    state_files = [profile_path(profile.root), work / "nr_settings.json", work / "nr_trigger"]
    saved = {}
    try:
        saved = {path: path.read_bytes() if path.exists() else None for path in state_files}
        stage = Path(tempfile.mkdtemp(prefix=".dlss-nr-stage-", dir=destination.parent))
        if destination.exists():
            shutil.copytree(destination, stage, dirs_exist_ok=True)
        _copy_runtime(profile, stage, destination)
        allowed, detail = _install_process_guard(profile)
        if not allowed:
            raise ValueError(detail)
        owns, detail = _owned_installation(profile)
        if not owns:
            raise ValueError(detail)
        if destination.exists():
            backup = Path(tempfile.mkdtemp(prefix=".dlss-nr-backup-", dir=destination.parent))
            backup.rmdir()
            destination.rename(backup)
        stage.rename(destination)
        stage = None
        committed = True
        settings = {}
        if saved[work / "nr_settings.json"]:
            settings = json.loads(saved[work / "nr_settings.json"].decode("utf-8-sig"))
            if not isinstance(settings, dict):
                raise ValueError("Settings must be a JSON object")
        settings.update({"render_scale": 0.4, "min_extent": 320})
        _atomic_json(work / "nr_settings.json", settings)
        (work / "nr_trigger").unlink(missing_ok=True)
        save_profile(profile)
    except (OSError, ValueError, TypeError, KeyError) as error:
        if committed:
            shutil.rmtree(destination)
        if backup and backup.exists():
            backup.rename(destination)
            backup = None
        if committed:
            for path, content in saved.items():
                if content is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_bytes(content)
        return {"ok": False, "error": str(error), "commands": commands}
    finally:
        if stage and stage.exists():
            shutil.rmtree(stage)
    if backup and backup.exists():
        shutil.rmtree(backup)
    if emit:
        emit("Installation complete; the effect is off\n")
    return {"ok": True, "error": None, "installed": str(destination), "root": str(profile.root),
            "profile": profile.to_dict(), "commands": commands, "trigger_exists": False}


def set_effect(profile, enabled):
    owns, detail = _owned_installation(profile)
    if not owns or not (installed_path(profile) / OWNER_NAME).is_file():
        return {"ok": False, "error": detail if not owns else "Install this profile before toggling its effect"}
    trigger = Path(runtime_env(profile)["NR_LAYER_TRIGGER"])
    try:
        launch = _launch_state(profile)
        if launch.get("game_exe") and _canonical(launch["game_exe"]) != _canonical(profile.game_exe):
            for process in _process_snapshot():
                if process.get("ProcessId") == launch.get("pid"):
                    raise ValueError("The active game uses another profile; select it before changing its effect")
        if trigger.is_symlink() or (trigger.exists() and not trigger.is_file()):
            raise ValueError("The release trigger path is not an owned regular file")
        if enabled:
            trigger.parent.mkdir(parents=True, exist_ok=True)
            trigger.touch()
        else:
            trigger.unlink(missing_ok=True)
        return {"ok": True, "error": None, "trigger_exists": bool(enabled),
                "trigger": str(trigger), "processing_confirmed": False,
                "detail": "Trigger changed; fresh processed frames must confirm processing"}
    except (OSError, ValueError) as error:
        return {"ok": False, "error": str(error)}


def _new_log_state(complete=True):
    return {"offset": 0, "pending": b"", "identity": None, "prefix": None,
            "processed": 0, "rejected": 0, "errors": 0, "error_tail": deque(maxlen=30),
            "last_frame": None, "network_shape": None, "half_probe": None, "allocator": False,
            "model_ready": None, "session": 0, "counts_complete": complete}


def _parse_log_line(state, line):
    if line.startswith("half rounding:"):
        previous_session = state["session"]
        for name, value in _new_log_state().items():
            if name not in ("offset", "pending", "identity", "prefix"):
                state[name] = value
        state["session"] = previous_session + 1
        state["half_probe"] = {"checked": "NOT CHECKED" not in line, "ok": None, "detail": line}
    if "half_round uses" in line and state["half_probe"]:
        state["half_probe"]["detail"] += "\n" + line
    if line.startswith("model ready"):
        if state["model_ready"]:
            previous_session = state["session"]
            for name, value in _new_log_state().items():
                if name not in ("offset", "pending", "identity", "prefix"):
                    state[name] = value
            state["session"] = previous_session + 1
        state["model_ready"] = line
        if (state["half_probe"] and state["half_probe"]["checked"]
                and state["half_probe"]["ok"] is None):
            state["half_probe"]["ok"] = True
    if "keeping NumPy's large blocks" in line:
        state["allocator"] = True
    if line.startswith("frame rejected/failed") or "unsupported VkFormat" in line:
        state["rejected"] += 1
    if any(word in line for word in ("frame rejected/failed", "GPU lost", "Traceback", "FAIL:", "ERROR", "unsupported VkFormat")):
        state["errors"] += 1
        state["error_tail"].append(line[:500])
        if line.startswith("FAIL:") and state["half_probe"]:
            state["half_probe"]["ok"] = False
    match = re.match(r"^(\d+)x(\d+)\s+\S+\s+in\s+(\d+(?:\.\d+)?)s\s+change\s+(\S+)", line)
    if match:
        state["processed"] += 1
        network = re.search(r"\bnetwork (\d+)x(\d+)", line)
        state["network_shape"] = [int(value) for value in network.groups()] if network else None
        state["last_frame"] = {"width": int(match[1]), "height": int(match[2]),
                               "seconds": float(match[3]), "change": match[4], "line": line[:1000]}
        if match[4].lower() in ("nan", "inf", "-inf"):
            state["errors"] += 1
            state["error_tail"].append("Non-finite image change: " + line[:400])


def status(profile):
    log = Path(runtime_env(profile)["NR_LAYER_LOG"])
    key = _canonical(log)
    observer = profile.root / "work/windows-wizard/status-observer.json"
    try:
        launch = _launch_state(profile)
    except (OSError, ValueError):
        launch = {}
    matching_launch = bool(launch.get("root") and launch.get("game_exe")
                           and _canonical(launch["root"]) == _canonical(profile.root)
                           and _canonical(launch["game_exe"]) == _canonical(profile.game_exe))
    context = {"root": _canonical(profile.root), "game_exe": _canonical(profile.game_exe),
               "pid": launch.get("pid") if matching_launch else None,
               "started_utc": launch.get("started_utc") if matching_launch else None,
               "start_offset": launch.get("daemon_start_bytes", 0) if matching_launch else 0}
    if not isinstance(context["start_offset"], int) or context["start_offset"] < 0:
        context["start_offset"] = 0
    context_key = hashlib.sha256(json.dumps(context, sort_keys=True).encode("utf-8")).hexdigest()
    with _LOG_LOCK:
        state = _LOG_STATES.get(key)
        if state is None:
            try:
                saved = _read_json(observer, limit=256 * 1024)
                if saved.get("schema_version") == SCHEMA_VERSION and saved.get("context_key") == context_key:
                    state = saved["state"]
                    state["pending"] = base64.b64decode(state["pending"])
                    state["prefix"] = bytes.fromhex(state["prefix"]) if state["prefix"] is not None else None
                    state["identity"] = tuple(state["identity"]) if state["identity"] else None
                    state["error_tail"] = deque(state["error_tail"], maxlen=30)
            except (OSError, ValueError, TypeError, KeyError):
                state = None
        if state is None or state.get("context_key") != context_key:
            state = _new_log_state()
            state["offset"] = context["start_offset"]
            state["context_key"] = context_key
        _LOG_STATES[key] = state
        initial = state["identity"] is None
        fresh_processed = 0
        mtime = None
        try:
            stat = log.stat()
            mtime = stat.st_mtime
            with log.open("rb") as handle:
                prefix = handle.read(64)
                identity = (stat.st_dev, stat.st_ino)
                # A short file's first 64 bytes legitimately grow. Only a changed
                # existing prefix, replaced file or truncation starts a new log.
                prefix_changed = state["prefix"] is not None and not prefix.startswith(state["prefix"])
                if (state["identity"] is not None and identity != state["identity"]) or stat.st_size < state["offset"] or prefix_changed:
                    state = _new_log_state(complete=stat.st_size <= MAX_LOG_READ)
                    state["context_key"] = context_key
                    _LOG_STATES[key] = state
                    initial = True
                offset = state["offset"]
                if stat.st_size - offset > MAX_LOG_READ:
                    offset = stat.st_size - MAX_LOG_READ
                    state["counts_complete"] = False
                    state["pending"] = b""
                handle.seek(offset)
                content = handle.read(min(MAX_LOG_READ, max(0, stat.st_size - offset)))
                consumed = handle.tell()
            if offset and offset != state["offset"]:
                content = content.split(b"\n", 1)[-1]
            before = state["processed"]
            content = state["pending"] + content
            lines = content.split(b"\n")
            state["pending"] = lines.pop()
            for line in lines:
                _parse_log_line(state, line.decode("utf-8", errors="replace").rstrip("\r"))
            # First observation after an authenticated launch may contain its
            # first frames. An unassociated historical log cannot confirm them.
            fresh_processed = max(0, state["processed"] - before) if not initial or matching_launch else 0
            state.update(offset=consumed, identity=identity, prefix=prefix, context_key=context_key)
        except FileNotFoundError:
            state = _new_log_state()
            state["context_key"] = context_key
            _LOG_STATES[key] = state
        except OSError as error:
            return {"ok": False, "error": str(error), "log_path": str(log)}
        result = {name: value for name, value in state.items()
                  if name not in ("offset", "pending", "identity", "prefix", "error_tail", "context_key")}
        result.update(ok=True, error=None, error_tail=list(state["error_tail"]),
                      trigger_exists=Path(runtime_env(profile)["NR_LAYER_TRIGGER"]).is_file(),
                      log_path=str(log), log_mtime=mtime, fresh_processed=fresh_processed,
                      processing_observed=state["processed"] > 0,
                      game_connected=None)
        persisted = dict(state, pending=base64.b64encode(state["pending"]).decode("ascii"),
                         prefix=state["prefix"].hex() if state["prefix"] is not None else None,
                         identity=list(state["identity"]) if state["identity"] else None,
                         error_tail=list(state["error_tail"]))
        try:
            _atomic_json(observer, {"schema_version": SCHEMA_VERSION, "context_key": context_key,
                                    "state": persisted})
        except OSError as error:
            result["observer_error"] = str(error)
    active = False
    process_error = None
    if matching_launch and isinstance(launch.get("pid"), int) and launch.get("exit_code") is None:
        try:
            active = any(process.get("ProcessId") == launch["pid"] and process.get("ExecutablePath")
                         and _canonical(process["ExecutablePath"]) == _canonical(profile.game_exe)
                         for process in _process_snapshot())
        except (OSError, ValueError):
            process_error = "The launch PID could not be verified"
    result.update(launch=context, active_game=active,
                  fresh_frames=bool(active and fresh_processed > 0 and mtime is not None
                                    and time.time() - mtime < 10))
    if process_error:
        result["process_observation_error"] = process_error
    return result


def _system_metadata():
    values = {"platform": platform.platform(), "system": platform.system(),
              "release": platform.release(), "version": platform.version(), "gpu": []}
    if os.name != "nt":
        return values
    script = """
$null = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture
$gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,PNPDeviceID)
@{windows=$os;gpu=$gpu} | ConvertTo-Json -Depth 5 -Compress
"""
    command = "powershell.exe"
    system_root = os.environ.get("SystemRoot")
    if system_root:
        command = str(Path(system_root) / "System32/WindowsPowerShell/v1.0/powershell.exe")
    got = _run([command, "-NoProfile", "-NonInteractive", "-Command", script], timeout=8)
    try:
        if got["returncode"] == 0:
            values.update(json.loads(got["stdout"]))
        else:
            values["hardware_query_error"] = got["output"]
    except ValueError:
        values["hardware_query_error"] = got["output"]
    return values


def export_report(profile, destination):
    destination = Path(destination)
    if destination.is_dir():
        destination /= "nr-windows-report-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + ".json"
    if destination.suffix.lower() != ".json":
        return {"ok": False, "error": "Report destination must be a JSON file"}
    try:
        observation = status(profile)
        log = Path(runtime_env(profile)["NR_LAYER_LOG"])
        tail = ""
        if log.is_file():
            with log.open("rb") as handle:
                handle.seek(max(0, log.stat().st_size - MAX_LOG_TAIL))
                tail = handle.read(MAX_LOG_TAIL).decode("utf-8", errors="replace")
        metadata = None
        metadata_error = None
        path = profile.root / "release-metadata.json"
        if path.is_file():
            try:
                metadata = _read_json(path, limit=64 * 1024)
            except (OSError, ValueError) as error:
                metadata_error = str(error)
        try:
            settings = _read_json(profile.root / "work/nr_settings.json")
            settings_error = None
        except (OSError, ValueError) as error:
            settings = None
            settings_error = str(error)
        report = {"schema_version": SCHEMA_VERSION, "created_utc": datetime.now(timezone.utc).isoformat(),
                  "release": metadata, "release_metadata_error": metadata_error,
                  "root": str(profile.root), "profile": profile.to_dict(),
                  "system": _system_metadata(), "python": _python_info(profile.python),
                  "settings": settings, "settings_error": settings_error,
                  "status": observation, "daemon_log_tail": tail,
                  "note": "Daemon timings and observed frame counts are not game FPS. No DLL or weights are included."}
        _atomic_json(destination, report)
        return {"ok": True, "error": None, "path": str(destination)}
    except (OSError, ValueError) as error:
        return {"ok": False, "error": str(error)}
