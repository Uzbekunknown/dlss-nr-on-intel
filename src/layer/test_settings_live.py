"""The daemon's view of the two settings files.

The menu writes the user's settings only on Save, and writes what they are TRYING to a `.live`
file beside it. The daemon has to read the pair as "saved, with the preview on top", and the
distinction matters in one direction only: when the preview stops mentioning a knob, that knob must
go back to its saved value rather than staying where the preview last put it.

That is the failure this exists to catch. Layering the files incrementally - apply whatever is new
in each - leaves a knob stuck at a previewed value forever, which is what Reset would produce: the
panel shows the defaults, the user closes it without saving, and the picture keeps the defaults
instead of going back to what is on disk.

Run: python test_settings_live.py
"""
import argparse
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))


def _dead_pid():
    """A PID that is certainly not running.

    Started and reaped here rather than guessed at, so the value cannot accidentally name a live
    process on the machine the test happens to run on - which is how a "the owner is gone" case
    silently turns into a "the owner is alive" case and stops testing anything.
    """
    import subprocess
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    return proc.pid


def load_daemon():
    """The daemon module, without running its main.

    Imported by path rather than as a module name: it lives in src/layer with the rest of the
    Python side and is not on the path as a package.
    """
    spec = importlib.util.spec_from_file_location("nr_daemon_under_test", HERE / "nr_daemon.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def args_for(settings):
    """The argument namespace `Settings.__init__` reads.

    It takes the parsed command line, so the test builds the same shape rather than reaching into
    the object afterwards - a constructor that reads five attributes would otherwise be half-tested.
    """
    values = {"settings": str(settings)}
    for knob in ("profile", "intensity", "render_scale", "min_extent", "detail_strength",
                 "colour_strength", "temporal", "hold", "release", "cut_limit"):
        values[knob] = None
    return argparse.Namespace(**values)


def main():
    daemon = load_daemon()
    checks = []

    with tempfile.TemporaryDirectory(prefix="nr_settings_live_") as folder:
        folder = pathlib.Path(folder)
        settings = folder / "nr_settings.json"
        live = folder / "nr_settings.json.live"

        def write(path, values):
            path.write_text(json.dumps(values, indent=2) + "\n", encoding="utf-8")

        def write_preview(values, pid=None, confirmed=None):
            """A preview as the layer writes it.

            `pid` defaults to a process that has genuinely exited, so "the menu that wrote this is
            gone" is a fact rather than an assumption. The first version used this test's own PID -
            which stays alive for the whole run - and the guard was never exercised at all: the
            suite passed with the check disabled, which is worse than failing, because it would have
            shipped the bug a second time.

            `pid="live"` picks a running process instead, for the cases that assert the live effect
            still works. `pid=0` writes no PID at all, the shape an older layer left behind.
            """
            payload = dict(values)
            if pid != 0:
                payload["menu_pid"] = _dead_pid() if pid is None else (
                    os.getpid() if pid == "live" else pid)
            if confirmed is not None:
                payload["menu_confirmed"] = 1 if confirmed else 0
            write(live, payload)

        def make():
            s = daemon.Settings(args_for(settings))
            # The constructor takes its values from the namespace; None means "not given", so the
            # knobs are filled from the file. The first refresh is what the daemon does at startup.
            s.refresh()
            return s

        write(settings, {"render_scale": 0.6, "intensity": 0.25, "profile": "natural"})
        s = make()

        checks.append(("the saved file sets the starting values",
                       abs(s.render_scale - 0.6) < 1e-6 and abs(s.intensity - 0.25) < 1e-6,
                       f"render_scale={s.render_scale} intensity={s.intensity}"))

        # The menu previews a different render_scale without saving.
        write_preview(pid="live", values={"render_scale": 0.8})
        s.refresh()
        checks.append(("the preview overrides the saved value",
                       abs(s.render_scale - 0.8) < 1e-6,
                       f"render_scale={s.render_scale}"))

        # The user presses Save: the file and the preview now agree.
        write(settings, {"render_scale": 0.8, "intensity": 0.25, "profile": "natural"})
        write_preview(pid="live", values={"render_scale": 0.8})
        s.refresh()
        checks.append(("a save keeps the value",
                       abs(s.render_scale - 0.8) < 1e-6,
                       f"render_scale={s.render_scale}"))

        # The user previews a change and then closes without saving. The bridge removes the preview
        # and rewrites... nothing: the saved file is still 0.8, so that is what must be in force.
        # This is the case a naive incremental reader gets wrong.
        write_preview(pid="live", values={"render_scale": 0.3, "intensity": 0.9})
        s.refresh()
        checks.append(("a second preview applies",
                       abs(s.render_scale - 0.3) < 1e-6 and abs(s.intensity - 0.9) < 1e-6,
                       f"render_scale={s.render_scale} intensity={s.intensity}"))

        live.unlink()
        write(settings, {"render_scale": 0.8, "intensity": 0.25, "profile": "natural"})
        s.refresh()
        checks.append(("removing the preview restores the saved values",
                       abs(s.render_scale - 0.8) < 1e-6 and abs(s.intensity - 0.25) < 1e-6,
                       f"render_scale={s.render_scale} intensity={s.intensity}"))

        # A preview that stops mentioning a knob must drop that knob back to the saved value, even
        # while other knobs are still being previewed. This is Reset: every knob moves to a default
        # at once, so the preview stays but its contents change wholesale.
        write(settings, {"render_scale": 0.8, "intensity": 0.25})
        write_preview(pid="live", values={"render_scale": 0.3, "intensity": 0.9})
        s.refresh()
        write_preview(pid="live", values={"render_scale": 0.5})          # intensity no longer previewed
        s.refresh()
        # The values are read HERE, not in the summary below: a later check changes them, and a
        # message that reports the value at print time reports the wrong frame - which is what made
        # this look like a logic failure when the assertion itself was already correct.
        scale_now, intensity_now = s.render_scale, s.intensity
        checks.append(("a knob dropped from the preview returns to its saved value",
                       abs(intensity_now - 0.25) < 1e-6 and abs(scale_now - 0.5) < 1e-6,
                       f"render_scale={scale_now} intensity={intensity_now}"))

        # An unrelated key in the preview must not be applied as a knob, and must not crash.
        write_preview(pid="live", values={"render_scale": 0.5, "menu_language": 2})
        s.refresh()
        checks.append(("an unknown key in the preview is ignored",
                       abs(s.render_scale - 0.5) < 1e-6,
                       f"render_scale={s.render_scale}"))

        # --- the reported bug -------------------------------------------------------------------
        #
        # "Save, then Reset, then restart shows the picture from BEFORE the save; touching any
        # option fixes it."
        #
        # A Reset wrote its defaults into the preview and the game exited with the panel open, so
        # the file survived into the next session. No timestamp can separate that from a legitimate
        # preview written at the same distance after the save, and the old preview carries no PID
        # either - the only thing that distinguishes them is that no Save confirmed it. A daemon's
        # FIRST reading is where an inherited preview arrives, so that is where it is refused.
        write(settings, {"render_scale": 0.8, "intensity": 0.25, "profile": "cinematic"})
        # Unconfirmed, and NEWER than the save - exactly the file the user's session left behind.
        write_preview({"render_scale": 0.1, "intensity": 0.1, "profile": "standard"},
                      confirmed=False)

        fresh = daemon.Settings(args_for(settings))     # a new game, a new daemon
        fresh.refresh()
        checks.append(("a leftover unconfirmed preview is ignored",
                       abs(fresh.render_scale - 0.8) < 1e-6
                       and abs(fresh.intensity - 0.25) < 1e-6
                       and fresh.profile == "cinematic",
                       f"render_scale={fresh.render_scale} intensity={fresh.intensity} "
                       f"profile={fresh.profile}"))

        # A CONFIRMED preview left behind by a Save must still be honoured, or the picture after a
        # restart would not match the file the user saved.
        write_preview({"render_scale": 0.8, "intensity": 0.25, "profile": "cinematic"},
                      confirmed=True)
        fresh2 = daemon.Settings(args_for(settings))
        fresh2.refresh()
        checks.append(("a confirmed preview left behind is honoured",
                       abs(fresh2.render_scale - 0.8) < 1e-6,
                       f"render_scale={fresh2.render_scale}"))

        # And the live effect still works: once a daemon is running, a preview it has not seen
        # before is applied whatever the flag says, because that is the user moving a slider.
        write_preview({"render_scale": 0.9, "intensity": 0.6}, pid="live", confirmed=False)
        fresh2.refresh()
        checks.append(("a preview arriving while the daemon runs is applied",
                       abs(fresh2.render_scale - 0.9) < 1e-6 and abs(fresh2.intensity - 0.6) < 1e-6,
                       f"render_scale={fresh2.render_scale} intensity={fresh2.intensity}"))

        # An old preview with no flags at all is from a previous session and is refused, which is
        # the shape of the file the reported bug actually involved.
        write_preview({"render_scale": 0.1, "intensity": 0.1, "profile": "standard"}, pid=0)
        stale_ok = True
        try:
            fresh3 = daemon.Settings(args_for(settings))
            fresh3.refresh()
            stale_ok = abs(fresh3.render_scale - 0.8) < 1e-6 and fresh3.profile == "cinematic"
        except Exception as error:      # noqa: BLE001 - reported, not swallowed
            stale_ok = False
            print(f"  (a flagless preview raised {error!r})")
        checks.append(("a flagless preview from an older layer is refused",
                       stale_ok, "saved values kept"))

    width = max(len(name) for name, _, _ in checks)
    failed = 0
    print()
    for name, ok, detail in checks:
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<{width}}  {detail}")
        failed += 0 if ok else 1
    print()
    print(f"{len(checks) - failed}/{len(checks)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
