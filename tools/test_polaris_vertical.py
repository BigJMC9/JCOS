#!/usr/bin/env python3
"""Run copied vertical plan/refusal and interrupted-prefix restoration tests in RAM."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/vertical-host'));a=p.parse_args();r=Path(__file__).resolve().parent.parent;o=a.build_dir.resolve();o.mkdir(parents=True,exist_ok=True);exe=o/('polaris-vertical.exe' if os.name=='nt' else 'polaris-vertical')
subprocess.run([a.cc,'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-O2','-I'+str(r/'include'),str(r/'tools/tests/polaris_vertical_test.c'),str(r/'user/drivers/amd_polaris_link.c'),str(r/'user/drivers/amd_polaris_timing.c'),str(r/'user/drivers/amd_polaris_vertical.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
