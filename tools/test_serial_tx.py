#!/usr/bin/env python3
"""Test the actual kernel UART writer using hardware-free port doubles."""
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
    exe = output / ("serial-tx.exe" if os.name == "nt" else "serial-tx")
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    str(root / "tools/tests/serial_tx_test.c"),
                    str(root / "kernel/serial.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    main()
