#!/usr/bin/env python3
"""player_reports.py: a digest keeps no trace of the player, both frame lines parse, the probe
report parses as coopmat_probe.c writes it, and the fit recovers a known line. The committed data
file must load, and every speed test in it must fit."""
import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import player_reports as P  # noqa: E402

# What a Save report carries that names the player; none of it may reach a digest.
PRIVATE = ("C:\\Users\\someone", "someone", "DESKTOP-PRIVATE", "\\\\.\\pipe\\nr_windows_feed",
           "8f2c0ffee", "PCI\\VEN_8086&DEV_56A0&SUBSYS_0000&REV_08\\4&1234", "31337")

CPU_LINE = ("1920x1080 R8G8B8A8_UNORM in 0.06s  change 0.01310  gate 0.020, held 12%, released 44%, "
            "cut 0.1026  gpu 2+22+1ms  network 960x576 scale 0.5")
GPU_LINE = ("1280x720 R8G8B8A8_UNORM in 0.042s  change 0.02589  cut 0.0003  gpu 0+37+0ms  network "
            "512x320 scale 0.4 transport=gpu-shared image prep=1.418ms compose=0.492ms history=still")


def report():
    return {
        "created_utc": "2026-11-01T13:43:01+00:00",
        "release": {"version": None, "source_commit": "c0b7f9d1041e7a4f5cf33f76c5f02b93e78ab208"},
        "root": "C:\\Users\\someone\\DLSS-NR",
        "profile": {"root": "C:\\Users\\someone\\DLSS-NR", "api": "vulkan", "preset": "A",
                    "game_exe": "C:\\Users\\someone\\Games\\Nuclear Option\\NuclearOption.exe",
                    "gpu_image_path": True, "nr_every": 1, "nr_async": False},
        "system": {"gpu": [{"Name": "Virtual Desktop Monitor", "DriverVersion": "1.0",
                            "PNPDeviceID": "ROOT\\DISPLAY\\0000"},
                           {"Name": "Intel(R) Arc(TM) A770 Graphics", "DriverVersion": "32.0.101.9150",
                            "PNPDeviceID": PRIVATE[5]}],
                   "cpu": {"Name": "AMD Ryzen 5 5600 6-Core Processor   "},
                   "memory_bytes": 34282635264, "windows": {"BuildNumber": "26200"},
                   "computer": PRIVATE[2]},
        "python": {"base_prefix": "C:\\Users\\someone\\AppData\\Local\\Programs\\Python"},
        "dll": {"version": "310.9.16.0", "sha256": PRIVATE[4]},
        "settings": {"render_scale": 0.4, "min_extent": 320},
        "status": {"model_ready": "model ready in 1.3s on Intel(R) Arc(TM) A770 Graphics",
                   "daemon_session": {"endpoint": PRIVATE[3], "runtime_pid": int(PRIVATE[6])}},
        "speed_test": {"device": "Intel(R) Arc(TM) A770 Graphics", "preset": "B",
                       "created_utc": "2026-11-01T13:17:56+00:00", "blocks": 31,
                       "networks": [{"preset": "B", "reported": "B"}],
                       "cases": [{"width": 1280, "height": 720, "render_scale": 0.4,
                                  "network": [512, 320], "network_ms": 19, "round_trip_ms": 26.9,
                                  "preset": "B"}]},
        "daemon_log_tail": "\n".join(["listening on " + PRIVATE[3], GPU_LINE, GPU_LINE.replace(
            "0+37+0ms", "0+39+0ms").replace("in 0.042s", "in 0.044s"), GPU_LINE, CPU_LINE]),
    }


