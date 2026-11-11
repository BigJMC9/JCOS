#!/usr/bin/env python3
"""Audit fixed vertical writes, restoration ledgers and pinned set_drr facts."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source-dir',type=Path,required=True);p.add_argument('--build-dir',type=Path,default=Path('build/vtotal-audit'));p.add_argument('--cc',default='clang');a=p.parse_args();root=Path(__file__).resolve().parent.parent
v=json.loads((root/'docs/POLARIS_VTOTAL_REVIEW.json').read_text());a.build_dir.mkdir(parents=True,exist_ok=True)
for s in v['sources']:assert hashlib.sha256((a.source_dir/s['file']).read_bytes()).hexdigest()==s['sha256'],s['file']
def defines(file):return {m[1]:int(m[2],0) for m in re.finditer(r'^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$',(a.source_dir/file).read_text(),re.M)}
o=defines('dce_11_2_d.h');m=defines('dce_11_2_sh_mask.h')
src=['#include <stdio.h>','#include "polaris_vtotal_protocol.h"','#include "polaris_native_protocol.h"','#include "polaris_profile.h"','int main(void){','printf("C %u %u %u\\n",POLARIS_VERTICAL_FIELDS,POLARIS_VERTICAL_VALUES,POLARIS_VERTICAL_WORDS);']
for f in v['field_facts']:src.append('printf("M %u\\n",'+f['compiled']+');')
for f in v['fields']:src.append('{JcosU32 n=0;if(!jcos_polaris_vertical_offset('+str(f['selector'])+',0x40000,&n))return 1;printf("R %u\\n",n);}')
src.append('printf("N %llu %llu\\n",(unsigned long long)POLARIS_NATIVE_VERSION,(unsigned long long)POLARIS_NATIVE_RESCUE_VTOTAL_FAULT);');src.append('const PolarisHardwareProfile *p=polaris_profile_qualified();printf("P %u %u %u %u %u %u %u\\n",p->pipeline.target_vtotal,p->pipeline.vertical_control,p->pipeline.original_vertical[0],p->pipeline.original_vertical[1],p->pipeline.original_vertical[2],p->access.rebind_page,p->pipeline.pipe);');src.append('return 0;}');source=a.build_dir/'vertical-facts.c';exe=a.build_dir/'vertical-facts.exe';source.write_text('\n'.join(src)+'\n')
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'include'),'-I'+str(root/'kernel'),str(source),str(root/'kernel/polaris_profile.c'),str(root/'kernel/lib.c'),'-o',str(exe)],check=True)
rows=subprocess.check_output([str(exe)],text=True).splitlines();assert rows.pop(0)=='C 3 86 18'
for f in v['field_facts']:assert rows.pop(0)=='M '+str(m[f['upstream']]),f
for f in v['fields']:assert rows.pop(0)=='R '+str(f['byte_offset']) and o[f['register']]*4==f['byte_offset']
assert rows.pop(0)=='N 5 13';assert rows.pop(0)=='P 1132 17 0 0 0 24576 0';assert not rows and {f['byte_offset']&~4095 for f in v['fields']}=={0x6000}
tg=(a.source_dir/'dce110_timing_generator.c').read_text();body=re.search(r'void dce110_timing_generator_set_drr\(.*?\n}\n',tg,re.S)[0]
assert re.findall(r'addr = CRTC_REG\(mm(\w+)\);\n\s*v_total_\w+ = dm_read_reg',body)==['CRTC_V_TOTAL_MIN','CRTC_V_TOTAL_MAX','CRTC_V_TOTAL_CONTROL']
assert re.findall(r'addr = CRTC_REG\(mm(\w+)\);\n\s*dm_write_reg',body)==['CRTC_V_TOTAL_MIN','CRTC_V_TOTAL_MAX','CRTC_V_TOTAL_CONTROL']
for name in ['MIN','MAX']:assert re.search(r'params->vertical_total_'+name.lower()+r' - 1,',body)
for name in ['MIN_SEL','MAX_SEL']:assert re.search(r'set_reg_field_value\(v_total_cntl,\s*1,\s*CRTC_V_TOTAL_CONTROL,\s*CRTC_V_TOTAL_'+name+r'\)',body)
assert 'dce110_timing_generator_construct' in (a.source_dir/'dce112_resource.c').read_text()
mmio=(root/'kernel/polaris_mmio.c').read_text()
body=re.search(r'bool polaris_mmio_vtotal_write\(.*?\n}\n',mmio,re.S)[0]
for token in ['b->vtotal_next>=3','b->recovery_armed','PCI_ASSIGNMENT_QUARANTINED','b->rebind_next','b->plan.profile->pipeline.original_vertical[expected]','b->plan.profile->pipeline.target_vtotal','b->plan.profile->pipeline.vertical_control','VM_DEVICE|VM_WRITE','++b->vtotal_next','device_mmio_store32']:assert token in body,token
assert body.index('++b->vtotal_next')<body.index('device_mmio_store32')
assert '(b->vtotal_enabled && b->recovery_armed && b->vtotal_next!=3)' in mmio
assert 'jcos_polaris_vtotal_snapshot_admitted(snapshot,POLARIS_VERTICAL_VALUES)' in mmio
user=(root/'user/drivers/amd_polaris_vtotal.c').read_text();gate=(root/'kernel/polaris_vtotal_gate.c').read_text()
assert '1132' in user and '1132' in gate and '0x11' in user and '0x11' in gate
assert '0xFFFFFFU' in user and '0xFFFFFFU' in gate
assert 'POLARIS_VTOTAL_MESSAGES 458U' in (root/'include/polaris_vtotal_protocol.h').read_text()
broker=(root/'kernel/polaris_broker.c').read_text()
body=re.search(r'static bool vtotal_round\(.*?\n}\n',broker,re.S)[0]
assert body.index('d->vtotal_attempted=true')<body.index('managed_service_send_oneway')
assert body.index('rescue_fault && gate.result.write_attempts>=fault')<body.index('polaris_vtotal_gate_step')
for token in ['7U+fault','complete=!rescue_fault','d->role!=2 || !fault','d->role==2 && fault']:
 assert token in body,token
native=(root/'user/polaris_native.c').read_text()
assert 'POLARIS_NATIVE_RESCUE_VTOTAL_FAULT' in native and 'POLARIS_NATIVE_ROLE==POLARIS_NATIVE_ROLE_RESCUE && q.words[2] && !identity && !attempted' in native
record=dict(result='PASS',source_files=len(v['sources']),compiled_offsets=v['fields'],compiled_field_facts=5,compiled_profile_values=[1132,17,0,0,0,24576,0],pages=[0x6000],diagnostic=dict(native_protocol=5,operation=13,max_reads=6,max_stores=3,max_incoming=10,published_completion=False,shared_vertical_attempt=True,new_registers=False),scope='Pinned write register/mask/frame facts and production store-order/ledger/RO-user boundary checks; no physical write or restoration proof')
(a.build_dir/'audit.json').write_text(json.dumps(record,indent=2)+'\n')
print('PINNED POLARIS VTOTAL AUDIT: PASS;8 hashes,3 offsets,5 masks/frame facts,consumed-before-store ledger,independent gate/user sequencing;rescue-prefix broker cap/protocol5;physical diagnostic pending')
