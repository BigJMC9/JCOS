#!/usr/bin/env python3
"""Offline audit of compiled DIG facts against pinned AMD definitions/getters."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source-dir',type=Path,required=True);p.add_argument('--build-dir',type=Path,default=Path('build/link-audit'));p.add_argument('--cc',default='clang');a=p.parse_args();root=Path(__file__).resolve().parent.parent
v=json.loads((root/'docs/POLARIS_LINK_REVIEW.json').read_text());a.build_dir.mkdir(parents=True,exist_ok=True)
for s in v['sources']:assert hashlib.sha256((a.source_dir/s['file']).read_bytes()).hexdigest()==s['sha256'],s['file']
def defines(file):
 text=(a.source_dir/file).read_text();return {m[1]:int(m[2],0) for m in re.finditer(r'^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$',text,re.M)}
o=defines('dce_11_2_d.h');m=defines('dce_11_2_sh_mask.h')
src=['#include <stdio.h>','#include "polaris_link_protocol.h"','int main(void){','printf("C %u %u %u %u\\n",POLARIS_LINK_BACKENDS,POLARIS_LINK_FRONTENDS,POLARIS_LINK_VALUES,POLARIS_LINK_PAGES);']
for fact in v['field_facts']:src.append('printf("M '+fact['compiled']+' %u\\n",'+fact['compiled']+');')
for field,x in enumerate(v['fields']):
 for unit in range(x['units']):src.append(f'{{JcosU32 n=0;if(!jcos_polaris_link_offset({unit},{x["selector"]},0x40000,&n))return 1;printf("R {field} {unit} %u\\n",n);}}')
src.append('return 0;}');source=a.build_dir/'link-facts.c';exe=a.build_dir/'link-facts.exe';source.write_text('\n'.join(src)+'\n')
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'include'),str(source),'-o',str(exe)],check=True)
rows=subprocess.check_output([str(exe)],text=True).splitlines();assert rows.pop(0)=='C 7 6 52 3'
for fact in v['field_facts']:
 fields=rows.pop(0).split();assert fields[:2]==['M',fact['compiled']] and int(fields[2])==m[fact['upstream']],fact
pages=set();offsets=[]
for field,x in enumerate(v['fields']):
 for unit in range(x['units']):
  fields=rows.pop(0).split();offset=o['mmDIG'+str(unit)+'_'+x['register']]*4
  assert fields==['R',str(field),str(unit),str(offset)];pages.add(offset&~4095);offsets.append(dict(unit=unit,field=field,offset=offset))
assert not rows and sorted(pages)==v['pages']
getters=(a.source_dir/'dce_link_encoder.c').read_text();streams=(a.source_dir/'dce_stream_encoder.c').read_text();resources=(a.source_dir/'dce112_resource.c').read_text()
assert re.search(r'unsigned int dce110_get_dig_frontend\(.*?REG_GET\(DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, &value\)',getters,re.S)
assert re.search(r'bool dce110_is_dig_enabled\(.*?REG_GET\(DIG_BE_EN_CNTL, DIG_ENABLE, &value\)',getters,re.S)
assert re.search(r'static unsigned int dig_source_otg\(.*?REG_GET\(DIG_FE_CNTL, DIG_SOURCE_SELECT, &tg_inst\)',streams,re.S)
links=re.search(r'link_enc_regs\[\] = \{(.*?)\};',resources,re.S)[1];front=re.search(r'stream_enc_regs\[\] = \{(.*?)\};',resources,re.S)[1]
assert re.findall(r'link_regs\((\d)\)',links)==list('0123456') and re.findall(r'stream_enc_regs\((\d)\)',front)==list('012345')
(a.build_dir/'audit.json').write_text(json.dumps(dict(result='PASS',source_files=len(v['sources']),compiled_offsets=offsets,compiled_masks=len(v['field_facts']),pages=sorted(pages),scope='Facts and upstream getter usage only;not physical read/power/hang safety qualification'),indent=2)+'\n')
print('PINNED POLARIS LINK AUDIT: PASS;seven source hashes,20compiled offsets/nine masks/three pages,seven backend/six frontend resources and getter usage;physical qualification pending')