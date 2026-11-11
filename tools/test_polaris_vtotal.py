#!/usr/bin/env python3
"""Exercise production vertical sequencer and independent finite gate with RAM effects."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/vtotal-host'));a=p.parse_args();r=Path(__file__).resolve().parent.parent;o=a.build_dir.resolve();o.mkdir(parents=True,exist_ok=True);exe=o/('polaris-vtotal.exe' if os.name=='nt' else 'polaris-vtotal')
subprocess.run([a.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(r/'include'),'-I'+str(r/'kernel'),str(r/'tools/tests/polaris_vtotal_test.c'),str(r/'user/drivers/amd_polaris_vtotal.c'),str(r/'kernel/polaris_vtotal_gate.c'),str(r/'kernel/lib.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
