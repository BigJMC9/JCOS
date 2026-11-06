#!/usr/bin/env python3
"""Reproduce the focused-client exit observation boundary in the actual test."""
import argparse
import os
from pathlib import Path
import subprocess
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc",default="clang")
    parser.add_argument("--build-dir",type=Path,default=Path("build/host-tests"))
    args=parser.parse_args()
    root=Path(__file__).resolve().parent.parent
    out=args.build_dir.resolve(); out.mkdir(parents=True,exist_ok=True)
    exe=out/("display-surface-wait.exe" if os.name=="nt" else "display-surface-wait")
    subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
        "-ffunction-sections","-fdata-sections","-fno-asynchronous-unwind-tables",
        "-fno-unwind-tables","-Wl,--gc-sections","-I"+str(root/"kernel"),
        "-I"+str(root/"kernel/tests"),"-I"+str(root/"include"),
        str(root/"tools/tests/display_surface_wait_test.c"),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
if __name__=="__main__":
    main()
