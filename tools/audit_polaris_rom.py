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
assert master[facts['master_i2c_entry']]==('USHORT','GPIO_I2C_Info')
assert master[facts['master_gpio_pin_entry']]==('USHORT','GPIO_Pin_LUT')
assert fields('ATOM_COMMON_RECORD_HEADER')==[('UCHAR','ucRecordType'),('UCHAR','ucRecordSize')]
assert fields('ATOM_HPD_INT_RECORD')==[('UCHAR','ucHPDIntGPIOID'),('UCHAR','ucPlugged_PinState')]
assert fields('ATOM_GPIO_PIN_ASSIGNMENT')==[('USHORT','usGpioPin_AIndex'),('UCHAR','ucGpioPinBitShift'),('UCHAR','ucGPIO_ID')]
assignment=re.findall(r'(USHORT|UCHAR|ATOM_I2C_ID_CONFIG_ACCESS)\s+(\w+)\s*;',block('ATOM_GPIO_I2C_ASSIGMENT'))
assert len(assignment)==19 and assignment[8]==('ATOM_I2C_ID_CONFIG_ACCESS','sucI2cId')
assert sum(2 if t=='USHORT' else 1 for t,n in assignment)==27
assert [n for t,n in assignment[9:17]]==['ucClkMaskShift','ucClkEnShift','ucClkY_Shift','ucClkA_Shift','ucDataMaskShift','ucDataEnShift','ucDataY_Shift','ucDataA_Shift']
assert 'ATOM_I2C_ID_CONFIG          sucI2cId' in block('ATOM_I2C_RECORD')
for name,value in [('ATOM_I2C_RECORD_TYPE',1),('ATOM_HPD_INT_RECORD_TYPE',2)]:
 m=re.search(r'^#define\s+'+name+r'\s+(\d+)',s,re.M);assert m and int(m[1])==value
consumer=(a.source_dir/'bios_parser.c').read_text()
assert re.search(r'record_header->ucRecordType\s*==\s*0xff\s*\|\|\s*record_header->ucRecordSize\s*==\s*0',consumer)
assert facts['last_record_type']==255 and facts['last_record_token_bytes']==1
assert fields('ATOM_DISPLAY_OBJECT_PATH')==[('USHORT','usDeviceTag'),('USHORT','usSize'),('USHORT','usConnObjectId'),('USHORT','usGPUObjectId')]
assert re.search(r'USHORT\s+usGraphicObjIds\[\]',block('ATOM_DISPLAY_OBJECT_PATH'))
assert fields('ATOM_DISPLAY_OBJECT_PATH_TABLE')==[('UCHAR','ucNumOfDispPath'),('UCHAR','ucVersion'),('UCHAR','ucPadding')]
assert re.search(r'ucPadding\[2\]',block('ATOM_DISPLAY_OBJECT_PATH_TABLE'))
assert facts['display_path_table_header_bytes']==4 and facts['display_path_header_bytes']==8 and facts['direct_path_bytes']==10
for name,value in [('GRAPH_OBJECT_TYPE_GPU',1),('GRAPH_OBJECT_TYPE_ENCODER',2)]:
 m=re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)',obj,re.M);assert m and int(m[1],16)==value
assert re.search(r'^#define\s+ATOM_ENCODER_CAP_RECORD_TYPE\s+20',s,re.M)
for name in ['ATOM_ENCODER_CAP_RECORD','ATOM_ENCODER_CAP_RECORD_V2']:
 assert 'ATOM_COMMON_RECORD_HEADER' in block(name) and re.search(r'USHORT\s+usEncoderCap',block(name))
assert facts['encoder_cap_record_type']==20 and facts['encoder_cap_record_bytes']==4
paths=(a.source_dir/'amdgpu_atombios.c').read_text()
assert 'usGraphicObjIds[j]' in paths and 'le16_to_cpu(path->usSize) - 8' in paths and 'le16_to_cpu(cap_record->usEncoderCap)' in paths
print('PINNED POLARIS ROM FACT AUDIT: PASS;five source hashes,base layouts,link/record/I2C/HPD/GPIO facts end marker and direct path/encoder/capability layouts')
