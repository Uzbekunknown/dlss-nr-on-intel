#!/usr/bin/env bash
# ============================================================================
#  setup.sh  -  configure a DLSS-NR release to run a game (Linux / macOS)
#
#  Ships inside the release folder, next to the built layer and the runtime. It is the
#  last step a user runs; nothing here compiles anything.
#
#    1. Find nvngx_dlssnr.dll. Three ways, in the order offered:
#         a) one sitting next to this script
#         b) a folder you pick, searched for the file
#         c) the DLSS 5 AIO pack, downloaded into aio/ if you do not have it
#    2. Extract the logical weights from that DLL (needs Python 3 with NumPy and
#       safetensors).
#    3. Install the layer: copy it under a name you choose, write the manifest so its
#       library_path is that file, and write a launcher pointing the daemon at this
#       folder.
#
#  Usage:
#    ./setup.sh --game /path/to/Game
#    ./setup.sh --game ... --dll /path/to/nvngx_dlssnr.dll
#    ./setup.sh --game ... --name libnr_layer.so --skip-weights
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME=""
DLL=""
NAME="libnr_layer.so"
SKIP_WEIGHTS=0
AIODIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --game)         GAME="$2"; shift 2 ;;
    --dll)          DLL="$2";  shift 2 ;;
    --name)         NAME="$2"; shift 2 ;;
    --aio)          AIODIR="$2"; shift 2 ;;
    --skip-weights) SKIP_WEIGHTS=1; shift ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

echo "============================================================"
echo " DLSS-NR on Intel - setup"
echo "============================================================"
echo

# ---- what is in this folder ----
if [[ ! -f "$HERE/nr_layer.so" ]]; then
  echo "ERROR: nr_layer.so is not in $HERE" >&2
  echo "       Run this from inside the release folder it came in." >&2
  exit 1
fi
if [[ ! -d "$HERE/work" ]]; then
  echo "ERROR: the work/ folder is missing next to nr_layer.so" >&2
  exit 1
fi
echo "Release folder: $HERE"
echo

weights="$HERE/work/mlxw/dlssnr-logical.safetensors"

if [[ $SKIP_WEIGHTS -eq 0 && ! -f "$weights" ]]; then
  # ---- 1. the NVIDIA DLL ----
  if [[ -n "$DLL" && ! -f "$DLL" ]]; then
    echo "WARNING: $DLL does not exist; looking for one instead."
    DLL=""
  fi
  if [[ -z "$DLL" && -f "$HERE/nvngx_dlssnr.dll" ]]; then
    DLL="$HERE/nvngx_dlssnr.dll"
    echo "[1/4] found nvngx_dlssnr.dll next to this script"
  fi
  if [[ -z "$DLL" ]]; then
    echo "[1/4] no nvngx_dlssnr.dll beside this script."
    echo
    echo "      Where is yours? Pass a path to the DLL or to the folder holding it,"
    echo "      or press Enter to fetch the DLSS 5 AIO pack instead."
    echo
    read -r -p "Path: " ANSWER || ANSWER=""
    if [[ -n "$ANSWER" ]]; then
      ANSWER="${ANSWER%\"}"; ANSWER="${ANSWER#\"}"
      if [[ -f "$ANSWER/nvngx_dlssnr.dll" ]]; then
        DLL="$ANSWER/nvngx_dlssnr.dll"
      elif [[ -f "$ANSWER" ]]; then
        DLL="$ANSWER"
      else
        echo "  That path does not exist; fetching the pack instead."
      fi
    fi
    if [[ -n "$DLL" ]]; then
      echo "  using $DLL"
    fi
  fi

  # ---- 1c. the DLSS 5 AIO pack ----
  if [[ -z "$DLL" ]]; then
    AIODIR="${AIODIR:-$HERE/aio}"
    mkdir -p "$AIODIR"
    echo
    echo "  Fetching the DLSS 5 AIO pack into $AIODIR ..."
    command -v curl >/dev/null || { echo "ERROR: curl is needed to download it." >&2; exit 3; }
    base="https://github.com/Paimonshen/dllss5-aio/releases/latest/download"
    for part in 001 002 003; do
      f="$AIODIR/DLSS5-AIO-v1.2.5.7z.$part"
      [[ -f "$f" ]] || { echo "  DLSS5-AIO-v1.2.5.7z.$part"; curl -L --fail -o "$f" "$base/DLSS5-AIO-v1.2.5.7z.$part"; }
    done
    if command -v 7z >/dev/null; then
      echo "  7z found - unpacking."
      ( cd "$AIODIR" && 7z x -y DLSS5-AIO-v1.2.5.7z.001 >/dev/null )
    fi
    DLL="$(find "$AIODIR" -name nvngx_dlssnr.dll -print -quit 2>/dev/null || true)"
    if [[ -z "$DLL" ]]; then
      echo >&2
      echo "  The pack is downloaded but not unpacked. Extract DLSS5-AIO-v1.2.5.7z.001" >&2
      echo "  into $AIODIR (it needs 7-Zip, which this script will not install) and" >&2
      echo "  run this again - it will find 02-DLSS5-Neural-Rendering/nvngx_dlssnr.dll." >&2
      exit 3
    fi
    echo "  found $DLL"
  fi

  # ---- 2. Python ----
  command -v python3 >/dev/null || { echo "ERROR: python3 not found on PATH." >&2; exit 3; }
  if ! python3 -c "import numpy, safetensors" >/dev/null 2>&1; then
    echo "ERROR: Python is missing the packages the extractor needs." >&2
    echo "       Run:  python3 -m pip install numpy safetensors" >&2
    exit 3
  fi
  echo "[2/4] extracting weights from $DLL"
  echo "      (a few minutes; the DLL is read, nothing is written back to it)"
  python3 "$HERE/scripts/get_weights.py" "$DLL" --work-dir "$HERE/work"
  [[ -f "$weights" ]] || { echo "ERROR: extraction reported success but $weights is missing." >&2; exit 3; }
  echo
