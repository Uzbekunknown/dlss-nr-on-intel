"""The C knob table against `nr_knobs.py`, which is the definition.

The other half of `test_menu_rows.c`. That file checks the table is well formed on its own; this
one checks it agrees with the one place the knobs are actually defined. Neither half can be
satisfied by editing the other, which is the point.

Why the table is copied at all: the menu draws inside the game's process with no interpreter in it,
so it cannot read `nr_knobs.py`. The copy is unavoidable. A copy that is WRONG is not, and this is
what catches that.

It has caught it once already. `render_scale` defaulted to 0.55 in two of the three copies and 1.0
in `nr_knobs.py`, and nothing said so until the three were compared by hand - which is what the
author's review asked to be made mechanical.

How it reads the C table: by compiling a tiny program that prints it through the accessors at the
bottom of `nr_menu_bridge.h`, rather than by parsing the C source. Parsing would pass on a table
the compiler had rejected, and would break on a reformat; asking the compiled object is asking the
same table the menu uses.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))

import nr_knobs  # noqa: E402

# The probe: prints one line a knob, the fields `nr_knobs.py` also has. Built from the same
# sources the layer is, so a table that does not compile fails here rather than silently.
PROBE = r"""
#include "nr_menu_bridge.h"
#include <stdio.h>
int main(void)
{
    int count = nr_menu_bridge_knob_count();
    for (int i = 0; i < count; ++i) {
        const char *name = nr_menu_bridge_knob_name(i);
        float low = 0, high = 0, step = 0, fallback = 0;
        int numeric = nr_menu_bridge_knob_range(i, &low, &high, &step, &fallback);
        if (numeric)
            printf("%s|number|%g|%g|%g|%g\n", name, low, high, step, fallback);
        else
            printf("%s|choice\n", name);
    }
    return 0;
}
"""

SOURCES = ("src/layer/nr_menu_bridge.c", "src/layer/nr_menu_strings.c",
           "src/layer/nr_menu_strings_lookup.c", "src/layer/nr_menu.c",
           "src/layer/nr_menu_font.c", "src/layer/nr_file.c")


def vcvars():
    """The Visual Studio environment script, or None when this is not a Windows build."""
    import os
    vswhere = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    out = subprocess.run([str(vswhere), "-latest", "-products", "*",
                          "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath"],
                         capture_output=True, text=True, timeout=120)
    root = out.stdout.strip()
    if not root:
        return None
    return pathlib.Path(root) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"


def read_c_table():
    """Compile the probe and read the table it prints.

    Returns None when there is no compiler to build it with, which the caller reports as a skip
    rather than a pass - a check that cannot run is not a check that succeeded.

    The probe is built with the Visual Studio compiler through `vcvars64.bat`, which is the only
    way this table is read: the accessors at the bottom of `nr_menu_bridge.h` are what the menu
    itself uses, and asking a compiled object is asking the same table. Off Windows there is no
    MSVC and no `vcvars64.bat`, so this cannot run - which is why the build systems register it
    on Windows only rather than let it exit 1 on every Linux CTest run.
    """
    if sys.platform != "win32":
        return None
    vc = vcvars()
    if vc is None:
        return None

    include = REPO / "src" / "layer"
    with tempfile.TemporaryDirectory(prefix="menu_rows_") as tmpdir:
        tmp = pathlib.Path(tmpdir)
        probe = tmp / "probe.c"
        probe.write_text(PROBE, encoding="utf-8")
        exe = tmp / "probe.exe"

        # The Vulkan include, because nr_menu.c includes vulkan.h. Taken from the SDK the build
        # uses rather than hard-coded, so a machine with a different version still works.
        vk_include = None
        sdk = pathlib.Path(r"C:\VulkanSDK")
        if sdk.exists():
            versions = sorted((p for p in sdk.iterdir() if (p / "Include").exists()), reverse=True)
            if versions:
                vk_include = versions[0] / "Include"

        # Written to a .bat rather than passed as one long `cmd /c`: the command contains quotes
        # around six paths and a `call ... && cl ...` chain, and cmd's quote handling turns that
        # into a silently empty output - which is how this failed the first time, reported as
        # "the probe did not build" with nothing after it.
        lines = [
            "@echo off",
            f'call "{vc}" >nul 2>&1',
            f'cd /d "{REPO}"',
            "cl /nologo /O2 /TC /utf-8 /D_WIN32 "
            + (f'/I"{vk_include}" ' if vk_include else "")
            + f'/I"{include}" /Fe:"{exe}" "{probe}" '
            + " ".join(f'"{REPO / s}"' for s in SOURCES)
            + " /link user32.lib gdi32.lib",
            "echo CL_EXIT=%errorlevel%",
        ]
        bat = tmp / "build.bat"
        bat.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")

        built = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True,
                               timeout=600)
        if not exe.exists():
            print("  the probe did not build:")
            print(built.stdout[-2000:])
            print(built.stderr[-2000:])
            return None

        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        rows = []
        for line in ran.stdout.splitlines():
            parts = line.strip().split("|")
            if len(parts) < 2:
                continue
            name, kind = parts[0], parts[1]
            if kind == "choice":
                rows.append((name, "choice", None, None, None, None))
            else:
                rows.append((name, "number", float(parts[2]), float(parts[3]),
                             float(parts[4]), float(parts[5])))
        return rows


def main():
    print("test_menu_rows: the C table against nr_knobs.py")
    print()

    rows = read_c_table()
    if rows is None:
        print("  SKIP: no compiler available to read the C table")
        print("        (a check that cannot run is not a check that passed)")
        return 1

    c_by_name = {r[0]: r for r in rows}
    py_by_name = {k.name: k for k in nr_knobs.KNOBS}

    failures = []

    missing = sorted(set(py_by_name) - set(c_by_name))
    if missing:
        failures.append(f"the C table is missing: {missing}")
    extra = sorted(set(c_by_name) - set(py_by_name))
    if extra:
        failures.append(f"the C table has knobs nr_knobs.py does not: {extra}")

    for name in sorted(set(py_by_name) & set(c_by_name)):
        c = c_by_name[name]
        p = py_by_name[name]
        if p.kind == "choice":
            if c[1] != "choice":
                failures.append(f"{name}: nr_knobs.py says choice, the C table says {c[1]}")
            continue
        if c[1] != "number":
            failures.append(f"{name}: nr_knobs.py says number, the C table says {c[1]}")
            continue
        for label, want, got in (("low", p.low, c[2]), ("high", p.high, c[3]),
                                 ("step", p.step, c[4]), ("default", p.default, c[5])):
            if abs(float(want) - float(got)) > 1e-6:
                failures.append(f"{name}: {label} is {got} in the C table, "
                                f"{want} in nr_knobs.py")

    if failures:
        print("  FAIL")
        for line in failures:
            print("    - " + line)
        return 1

    print(f"  ok: {len(rows)} knobs agree on name, kind, range, step and default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
