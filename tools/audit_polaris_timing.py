#!/usr/bin/env python3
"""Audit fixed timing offsets/masks against pinned definitions and consumers."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source-dir',type=Path,required=True);p.add_argument('--build-dir',type=Path,default=Path('build/timing-audit'));p.add_argument('--cc',default='clang');a=p.parse_args();root=Path(__file__).resolve().parent.parent
v=json.loads((root/'docs/POLARIS_TIMING_REVIEW.json').read_text());a.build_dir.mkdir(parents=True,exist_ok=True)
for s in v['sources']:assert hashlib.sha256((a.source_dir/s['file']).read_bytes()).hexdigest()==s['sha256'],s['file']
def defines(file):
 text=(a.source_dir/file).read_text();return {m[1]:int(m[2],0) for m in re.finditer(r'^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$',text,re.M)}
o=defines('dce_11_2_d.h');m=defines('dce_11_2_sh_mask.h')
src=['#include <stdio.h>','#include "polaris_timing_protocol.h"','int main(void){','printf("C %u %u %u\\n",POLARIS_TIMING_FIELDS,POLARIS_TIMING_VALUES,POLARIS_TIMING_WORDS);']
for f in v['field_facts']:src.append('printf("M '+f['compiled']+' %u\\n",'+f['compiled']+');')
for f in v['fields']:src.append('{JcosU32 n=0;if(!jcos_polaris_timing_offset('+str(f['selector'])+',0x40000,&n))return 1;printf("R '+str(f['selector'])+' %u\\n",n);}')
src.append('return 0;}');source=a.build_dir/'timing-facts.c';exe=a.build_dir/'timing-facts.exe';source.write_text('\n'.join(src)+'\n')
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'include'),str(source),'-o',str(exe)],check=True)
rows=subprocess.check_output([str(exe)],text=True).splitlines();assert rows.pop(0)=='C 14 80 9'
for f in v['field_facts']:
 fields=rows.pop(0).split();assert fields[:2]==['M',f['compiled']] and int(fields[2])==m[f['upstream']],f
pages=set();offsets=[]
for f in v['fields']:
 fields=rows.pop(0).split();offset=o[f['register']]*4;assert fields==['R',str(f['selector']),str(offset)];pages.add(offset&~4095);offsets.append(dict(field=f['selector'],register=f['register'],offset=offset))
assert not rows and sorted(pages)==v['pages']==[0x6000,0x13000]
tg=(a.source_dir/'dce110_timing_generator.c').read_text();stream=(a.source_dir/'dce_stream_encoder.c').read_text()
blank=re.search(r'void dce110_timing_generator_program_blanking\(.*?\n}\n',tg,re.S)[0]
for name in ['CRTC_H_TOTAL','CRTC_V_TOTAL','CRTC_H_BLANK_START_END','CRTC_V_BLANK_START_END']:assert 'CRTC_REG(mm'+name+')' in blank and name in blank
assert re.search(r'timing->h_total\s*-\s*1',blank) and re.search(r'timing->v_total\s*-\s*1',blank)
assert 'timing->h_addressable' in blank and 'timing->v_addressable' in blank
assert re.search(r'dm_read_reg\(tg->ctx,\s*CRTC_REG\(mmCRTC_V_SYNC_A_CNTL\)\)',tg)
assert 'REG_GET(DP_VID_STREAM_CNTL, DP_VID_STREAM_ENABLE, &reg1)' in stream
for name in ['DP_PIXEL_FORMAT','DP_VID_TIMING','DP_VID_N','DP_VID_M']:assert re.search(r'REG_UPDATE(?:_\d)?\('+name+r',',stream),name
(a.build_dir/'audit.json').write_text(json.dumps(dict(result='PASS',source_files=len(v['sources']),compiled_offsets=offsets,compiled_field_facts=len(v['field_facts']),pages=sorted(pages),scope='Pinned facts/consumer use support narrow operational-domain experiment;physical read completion/power/reset/hang safety pending'),indent=2)+'\n')
print('PINNED POLARIS TIMING AUDIT: PASS;eight source hashes,14compiled offsets/20field facts/two existing pages,consumer total-minus-one/blank/read/RMW facts;physical pending')
