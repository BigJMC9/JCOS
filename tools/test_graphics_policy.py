#!/usr/bin/env python3
"""Exercise the production Ring3 policy and identity-bound client without hardware."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--cc",default="clang")
p.add_argument("--build-dir",type=Path,default=Path("build/host-tests"))
a=p.parse_args();root=Path(__file__).resolve().parent.parent
out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
exe=out/("graphics-policy.exe" if os.name=="nt" else "graphics-policy")
subprocess.run([a.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-Ikernel","-Iinclude",
 "tools/tests/graphics_policy_test.c","user/graphics_policy.c","kernel/graphics_supervisor.c",
 "-o",str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],cwd=root,check=True)
