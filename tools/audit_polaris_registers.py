#!/usr/bin/env python3
"""Offline comparison of compiled register facts with exact pinned AMD sources.
No download, device access, access-type inference or qualification is performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

def checked_bytes(data, expected, label):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError("source SHA-256 mismatch: " + label)
    return data

def numeric_definitions(data):
    result = {}
    for number, line in enumerate(data.decode("utf-8").splitlines(), 1):
        match = re.match(r"^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$", line)
        if not match:
            match = re.match(r"^\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+),?\s*$", line)
        if not match:
            continue
        name, literal = match.groups()
        value = int(literal, 0)
        if name in result and result[name]["value"] != value:
            raise ValueError("conflicting upstream definition: " + name)
        result[name] = {"value": value, "line": number}
    return result

def compare(contract, offsets, masks, rows):
    fields = contract["fields"]
    count = rows.pop("C", None)
    if count != [6, len(fields)]:
        raise ValueError("compiled pipe/selector count mismatch")
    result = []
    pages = set()
    for selector, field in enumerate(fields):
        if rows.pop(("S", selector), None) != selector:
            raise ValueError("compiled semantic selector mismatch: " + field["selector"])
    for pipe in range(6):
        for selector, field in enumerate(fields):
            key = ("R", pipe, selector)
            actual = rows.pop(key, None)
            macro = "mm" + field["family"] + str(pipe) + "_" + field["register"]
            source = offsets.get(macro)
            if not source or actual != source["value"] * 4:
                raise ValueError("register offset mismatch: " + macro)
            if actual % 4 or actual + 4 > 0x40000:
                raise ValueError("register offset outside BAR5 plan")
            pages.add(actual & ~4095)
            result.append({"pipe": pipe, "selector": field["selector"], "byte_offset": actual,
                "upstream_macro": macro, "upstream_line": source["line"]})
    checked_masks = []
    for mask in contract["masks"]:
        local = mask["local"]
        source_name = mask.get("source", contract["mask_source"])
        source = masks.get(source_name, {}).get(mask["upstream"])
        scale = mask.get("scale", 1)
        if scale not in (1, 4):
            raise ValueError("unsupported field scale")
        actual = rows.pop(("M", local), None)
        if not source or actual != source["value"] * scale:
            raise ValueError("field mask/shift mismatch: " + local)
        checked_masks.append({"local": local, "value": actual, "upstream_macro": mask["upstream"],
                              "upstream_line": source["line"], "source": source_name, "scale": scale})
    if rows or pages != {0x6000, 0x7000, 0x10000, 0x11000}:
        raise ValueError("unexpected compiled rows or register page set")
    return result, checked_masks, sorted(pages)

def read_rows(output):
    result = {}
    for line in output.splitlines():
        words = line.split()
        if len(words) == 3 and words[0] == "C":
            key, value = "C", [int(words[1]), int(words[2])]
        elif len(words) == 4 and words[0] == "R":
            key, value = ("R", int(words[1]), int(words[2])), int(words[3])
        elif len(words) == 3 and words[0] == "S":
            key, value = ("S", int(words[1])), int(words[2])
        elif len(words) == 3 and words[0] == "M":
            key, value = ("M", words[1]), int(words[2])
        else:
            raise ValueError("invalid compiled probe row")
        if key in result:
            raise ValueError("duplicate compiled probe row")
        result[key] = value
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--contract", type=Path, default=Path("docs/POLARIS_REGISTER_REVIEW.json"))
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--build-dir", type=Path, default=Path("build/polaris-audit"))
    parser.add_argument("--check-refusals", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    if contract["schema_version"] != 1 or not re.fullmatch("[0-9a-f]{40}", contract["upstream"]["commit"]):
        raise ValueError("unsupported source contract")
    sources = {}
    for source in contract["sources"]:
        name = source["file"]
        if name != Path(source["path"]).name or Path(name).name != name or name in sources:
            raise ValueError("invalid/duplicate source filename")
        sources[name] = checked_bytes((args.source_dir/name).read_bytes(), source["sha256"], name)
    offsets = numeric_definitions(sources[contract["offset_source"]])
    masks = {name: numeric_definitions(data) for name, data in sources.items()}
    source = ['#include <stdio.h>', '#include "amd_polaris_registers.h"', 'int main(void) {',
        'printf("C %u %u\\n", POLARIS_DISPLAY_PIPE_COUNT, POLARIS_DISPLAY_REGISTER_COUNT);',
        'for (unsigned p=0;p<POLARIS_DISPLAY_PIPE_COUNT;++p) {',
        'for (unsigned f=0;f<POLARIS_DISPLAY_REGISTER_COUNT;++f) {',
        'JcosU32 offset=0; if (!jcos_polaris_register_offset(p,(PolarisDisplayRegister)f,0x40000ULL,&offset)) return 2;',
        'printf("R %u %u %u\\n",p,f,offset); }}']
    for index, field in enumerate(contract["fields"]):
        if not IDENTIFIER.fullmatch(field["selector"]) or not field["selector"].startswith("POLARIS_REG_"):
            raise ValueError("invalid semantic selector name")
        source.append('printf("S '+str(index)+' %u\\n", (unsigned)'+field["selector"]+');')
        if field["family"] not in ("CRTC", "DCP") or not IDENTIFIER.fullmatch(field["register"]):
            raise ValueError("invalid source register name")
    for mask in contract["masks"]:
        local = mask["local"]
        if not IDENTIFIER.fullmatch(local) or not local.startswith("POLARIS_"):
            raise ValueError("invalid local field name")
        source.append('printf("M '+local+' %u\\n", (unsigned)'+local+');')
    source += ['return 0;', '}']
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    probe = out/"polaris-register-facts.c"
    exe = out/("polaris-register-facts.exe" if os.name == "nt" else "polaris-register-facts")
    probe.write_text("\n".join(source)+"\n", encoding="utf-8")
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I"+str(root/"include"), str(probe), "-o", str(exe)], check=True)
    output = subprocess.check_output([str(exe)], text=True)
    rows = read_rows(output)
    registers, checked_masks, pages = compare(contract, offsets, masks, dict(rows))
    refusals = []
    if args.check_refusals:
        def must_refuse(label, action):
            try:
                action()
            except ValueError:
                refusals.append(label)
            else:
                raise AssertionError("audit accepted " + label)
        item = contract["sources"][0]
        must_refuse("changed upstream source bytes", lambda: checked_bytes(
            sources[item["file"]]+b" ", item["sha256"], item["file"]))
        bad = dict(rows); bad[("R", 5, 7)] += 4
        must_refuse("changed compiled register offset", lambda: compare(contract, offsets, masks, bad))
        bad = dict(rows); bad[("S", 0)] = 1
        must_refuse("changed compiled semantic selector", lambda: compare(contract, offsets, masks, bad))
        bad = dict(rows); bad[("M", contract["masks"][0]["local"])] ^= 1
        must_refuse("changed compiled field mask", lambda: compare(contract, offsets, masks, bad))
        for local in ("POLARIS_ARRAY_LINEAR_ALIGNED", "POLARIS_MC_VM_FB_LOCATION_BYTE"):
            bad = dict(rows); bad[("M", local)] ^= 1
            must_refuse("changed enum/location fact " + local,
                        lambda bad=bad: compare(contract, offsets, masks, bad))
        must_refuse("duplicate compiled row", lambda: read_rows(output + output.splitlines()[0]+"\n"))
    report = {"result": "PASS", "scope": contract["scope"], "upstream": contract["upstream"],
        "contract_sha256": hashlib.sha256(args.contract.read_bytes()).hexdigest(),
        "source_files_sha256": {s["file"]: s["sha256"] for s in contract["sources"]},
        "compiled_registers": registers, "compiled_masks_and_shifts": checked_masks,
        "register_pages": pages, "refusal_checks": refusals,
        "physical_access": "UNQUALIFIED; no device access or authorization",
        "unresolved": contract["unresolved"]}
    (out/"register-audit.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    (out/"compiled-register-facts.txt").write_text(output, encoding="utf-8")
    print("PASS: "+str(len(registers))+" compiled offsets, "+str(len(checked_masks))+
        " field/enum/location facts, four pipe register pages, "+str(len(sources))+" exact upstream sources; "+
        str(len(refusals))+" refusal checks. Physical reads remain unqualified.")

if __name__ == "__main__":
    main()
