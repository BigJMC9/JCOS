#!/usr/bin/env python3
"""Check pinned DCE stream offsets/masks and the copied-only access boundary."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source-dir',type=Path,required=True);p.add_argument('--build-dir',type=Path,default=Path('build/stream-audit'));p.add_argument('--cc',default='clang');a=p.parse_args();root=Path(__file__).resolve().parent.parent
v=json.loads((root/'docs/POLARIS_STREAM_REVIEW.json').read_text());a.build_dir.mkdir(parents=True,exist_ok=True)
for s in v['sources']:assert hashlib.sha256((a.source_dir/s['file']).read_bytes()).hexdigest()==s['sha256'],s['file']
def defines(file):return {m[1]:int(m[2],0) for m in re.finditer(r'^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$',(a.source_dir/file).read_text(),re.M)}
o=defines('dce_11_2_d.h');m=defines('dce_11_2_sh_mask.h')
src=['#include <stdio.h>','#include "polaris_stream_protocol.h"','int main(void){','printf("C %u %u %u\\n",POLARIS_STREAM_FIELDS,POLARIS_STREAM_VALUES,POLARIS_STREAM_WORDS);']
for f in v['field_facts']:src.append('printf("M %u\\n",'+f['compiled']+');')
for f in v['fields']:src.append('{JcosU32 n=0;if(!jcos_polaris_stream_offset('+str(f['selector'])+',0x40000,&n))return 1;printf("R %u\\n",n);}')
src.append('return 0;}');source=a.build_dir/'stream-facts.c';exe=a.build_dir/'stream-facts.exe';source.write_text('\n'.join(src)+'\n')
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'include'),str(source),'-o',str(exe)],check=True)
rows=subprocess.check_output([str(exe)],text=True).splitlines();assert rows.pop(0)=='C 5 96 14'
for f in v['field_facts']:assert rows.pop(0)=='M '+str(m[f['upstream']]),f
for f in v['fields']:assert rows.pop(0)=='R '+str(f['byte_offset']) and o[f['register']]*4==f['byte_offset']
assert not rows and {f['byte_offset']&~4095 for f in v['fields']}=={0x6000,0x13000}
se=(a.source_dir/'dce_stream_encoder.c').read_text();blank=re.search(r'static void dce110_stream_encoder_dp_blank\(.*?\n}\n',se,re.S)[0];unblank=re.search(r'static void dce110_stream_encoder_dp_unblank\(.*?\n}\n',se,re.S)[0]
assert blank.index('REG_WAIT(DP_VID_STREAM_CNTL')<blank.index('REG_UPDATE(DP_STEER_FIFO')
assert unblank.index('REG_UPDATE(DIG_FE_CNTL')<unblank.index('REG_UPDATE(DP_STEER_FIFO')<unblank.index('udelay(100)')<unblank.index('REG_UPDATE(DP_VID_STREAM_CNTL')
tg=(a.source_dir/'dce110_timing_generator.c').read_text();lock=re.search(r'void dce110_timing_generator_set_lock_master\(.*?\n}\n',tg,re.S)[0];assert 'mmCRTC_MASTER_UPDATE_LOCK' in lock and 'dm_read_reg' in lock
assert 'mmCRTC_MASTER_UPDATE_MODE' in tg and 'dce110_timing_generator_construct' in (a.source_dir/'dce112_resource.c').read_text()
mmio=(root/'kernel/polaris_mmio.c').read_text();body=re.search(r'bool polaris_mmio_read_stream_held\(.*?\n}\n',mmio,re.S)[0]
for token in ['held_binding','b->rebind_enabled','link_mapped_pages!=POLARIS_LINK_PAGES','jcos_polaris_stream_offset','read_leaf']:assert token in body,token
assert 'device_mmio_store' not in body and 'vmm_map' not in body and 'vmm_set_writable' not in body
capture=(root/'kernel/polaris_link_capture.c').read_text();assert capture.index('jcos_polaris_stream_read_admitted')<capture.index('polaris_mmio_read_stream_held')
assert capture.index('bool cleaned=release();')<capture.index('polaris_stream_decode_copy')
assert 'if(attempted)' in capture and 'if(!admitted(start,frequency))goto finish;\n  ++reads;' in capture
probe=(root/'user/register_reader_probe.c').read_text();assert 'stream_query>=POLARIS_STREAM_WORDS' in probe and 'request.words[1]!=stream_query' in probe and 'stream_started || vertical_started || timing_started || link_started' in probe
record=dict(result='PASS',source_files=len(v['sources']),compiled_offsets=v['fields'],compiled_field_facts=len(v['field_facts']),existing_pages=[0x6000,0x13000],new_read_registers=3,new_pages=0,new_write_permissions=False,consumer_facts='blank waits for inactive stream before FIFO reset;unblank DIG_START/FIFO clear/100us/enable;masterlock RMW;mode initialization',scope='Source facts and boundary review only;physical new read profile and any future write sequence unqualified')
(a.build_dir/'audit.json').write_text(json.dumps(record,indent=2)+'\n')
print('PINNED POLARIS STREAM AUDIT: PASS;8hashes,5offsets,7mask facts;2existing RO pages;no added writes;source blank/unblank ordering only')