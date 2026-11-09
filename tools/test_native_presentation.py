"""Generation-bound normal display service with real output/lease code."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,default=Path('build/host-tests'))
a=p.parse_args();root=Path(__file__).resolve().parent.parent;out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
exe=out/('native-presentation.exe' if os.name=='nt' else 'native-presentation')
subprocess.run([a.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
 'tools/tests/native_display_service_test.c','kernel/display_service.c','kernel/framebuffer.c','kernel/display.c','kernel/terminal.c','kernel/font8x8.c','kernel/lib.c',
 '-Wl,--gc-sections','-o',str(exe)],cwd=root,check=True)
for scenario in range(9):subprocess.run([str(exe),str(scenario)],cwd=root,check=True)

# Compile the actual GUI ticket code;unused graphics/hardware paths are discarded.
ticket=out/('gui-reconnect-ticket.exe' if os.name=='nt' else 'gui-reconnect-ticket')
subprocess.run([a.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
 '-fno-asynchronous-unwind-tables','-fno-unwind-tables','-Ikernel','-Iinclude','tools/tests/gui_reconnect_ticket_test.c','-Wl,--gc-sections','-o',str(ticket)],cwd=root,check=True)
subprocess.run([str(ticket)],cwd=root,check=True)
