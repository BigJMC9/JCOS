#!/usr/bin/env python3
"""Exercise production BAR5 preparation/rollback and strict device-memory PTEs."""
import argparse
import os
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--build-dir", type=Path, default=Path("build/host-tests"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    suites = {
        "polaris-mmio": ["tools/tests/polaris_mmio_test.c", "kernel/polaris_mmio.c",
            "kernel/display_probe.c", "kernel/tests/polaris_mmio_plan_test.c", "kernel/physmap.c"],
        "vmm-device": ["tools/tests/vmm_device_test.c", "kernel/vmm.c"],
        "polaris-live-controller": ["tools/tests/polaris_live_capture_test.c",
            "kernel/polaris_live_capture.c", "kernel/polaris_observation.c", "kernel/polaris_vram.c", "kernel/polaris_plane.c"],
        "polaris-vram": ["tools/tests/polaris_vram_test.c", "kernel/polaris_vram.c"],
        "polaris-observation": ["tools/tests/polaris_observation_test.c", "kernel/polaris_observation.c"],
    }
    for name, sources in suites.items():
        exe = out / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-fno-asynchronous-unwind-tables",
            "-Ikernel", "-Ikernel/tests", "-Iinclude",
            *(["-DJCOS_POLARIS_LIVE_CAPTURE=1"] if name == "polaris-live-controller" else []),
            *[str(root / path) for path in sources], str(root / "kernel/polaris_profile.c"), str(root / "kernel/lib.c"),
            "-Wl,--gc-sections", "-o", str(exe)], cwd=root, check=True)
        subprocess.run([str(exe)], check=True)
        if name == "polaris-mmio":
            subprocess.run([str(exe), "poison"], check=True)
            subprocess.run([str(exe), "location-poison"], check=True)
        if name == "polaris-live-controller":
            subprocess.run([str(exe), "pending-mode"], check=True)
            for case in range(1, 104):
                subprocess.run([str(exe), str(case)], check=True)
            print("PASS: production live controller, zero/pending-mode success and 103 refusal/failure boundaries; retained quarantine, incomplete ledgers, no retry or device access.")

if __name__ == "__main__":
    main()