fi

# ---- 3. the game folder ----
if [[ -z "$GAME" ]]; then
  echo "[3/4] Which game? Pass the folder holding its executable."
  read -r -p "Game folder: " GAME || GAME=""
  GAME="${GAME%\"}"; GAME="${GAME#\"}"
fi
[[ -n "$GAME" ]] || { echo "ERROR: no game folder given." >&2; exit 2; }
[[ -d "$GAME" ]] || { echo "ERROR: game folder not found: $GAME" >&2; exit 2; }

# ---- 4. install ----
echo "[4/4] installing into $GAME"
DEST="$GAME/dlss-nr"
mkdir -p "$DEST" "$DEST/work" "$DEST/src"
cp "$HERE/nr_layer.so" "$DEST/$NAME"
echo "  layer:      $DEST/$NAME"

MANIFEST="$HERE/VkLayer_dlss_nr.json"
[[ -f "$MANIFEST" ]] || MANIFEST="$HERE/src/layer/VkLayer_dlss_nr.json"
[[ -f "$MANIFEST" ]] || { echo "ERROR: no VkLayer_dlss_nr.json in the release." >&2; exit 3; }
sed "s|LIBRARY_PATH_PLACEHOLDER|$DEST/$NAME|g" "$MANIFEST" > "$DEST/VkLayer_dlss_nr.json"
echo "  manifest:   $DEST/VkLayer_dlss_nr.json"

for d in layer ref gpu bench; do
  [[ -d "$HERE/src/$d" ]] && cp -r "$HERE/src/$d" "$DEST/src/$d"
done
[[ -d "$HERE/work/mlx-dlss" ]] && cp -r "$HERE/work/mlx-dlss" "$DEST/work/mlx-dlss"
cp "$HERE"/work/*.spv "$DEST/work/" 2>/dev/null || true
cp "$HERE"/work/*.so  "$DEST/work/" 2>/dev/null || true
find "$DEST/src" -type d -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
echo "  runtime:    shaders and libraries"

LAUNCH="$DEST/launch-nr.sh"
{
  echo '#!/usr/bin/env bash'
  echo 'set -e'
  echo '# The loader finds the layer through VK_LAYER_PATH plus the manifest.'
  echo "export VK_LAYER_PATH=\"$DEST\""
  echo 'export VK_INSTANCE_LAYERS=VK_LAYER_dlssnr_intel'
  echo 'export ENABLE_NR_LAYER=1'
  echo 'export NR_LAYER_SPAWN=1'
  echo 'export NR_LAYER_LIVE=1'
  echo 'export NR_LAYER_SOCKET="${NR_LAYER_SOCKET:-/tmp/nr_layer.sock}"'
  echo '# The weights stay in the release folder.'
  echo "export NR_ROOT=\"$HERE\""
  echo ''
  echo 'GAME_EXE="${GAME_EXE:-}"'
  echo 'if [[ -z "$GAME_EXE" ]]; then'
  echo "  GAME_EXE=\$(find '$GAME' -maxdepth 1 -type f -perm -u+x ! -name '*.so' ! -name '*.dll' | head -1)"
  echo 'fi'
  echo 'if [[ -z "$GAME_EXE" ]]; then echo "Set GAME_EXE in this file" >&2; exit 1; fi'
  echo 'exec "$GAME_EXE" "$@"'
} > "$LAUNCH"
chmod +x "$LAUNCH"
echo "  launcher:   $LAUNCH"

echo
echo "Done."
echo "  Run the game through $LAUNCH"
echo "  Weights stayed in $HERE/work/mlxw; the launcher points NR_ROOT there."
echo
echo "COMPLIANCE: nothing NVIDIA's is redistributed by this script. The DLL it read is"
echo "yours, the weights came from it, and both stay on this machine."
