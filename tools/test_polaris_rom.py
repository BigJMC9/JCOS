#!/usr/bin/env python3
"""Compile/run the production userspace ROM parser with guarded RAM fixtures."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/host-tests'));a=p.parse_args()
r=Path(__file__).resolve().parent.parent;o=a.build_dir.resolve();o.mkdir(parents=True,exist_ok=True);exe=o/('polaris-rom.exe' if os.name=='nt' else 'polaris-rom')
subprocess.run([a.cc,'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-O2',str(r/'tools/tests/polaris_rom_test.c'),str(r/'user/drivers/amd_polaris_rom.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
