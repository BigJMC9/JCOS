#!/usr/bin/env python3
"""Test the retained reference constrained write/completion executor with semantic IO doubles."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--cc",default="clang");p.add_argument("--build-dir",type=Path,default=Path("build/host-tests"))
a=p.parse_args();root=Path(__file__).resolve().parent.parent
out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
exe=out/("polaris-program.exe" if os.name=="nt" else "polaris-program")
subprocess.run([a.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-Ikernel","-Iinclude",
 "tools/tests/polaris_program_test.c","kernel/polaris_program.c","kernel/polaris_plane.c",
 "user/drivers/amd_polaris_plane.c","kernel/lib.c","-o",str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],check=True)
