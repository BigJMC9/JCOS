#!/usr/bin/env python3
"""Run production copied-stream decoder, refusal and protected-boundary tests."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/stream-host'));a=p.parse_args();r=Path(__file__).resolve().parent.parent;o=a.build_dir.resolve();o.mkdir(parents=True,exist_ok=True);exe=o/('polaris-stream.exe' if os.name=='nt' else 'polaris-stream')
subprocess.run([a.cc,'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-O2','-I'+str(r/'include'),str(r/'tools/tests/polaris_stream_test.c'),str(r/'user/drivers/amd_polaris_stream.c'),str(r/'user/drivers/amd_polaris_link.c'),str(r/'user/drivers/amd_polaris_timing.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
