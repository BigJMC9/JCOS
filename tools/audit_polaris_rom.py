#!/usr/bin/env python3
"""Audit pinned ATOM structure facts; caller supplies the upstream cache."""
import argparse,hashlib,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source-dir',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parent.parent
review=json.loads((root/'docs/POLARIS_ROM_REVIEW.json').read_text())
for source in review['sources']:
 data=(a.source_dir/source['file']).read_bytes();assert hashlib.sha256(data).hexdigest()==source['sha256'],source['file']
s=re.sub(r'/\*.*?\*/|//[^\n]*','',(a.source_dir/'atombios.h').read_text(),flags=re.S)
def block(name):
 m=re.search(r'typedef\s+struct\s+_'+name+r'\s*\{(.*?)\}\s*'+name+r'\s*;',s,re.S);assert m,name
 return re.sub(r'/\*.*?\*/|//[^\n]*','',m[1],flags=re.S)
def fields(name):return re.findall(r'(ATOM_COMMON_TABLE_HEADER|USHORT|UCHAR)\s+(\w+)(?:\[\d+\])?\s*;',block(name))
facts=review['facts'];rom=fields('ATOM_ROM_HEADER');assert rom[0]==('ATOM_COMMON_TABLE_HEADER','sHeader')
# The only ROM array before the master pointer is the four-byte signature.
assert 'UCHAR  uaFirmWareSignature[4]' in block('ATOM_ROM_HEADER')
offset=0
for typ,name in rom:
 if name=='usMasterDataTableOffset':break
 offset+=4 if typ=='ATOM_COMMON_TABLE_HEADER' or name=='uaFirmWareSignature' else 2 if typ=='USHORT' else 1
assert offset==facts['atom_master_data_offset']==32
master=fields('ATOM_MASTER_LIST_OF_DATA_TABLES');assert master[facts['master_object_entry']]==('USHORT','Object_Header')
for name,length in [('ATOM_OBJECT_HEADER',16),('ATOM_OBJECT_HEADER_V3',18)]:
 fs=fields(name);assert fs[2]==('USHORT','usConnectorObjectTableOffset') and sum(4 if t=='ATOM_COMMON_TABLE_HEADER' else 2 if t=='USHORT' else 1 for t,n in fs)==length
assert fields('ATOM_OBJECT')==[('USHORT','usObjectID'),('USHORT','usSrcDstTableOffset'),('USHORT','usRecordOffset'),('USHORT','usReserved')]
obj=(a.source_dir/'ObjectID.h').read_text()
for name,value in [('GRAPH_OBJECT_TYPE_CONNECTOR',3),('CONNECTOR_OBJECT_ID_HDMI_TYPE_A',12),('CONNECTOR_OBJECT_ID_DISPLAYPORT',19),('RESERVED1_ID_MASK',0x800),('RESERVED2_ID_MASK',0x8000)]:
 m=re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)',obj,re.M);assert m and int(m[1],16)==value,name
assert re.search(r'#define\s+ATOM_ROM_TABLE_PTR\s+0x48',(a.source_dir/'atom.h').read_text())
print('PINNED POLARIS ROM FACT AUDIT: PASS;four source hashes,ROM/data/object offsets,object sizes,DP/HDMI IDs and reserved bits')
