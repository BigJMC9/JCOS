"""Exercise the production Ring3 sequence, finite gate and private transport."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/host-tests'))
a=p.parse_args();root=Path(__file__).resolve().parent.parent;out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
common=[a.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Ikernel','-Iinclude']
for name,files in [
 ('polaris-program-gate',['tools/tests/polaris_program_gate_test.c','kernel/polaris_program_gate.c','user/drivers/amd_polaris_program.c']),
 ('polaris-broker-transport',['tools/tests/polaris_broker_transport_test.c','kernel/polaris_broker.c','kernel/polaris_program_gate.c','kernel/polaris_vtotal_gate.c']),
 ('polaris-vtotal-transport',['tools/tests/polaris_vtotal_transport_test.c','kernel/polaris_broker.c','kernel/polaris_program_gate.c','kernel/polaris_vtotal_gate.c']),
]:
 exe=out/(name+('.exe' if os.name=='nt' else ''))
 subprocess.run(common+files+['kernel/polaris_plane.c','user/drivers/amd_polaris_plane.c','kernel/lib.c','-o',str(exe)],cwd=root,check=True)
 subprocess.run([str(exe)],cwd=root,check=True)
