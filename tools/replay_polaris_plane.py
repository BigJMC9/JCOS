#!/usr/bin/env python3
"""Replay a profile3 serial read journal through the real decoder/planner/validator.
This reads a host file and compiles a scalar fixture. It opens no device or port.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-log",type=Path,required=True)
    parser.add_argument("--build-dir",type=Path,default=Path("build/polaris-replay"))
    parser.add_argument("--cc",default="clang")
    parser.add_argument("--policy-source-dir",type=Path,help="Optional preserved before-fix C sources")
    parser.add_argument("--expect-refusal",action="store_true")
    args=parser.parse_args()
    root=Path(__file__).resolve().parent.parent
    raw=args.raw_log.read_bytes()
    text=raw.decode("ascii")
    marker="POLARIS INHERITED-SCANOUT CAPTURE / EXPERIMENTAL BUILD PROFILE 3"
    if text.count(marker)!=1:
        raise ValueError("exactly one profile3 capture required")
    capture=text.split(marker,1)[1]
    rows=re.findall(r"GPU READ #(\d+) (?:(?:PIPE=(\d+) (FIELD|WITNESS)=(\d+))|LOCATION=(\d+))\r?\n  VALUE=(0x[0-9A-F]+)",capture)
    if len(rows)!=46 or [int(r[0]) for r in rows]!=list(range(1,47)):
        raise ValueError("complete ordered46-load journal required")
    records=[(int(p) if p else None,k if k else "LOCATION",int(f or g),int(v,16)) for n,p,k,f,g,v in rows]
    if any(r[3]==0xFFFFFFFF for r in records):
        raise ValueError("absent register")
    if [(p,k,f) for p,k,f,v in records[:6]]!=[(p,"FIELD",0) for p in range(6)]:
        raise ValueError("control discovery ordering")
    pipe=records[11][0]
    expected=[(pipe,"FIELD",f) for f in range(15)]
    if pipe not in range(6) or [(p,k,f) for p,k,f,v in records[11:26]]!=expected or [(p,k,f) for p,k,f,v in records[26:41]]!=expected:
        raise ValueError("pipe sample ordering")
    values=[v for p,k,f,v in records[11:26]]
    if values!=[v for p,k,f,v in records[26:41]]:
        raise ValueError("changed pipe sample")
    for start in (6,44):
        if [(p,k,f) for p,k,f,v in records[start:start+2]]!=[(None,"LOCATION",0),(None,"LOCATION",1)]:
            raise ValueError("location ordering")
    locations=[[v for p,k,f,v in records[start:start+2]] for start in (6,44)]
    if locations[0]!=locations[1]:
        raise ValueError("changed location sample")
    witnesses=[]
    for start in (8,41):
        if [(p,k,f) for p,k,f,v in records[start:start+3]]!=[(pipe,"WITNESS",f) for f in range(3)]:
            raise ValueError("witness ordering")
        witnesses.append([v for p,k,f,v in records[start:start+3]])
    out=args.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    vector=lambda row: "{"+",".join(hex(v)+"U" for v in row)+"}"
    (out/"polaris_replay_fixture.h").write_text(
        "/* Derived from raw SHA256 "+hashlib.sha256(raw).hexdigest()+"; copied data only. */\n"+
        "static const unsigned replay_pipe="+str(pipe)+";\n"+
        "static const u32 replay_controls[6]="+vector([v for p,k,f,v in records[:6]])+";\n"+
        "static const u32 replay_values[15]="+vector(values)+";\n"+
        "static const u32 replay_location[2][2]={"+",".join(map(vector,locations))+"};\n"+
        "static const PolarisPlaneWitness replay_witness={{"+",".join(map(vector,witnesses))+"}};\n",encoding="utf-8")
    policy=args.policy_source_dir.resolve() if args.policy_source_dir else None
    sources=[root/"tools/tests/polaris_plane_replay_test.c",root/"user/drivers/amd_polaris_display.c",
        policy/"amd_polaris_plane.c" if policy else root/"user/drivers/amd_polaris_plane.c",
        policy/"polaris_plane.c" if policy else root/"kernel/polaris_plane.c",root/"kernel/polaris_vram.c",root/"kernel/polaris_profile.c",root/"kernel/polaris_observation.c",root/"kernel/lib.c"]
    exe=out/("polaris-replay.exe" if os.name=="nt" else "polaris-replay")
    subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror","-Ikernel","-Iinclude","-Iuser/drivers","-I"+str(out),
        *map(str,sources),"-o",str(exe)],cwd=root,check=True)
    subprocess.run([str(exe),*(["--expect-refusal"] if args.expect_refusal else [])],check=True)
    (out/"replay.json").write_text(json.dumps(dict(raw_sha256=hashlib.sha256(raw).hexdigest(),pipe=pipe,
        witnesses=witnesses,expect_refusal=args.expect_refusal,result="PASS",
        scope="Recorded-data host replay only; not a new physical read, recipe IPC or GPU write"),indent=2)+"\n",encoding="utf-8")
if __name__=="__main__":
    main()
