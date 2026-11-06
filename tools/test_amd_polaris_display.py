#!/usr/bin/env python3
"""Test the actual userspace Polaris display module using register fixtures."""
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
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = output / ("amd-polaris-display.exe" if os.name == "nt" else "amd-polaris-display")
    sources = ["tools/tests/amd_polaris_display_test.c", "user/drivers/amd_polaris_display.c",
               "user/tests/amd_polaris_display_test.c"]
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    *[str(root / name) for name in sources], "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
