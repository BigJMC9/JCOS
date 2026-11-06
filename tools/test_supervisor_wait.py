#!/usr/bin/env python3
"""Reproduce terminal-state observation races in the actual supervisor wait."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--managed", action="store_true", help="Exercise the managed-service wait")
    parser.add_argument("--build-dir", type=Path, default=Path("build/host-tests"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stem = "managed-service-wait" if args.managed else "supervisor-wait"
    exe = output / (stem + ".exe" if os.name == "nt" else stem)
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    *(["-DTEST_MANAGED_SERVICE_WAIT"] if args.managed else []),
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
                    str(root / "tools/tests/supervisor_wait_test.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
