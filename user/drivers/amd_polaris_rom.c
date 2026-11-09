#include "amd_polaris_rom.h"
static unsigned short le16(const unsigned char *b){return (unsigned short)(b[0]|((unsigned short)b[1]<<8));}
static int range(JcosU32 total,JcosU32 offset,JcosU32 bytes){return offset<=total && bytes<=total-offset;}
static int magic(const unsigned char *b,const char *s){for(JcosU32 i=0;i<4;++i)if(b[i]!=(unsigned char)s[i])return 0;return 1;}
static void clear(PolarisRomInfo *o){unsigned char *b=(unsigned char *)o;for(JcosU32 i=0;i<sizeof(*o);++i)b[i]=0;}
void polaris_rom_parse(const unsigned char *b,JcosU32 size,JcosU32 identity,PolarisRomInfo *out){
 if(!out)return;clear(out);out->status=POLARIS_ROM_SIZE;
 if(!b || size<0x4AU || size>POLARIS_ROM_LIMIT)return;
 JcosU32 crc=0xFFFFFFFFU;
 for(JcosU32 i=0;i<size;++i){crc^=b[i];for(JcosU32 j=0;j<8;++j)crc=(crc>>1)^((0U-(crc&1U))&0xEDB88320U);}
 out->crc32=~crc;
#define REFUSE(reason) do {JcosU32 saved=out->crc32;clear(out);out->crc32=saved;out->status=(reason);return;} while(0)
 if(b[0]!=0x55 || b[1]!=0xAA)REFUSE(POLARIS_ROM_SIGNATURE);
 JcosU32 pcir=le16(b+0x18);
 if(pcir<0x1CU || !range(size,pcir,0x18) || !magic(b+pcir,"PCIR"))REFUSE(POLARIS_ROM_PCIR);
 JcosU32 pcir_bytes=le16(b+pcir+10),image_bytes=(JcosU32)le16(b+pcir+16)*512U;
 if(pcir_bytes<0x18 || !image_bytes || image_bytes>size || !range(image_bytes,pcir,pcir_bytes) ||
    !b[2] || (JcosU32)b[2]*512U>image_bytes || b[pcir+20]!=0)REFUSE(POLARIS_ROM_LENGTH);
 if((JcosU32)le16(b+pcir+4)!= (identity&0xFFFFU) ||
    (JcosU32)le16(b+pcir+6)!=(identity>>16) || identity!=0x67DF1002U)REFUSE(POLARIS_ROM_IDENTITY);
 unsigned char sum=0;for(JcosU32 i=0;i<image_bytes;++i)sum=(unsigned char)(sum+b[i]);
 if(sum)REFUSE(POLARIS_ROM_CHECKSUM);
 JcosU32 rom=le16(b+0x48);
 if(rom<0x4AU || !range(image_bytes,rom,36) || !magic(b+rom+4,"ATOM"))REFUSE(POLARIS_ROM_ATOM);
 JcosU32 rom_bytes=le16(b+rom);
 if(rom_bytes<36 || !range(image_bytes,rom,rom_bytes))REFUSE(POLARIS_ROM_ATOM);
 if(b[rom+2]!=1 || b[rom+3]!=1)REFUSE(POLARIS_ROM_REVISION);
 JcosU32 master=le16(b+rom+32);
 if(master<0x4AU || !range(image_bytes,master,50))REFUSE(POLARIS_ROM_TABLE);
 JcosU32 master_bytes=le16(b+master);
 if(master_bytes<50 || !range(image_bytes,master,master_bytes))REFUSE(POLARIS_ROM_TABLE);
 if(b[master+2]!=1 || b[master+3]!=1)REFUSE(POLARIS_ROM_REVISION);
 /* Object_Header is entry22 in ATOM_MASTER_LIST_OF_DATA_TABLES. */
 JcosU32 object=le16(b+master+4+22*2);
 if(object<0x4AU || !range(image_bytes,object,16))REFUSE(POLARIS_ROM_TABLE);
 JcosU32 object_bytes=le16(b+object),rev=b[object+3],header=rev==3 ? 18U:16U;
 if(b[object+2]!=1 || rev<1 || rev>3)REFUSE(POLARIS_ROM_REVISION);
 if(object_bytes<header || !range(image_bytes,object,object_bytes))REFUSE(POLARIS_ROM_TABLE);
 JcosU32 connector=le16(b+object+6);
 if(connector<header || !range(object_bytes,connector,4))REFUSE(POLARIS_ROM_CONNECTOR);
 JcosU32 count=b[object+connector];
 if(!count || count>POLARIS_ROM_CONNECTORS || !range(object_bytes,connector+4,count*8))REFUSE(POLARIS_ROM_CONNECTOR);
 PolarisRomInfo parsed;clear(&parsed);
 for(JcosU32 i=0;i<count;++i){
  const unsigned char *p=b+object+connector+4+i*8;
  JcosU32 id=le16(p),links=le16(p+2),records=le16(p+4);
  if((id&0xF800U)!=0x3000U || !(id&0xFFU) || !(id&0x700U) || le16(p+6) ||
     (links && (links<header || !range(object_bytes,links,1))) ||
     (records && (records<header || !range(object_bytes,records,2))))REFUSE(POLARIS_ROM_CONNECTOR);
  for(JcosU32 j=0;j<i;++j)if((parsed.connectors[j]&0xFFFFU)==id)REFUSE(POLARIS_ROM_CONNECTOR);
  /* Kind is only an inventory label: 1=DP,2=HDMI-A,0=other. Records are not followed. */
  JcosU64 kind=(id&0xFFU)==0x13U ? 1U : (id&0xFFU)==0xCU ? 2U : 0U;
  parsed.connectors[i]=(JcosU64)id|((JcosU64)links<<16)|((JcosU64)records<<32)|(kind<<48);
 }
 parsed.crc32=out->crc32;parsed.status=POLARIS_ROM_OK;parsed.image_bytes=image_bytes;
 parsed.rom_header=(unsigned short)rom;parsed.master_data=(unsigned short)master;parsed.object_header=(unsigned short)object;
 parsed.object_bytes=(unsigned short)object_bytes;parsed.format=1;parsed.revision=(unsigned char)rev;parsed.count=(unsigned char)count;
 /* Explicit volatile byte stores avoid an implicit freestanding memcpy import. */
 volatile unsigned char *destination=(volatile unsigned char *)out;
 const unsigned char *source=(const unsigned char *)&parsed;
 for(JcosU32 i=0;i<sizeof(parsed);++i)destination[i]=source[i];
#undef REFUSE
}
JcosU64 polaris_rom_query(const PolarisRomInfo *o,JcosU32 i){
 if(!o)return 0;
 if(i==0)return (JcosU64)o->status|((JcosU64)o->format<<16)|((JcosU64)o->revision<<24)|((JcosU64)o->count<<32);
 if(i==1)return o->crc32;
 if(i==2)return (JcosU64)o->rom_header|((JcosU64)o->master_data<<16)|((JcosU64)o->object_header<<32)|((JcosU64)o->object_bytes<<48);
 if(i==3)return o->image_bytes;
 return i<POLARIS_ROM_QUERIES ? o->connectors[i-4] : 0;
}
