#!/usr/bin/env python3
"""Exercise production worker retention and supervisor call admission without hardware."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument("--cc",default="clang")
p.add_argument("--build-dir",type=Path,default=Path("build/host-tests"));a=p.parse_args()
root=Path(__file__).resolve().parent.parent;out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
for name,sources in [
 ("serial-worker",["tools/tests/serial_worker_test.c","kernel/serial_worker.c","kernel/lib.c"]),
 ("supervisor-gate",["tools/tests/supervisor_gate_test.c"]),
 ("polaris-watch",["tools/tests/polaris_watch_test.c","kernel/polaris_watch.c"])]:
 exe=out/(name+".exe" if os.name=="nt" else name)
 subprocess.run([a.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-ffunction-sections","-fdata-sections",
  "-fno-asynchronous-unwind-tables","-fno-unwind-tables","-Wl,--gc-sections","-Ikernel","-Iinclude","-DJCOS_POLARIS_NATIVE_WRITE=1",*sources,"-o",str(exe)],cwd=root,check=True)
 subprocess.run([str(exe)],check=True)
