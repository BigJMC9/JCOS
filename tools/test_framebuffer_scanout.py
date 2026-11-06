#!/usr/bin/env python3
"""Exercise the production framebuffer, display lease and terminal without a GPU."""
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
    exe = output / ("framebuffer-scanout.exe" if os.name == "nt" else "framebuffer-scanout")
    sources = ["tools/tests/framebuffer_scanout_test.c", "kernel/framebuffer.c",
               "kernel/display.c", "kernel/terminal.c", "kernel/lib.c", "kernel/font8x8.c"]
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    *[str(root / name) for name in sources], "-o", str(exe)], check=True)
    for scenario in ("idle", "preempt", "present"):
        subprocess.run([str(exe), scenario], check=True)


if __name__ == "__main__":
    main()
