#!/usr/bin/env python3
"""Test Ring3 surface-rebind recipes against the independent kernel preview."""
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
    exe=out/("polaris-plane.exe" if os.name=="nt" else "polaris-plane")
    sources=["tools/tests/polaris_plane_test.c","user/drivers/amd_polaris_plane.c",
             "kernel/polaris_plane.c","kernel/lib.c"]
    subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
        "-Ikernel","-Iinclude",*[str(root/p) for p in sources],"-o",str(exe)],cwd=root,check=True)
    subprocess.run([str(exe)],check=True)
if __name__=="__main__":
    main()
