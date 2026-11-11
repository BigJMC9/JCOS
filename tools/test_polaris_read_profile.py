#!/usr/bin/env python3
"""Production finite-capture policy and read-profile evaluator, no device access."""
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
        "polaris-read-profile": ["tools/tests/polaris_read_profile_test.c",
            "kernel/tests/polaris_read_profile_test.c", "kernel/pci.c"],
        "polaris-read-probe": ["tools/tests/polaris_read_probe_test.c"],
    }
    shared = ["kernel/polaris_capture.c", "kernel/polaris_read_profile.c",
        "kernel/polaris_mmio.c", "kernel/polaris_profile.c", "kernel/display_probe.c", "kernel/lib.c"]
    for name, extra in suites.items():
        exe = out / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-fno-asynchronous-unwind-tables",
            "-Ikernel", "-Ikernel/tests", "-Iinclude",
            *[str(root/name) for name in shared+extra], "-Wl,--gc-sections", "-o", str(exe)],
            cwd=root, check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    main()
