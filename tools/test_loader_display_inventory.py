#!/usr/bin/env python3
"""Build/run host firmware doubles against the real optional loader capture."""
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
    executable = output / ("loader-display-inventory.exe" if os.name == "nt" else "loader-display-inventory")
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    str(root / "tools/tests/loader_display_inventory_test.c"),
                    str(root / "loader/display_inventory.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    main()