class Digest(unittest.TestCase):
    def test_nothing_private(self):
        text = json.dumps(P.digest(report(), issue=25))
        for private in PRIVATE:
            self.assertNotIn(private.replace("\\", "\\\\"), text)
            self.assertNotIn(private, text)

    def test_what_it_keeps(self):
        got = P.digest(report(), issue=25)
        self.assertEqual(got["release"], "0.0.3.1")     # from the commit, when no version is written
        self.assertEqual((got["gpu"], got["driver"]), ("Intel(R) Arc(TM) A770 Graphics", "32.0.101.9150"))
        self.assertEqual(got["cpu"], "AMD Ryzen 5 5600 6-Core Processor")
        self.assertEqual(got["game_exe"], "NuclearOption.exe")
        self.assertEqual(got["speed_test"]["networks_named"], ["B"])
        self.assertEqual(got["speed_test"]["cases"][0]["field"], [512, 320])

    def test_frames_both_paths(self):
        frames = {f["transport"]: f for f in P.digest(report())["frames"]}
        gpu, cpu = frames["gpu-shared"], frames["cpu"]
        self.assertEqual((gpu["frames"], gpu["network_ms"], gpu["total_ms"]), (3, 37, 42.0))
        self.assertEqual((gpu["prep_ms"], gpu["compose_ms"]), (1.42, 0.49))   # medians, to 10 us
        self.assertEqual((cpu["upload_ms"], cpu["network_ms"], cpu["readback_ms"]), (2, 22, 1))
        self.assertIsNone(cpu["prep_ms"])
        self.assertEqual(cpu["field"], [960, 576])


class Probe(unittest.TestCase):
    TEXT = """physical devices: 2

=== device 0: Intel(R) Arc(TM) A770 Graphics ===
  apiVersion    : 1.4.359
  driver        : Intel Corporation, 101.9150
  subgroup size : 32

   idx   M   N   K   A      B      C      Result  satAcc  scope
   ---------------------------------------------------------------
     0   8   8  16   fp16   fp16   fp32   fp32    0      subgroup
     1   8   8  32   uint8  uint8  uint32 uint32  0      subgroup

  bench (one subgroup a workgroup, timed by the GPU):
    matrix 8x8x16, subgroup  8: exact, 57041 GFLOP/s
    matrix 8x16x16: not offered; tried anyway, as NR's kernels run it
    matrix 8x16x16, subgroup 32: exact, 38013 GFLOP/s
    matrix 8x16x16, subgroup 16: WRONG, 61 of 64 values (off by up to 3)
    vector float32 FMA, subgroup 32: 18124 GFLOP/s
    NR's kernels (float16 8x16x16 at subgroup 32): exact here, though the GPU does not offer the shape

=== device 1: Intel(R) Graphics ===
  driver        : Intel Corporation, 101.9150
  -- no KHR cooperative matrix here --
"""

    def test_parse(self):
        devices = P.probe(self.TEXT, issue=25)
        self.assertEqual(len(devices), 1)               # the device without matrix units is left out
        device = devices[0]
        self.assertEqual(device["driver"], "101.9150")
        self.assertEqual(device["offered"], ["8x8x16 fp16 to fp32", "8x8x32 uint8 to uint32"])
        self.assertEqual(device["matrix_gflops"]["8x16x16"]["32"], 38013)
        self.assertTrue(device["matrix_gflops"]["8x16x16"]["16"].startswith("WRONG"))
        self.assertEqual(device["fma32_gflops"], {"32": 18124})
        self.assertTrue(device["nr_kernels"].startswith("exact here"))


class Fit(unittest.TestCase):
    def test_recovers_a_line(self):
        fields = [(320, 320), (512, 320), (768, 448), (640, 384), (832, 512), (1920, 1152)]
        cases = [{"field": list(f), "network_ms": 10 + 50 * f[0] * f[1] / 1e6} for f in fields]
        cases[-1]["network_ms"] = 999                   # above the game fields: left out of the fit
        fixed, slope, worst = P.fit_network(cases)
        self.assertAlmostEqual(fixed, 10, places=9)
        self.assertAlmostEqual(slope, 50, places=9)
        self.assertLess(worst, 1e-9)

    def test_ranges_by_their_middle(self):
        cases = [{"field": [320, 320], "network_ms": [20, 30]}, {"field": [640, 640], "network_ms": 45}]
        fixed, slope, _ = P.fit_network(cases)
        self.assertAlmostEqual(fixed + slope * 0.1024, 25, places=9)

    def test_committed_data(self):
        data = P.load()
        self.assertTrue(data["speed_tests"] and data["probes"])
        for row in P.fits(data):
            self.assertGreater(row["fixed"], 0, row["test"]["source"])
            self.assertGreater(row["slope"], 0, row["test"]["source"])
            self.assertLess(row["worst"], 3, row["test"]["source"])
        text = P.DATA.read_text(encoding="utf-8")
        for private in ("C:\\\\", "Users", "pipe", "PNPDeviceID", "runtime_pid"):
            self.assertNotIn(private, text)


if __name__ == "__main__":
    unittest.main()
