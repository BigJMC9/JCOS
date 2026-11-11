#!/usr/bin/env python3
"""Run the production native orchestration with stateful hardware/service doubles."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument("--cc",default="clang");p.add_argument("--build-dir",type=Path,default=Path("build/host-tests"))
a=p.parse_args();root=Path(__file__).resolve().parent.parent;out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
exe=out/("polaris-native-controller.exe" if os.name=="nt" else "polaris-native-controller")
subprocess.run([a.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-DJCOS_POLARIS_NATIVE_WRITE=1","-Ikernel","-Iinclude",
 "tools/tests/polaris_native_controller_test.c","kernel/polaris_native.c","kernel/graphics_supervisor.c","user/graphics_policy.c","kernel/polaris_program.c","kernel/polaris_plane.c",
 "kernel/polaris_vtotal_gate.c","user/drivers/amd_polaris_vtotal.c","user/drivers/amd_polaris_vertical.c","user/drivers/amd_polaris_timing.c","user/drivers/amd_polaris_link.c","kernel/polaris_vram.c","kernel/polaris_observation.c","user/drivers/amd_polaris_plane.c","kernel/lib.c","-o",str(exe)],cwd=root,check=True)
for case in range(189):
    r=subprocess.run([str(exe),str(case)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    if r.returncode: print(r.stdout);r.check_returncode()
print("PASS: production native controller including24 vertical-mode cases;happy path,35 failures,2 watchdog and3 power-cleanup cases,16 heartbeat/diagnostic/progress and18 Ring3 policy/transport/identity/unsafe-action and5 core policy-failure injection and16 supervised GUI/deferred-rescue and8 terminal CPU-drain and18 explicit GUI reconnect/unsafe-CONNECT and14 normal native launch and29 retained primary restart cases; retirement/order,exclusive writers,one reserved rescue,quarantine,no rearm and retained failed cleanup; no hardware.")

# The controller uses service doubles; separately execute the real RPC helpers
# with corrupt reply envelopes, ACK failures and unrelated-service witnesses.
driver=out/("polaris-driver-liveness.exe" if os.name=="nt" else "polaris-driver-liveness")
subprocess.run([a.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-Ikernel","-Iinclude",
 "tools/tests/polaris_driver_liveness_test.c","kernel/polaris_driver.c","kernel/polaris_plane.c",
 "user/drivers/amd_polaris_plane.c","kernel/lib.c","-o",str(driver)],cwd=root,check=True)
subprocess.run([str(driver)],cwd=root,check=True)
