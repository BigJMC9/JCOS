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
    }
    for name, sources in suites.items():
        exe = out / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-fno-asynchronous-unwind-tables",
            "-Ikernel", "-Ikernel/tests", "-Iinclude",
            *[str(root / path) for path in sources], str(root / "kernel/lib.c"),
            "-Wl,--gc-sections", "-o", str(exe)], cwd=root, check=True)
        subprocess.run([str(exe)], check=True)
        if name == "polaris-mmio":
            subprocess.run([str(exe), "poison"], check=True)

if __name__ == "__main__":
    main()
