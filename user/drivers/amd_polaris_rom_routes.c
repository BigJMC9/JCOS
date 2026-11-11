#include "amd_polaris_rom.h"
/* Bounded ROM bytes only. Raw register indices never authorize GPIO/MMIO. */
typedef struct {JcosU32 first,end;} RomSpan;
typedef struct {
 const unsigned char *bytes;
 JcosU32 size,header,starts[6];
 RomSpan directories[5];
 const PolarisRomInfo *info;
} RomObjectLayout;
static unsigned short read16(const unsigned char *b){return (unsigned short)(b[0]|((unsigned short)b[1]<<8));}
static int fits(JcosU32 size,JcosU32 at,JcosU32 count){return at<=size && count<=size-at;}
static void clear_route(PolarisRomRoute *r){
 unsigned char *p=(unsigned char *)r;for(JcosU32 i=0;i<sizeof(*r);++i)p[i]=0;
}
static int object_id(JcosU32 id){
 JcosU32 type=(id>>12)&7U;
 return !(id&0x8800U) && (id&0x700U) && (type==1 || type==2 || type==3 || type==4 || type==6 || type==7) && ((id&255U) || type==1);
}
static int layout_init(const unsigned char *b,JcosU32 size,const PolarisRomInfo *info,RomObjectLayout *l){
 if(!fits(size,info->object_header,info->object_bytes) || info->object_bytes<16)return 0;
 l->bytes=b+info->object_header;l->size=info->object_bytes;l->header=info->revision==3 ? 18U:16U;l->info=info;
 if(l->size<l->header)return 0;
 const JcosU32 fields[5]={6,8,10,12,16};
 for(JcosU32 i=0;i<5;++i){
  JcosU32 at=i==4 && info->revision!=3 ? 0:read16(l->bytes+fields[i]);
  l->starts[i]=at;l->directories[i].first=0;l->directories[i].end=0;
  if(!at)continue;
  if(at<l->header || !fits(l->size,at,4))return 0;
  JcosU32 count=l->bytes[at],length=4+count*8;
  if(count>32 || !fits(l->size,at,length) || (i==0 && count!=info->count))return 0;
  l->directories[i].first=at;l->directories[i].end=at+length;
  for(JcosU32 j=0;j<i;++j)if(l->directories[j].end && at<l->directories[j].end && l->directories[j].first<at+length)return 0;
 }
 l->starts[5]=read16(l->bytes+14); /* Display-path contents remain uninterpreted. */
 if(l->starts[5] && (l->starts[5]<l->header || !fits(l->size,l->starts[5],4)))return 0;
 return 1;
}
static int data_range(const RomObjectLayout *l,JcosU32 start,JcosU32 length){
 if(start<l->header || !fits(l->size,start,length))return 0;
 for(JcosU32 i=0;i<5;++i)if(l->directories[i].end && start<l->directories[i].end && l->directories[i].first<start+length)return 0;
 return !l->starts[5] || start>=l->starts[5]+4 || start+length<=l->starts[5];
}
static JcosU32 section_limit(const RomObjectLayout *l,JcosU32 start){
 JcosU32 end=l->size;
 for(JcosU32 i=0;i<6;++i)if(l->starts[i]>start && l->starts[i]<end)end=l->starts[i];
 for(JcosU32 i=0;i<l->info->count;++i){
  JcosU32 links=(JcosU32)((l->info->connectors[i]>>16)&0xFFFFU),records=(JcosU32)((l->info->connectors[i]>>32)&0xFFFFU);
  if(links>start && links<end)end=links;
  if(records>start && records<end)end=records;
 }
 return end;
}
static JcosU32 read_links(const RomObjectLayout *l,JcosU32 at,PolarisRomRoute *r){
 if(!at)return POLARIS_ROM_ROUTE_OK;
 JcosU32 end=section_limit(l,at);
 if(!data_range(l,at,1))return POLARIS_ROM_ROUTE_BOUNDS;
 JcosU32 sources=l->bytes[at++];
 if(sources>POLARIS_ROM_LINK_IDS || !fits(end,at,sources*2+1))return POLARIS_ROM_ROUTE_BAD_LINKS;
 for(JcosU32 i=0;i<sources;++i){
  JcosU32 id=read16(l->bytes+at+i*2);if(!object_id(id))return POLARIS_ROM_ROUTE_BAD_LINKS;
  for(JcosU32 j=0;j<i;++j)if(r->sources[j]==id)return POLARIS_ROM_ROUTE_DUPLICATE;
  r->sources[i]=(unsigned short)id;
 }
 at+=sources*2;JcosU32 destinations=l->bytes[at++];
 if(destinations>POLARIS_ROM_LINK_IDS || !fits(end,at,destinations*2) || !data_range(l,at,destinations*2))return POLARIS_ROM_ROUTE_BAD_LINKS;
 for(JcosU32 i=0;i<destinations;++i){
  JcosU32 id=read16(l->bytes+at+i*2);if(!object_id(id))return POLARIS_ROM_ROUTE_BAD_LINKS;
  for(JcosU32 j=0;j<i;++j)if(r->destinations[j]==id)return POLARIS_ROM_ROUTE_DUPLICATE;
  r->destinations[i]=(unsigned short)id;
 }
 r->source_count=sources;r->dest_count=destinations;r->flags|=POLARIS_ROM_ROUTE_LINKS;
 return POLARIS_ROM_ROUTE_OK;
}
static JcosU32 read_records(const RomObjectLayout *l,JcosU32 at,PolarisRomRoute *r){
 if(!at)return POLARIS_ROM_ROUTE_OK;
 JcosU32 end=section_limit(l,at);
 for(JcosU32 n=0;n<=POLARIS_ROM_RECORD_LIMIT;++n){
  /* FF is a complete end token; the next byte may belong to another section. */
  if(!fits(end,at,1) || !data_range(l,at,1))return POLARIS_ROM_ROUTE_BAD_RECORDS;
  JcosU32 type=l->bytes[at];
  if(type==255){r->flags|=POLARIS_ROM_ROUTE_RECORDS;return POLARIS_ROM_ROUTE_OK;}
  if(!fits(end,at,2) || !data_range(l,at,2))return POLARIS_ROM_ROUTE_BAD_RECORDS;
  JcosU32 length=l->bytes[at+1];
  if(!type && !length){r->flags|=POLARIS_ROM_ROUTE_RECORDS;return POLARIS_ROM_ROUTE_OK;}
  if(n==POLARIS_ROM_RECORD_LIMIT || length<2 || !fits(end,at,length) || !data_range(l,at,length))return POLARIS_ROM_ROUTE_BAD_RECORDS;
  if(type<1 || type>22)return POLARIS_ROM_ROUTE_UNSUPPORTED;
  if(type==1){
   if(length!=4)return POLARIS_ROM_ROUTE_BAD_RECORDS;
   if(r->flags&POLARIS_ROM_ROUTE_I2C)return POLARIS_ROM_ROUTE_DUPLICATE;
   r->i2c_id=l->bytes[at+2];r->i2c_slave=l->bytes[at+3];r->flags|=POLARIS_ROM_ROUTE_I2C;
  }else if(type==2){
   if(length!=4 || l->bytes[at+3]>1)return POLARIS_ROM_ROUTE_BAD_RECORDS;
   if(r->flags&POLARIS_ROM_ROUTE_HPD)return POLARIS_ROM_ROUTE_DUPLICATE;
   r->hpd_gpio=l->bytes[at+2];r->hpd_state=l->bytes[at+3];r->flags|=POLARIS_ROM_ROUTE_HPD;
  }else ++r->other_count;
  r->record_types|=1U<<type;++r->record_count;at+=length;
 }
 return POLARIS_ROM_ROUTE_BAD_RECORDS;
}
static JcosU32 gpio_table(const unsigned char *b,JcosU32 size,const PolarisRomInfo *info,JcosU32 index,
 JcosU32 width,JcosU32 maximum,JcosU32 *at,JcosU32 *length,JcosU32 *count){
 if(!fits(size,info->master_data,4+index*2+2))return POLARIS_ROM_ROUTE_BOUNDS;
 *at=read16(b+info->master_data+4+index*2);
 if(!*at || *at<0x4AU || !fits(size,*at,4))return POLARIS_ROM_ROUTE_GPIO;
 *length=read16(b+*at);
 if(*length<4 || !fits(size,*at,*length) || (*length-4)%width)return POLARIS_ROM_ROUTE_BOUNDS;
 if(b[*at+2]!=1 || b[*at+3]!=1)return POLARIS_ROM_ROUTE_REVISION;
 *count=(*length-4)/width;
 return !*count || *count>maximum ? POLARIS_ROM_ROUTE_BOUNDS:POLARIS_ROM_ROUTE_OK;
}
static JcosU32 resolve_gpio(const unsigned char *b,JcosU32 size,const PolarisRomInfo *info,PolarisRomRoute *r){
 JcosU32 at=0,length=0,count=0,status,found;
 if(r->flags&POLARIS_ROM_ROUTE_I2C){
  status=gpio_table(b,size,info,10,27,32,&at,&length,&count);if(status)return status;
  r->table_info=(JcosU64)at|((JcosU64)length<<16);found=0;
  for(JcosU32 i=0;i<count;++i){
   const unsigned char *p=b+at+4+i*27;if(p[16]!=r->i2c_id)continue;
   if(++found!=1)return POLARIS_ROM_ROUTE_DUPLICATE;
   if(p[25] || p[26])return POLARIS_ROM_ROUTE_GPIO;
   for(JcosU32 n=0;n<8;++n){
    if(p[17+n]>=32)return POLARIS_ROM_ROUTE_GPIO;
    r->i2c_registers[n]=read16(p+n*2);r->i2c_shifts[n]=p[17+n];
   }
  }
  if(found!=1)return POLARIS_ROM_ROUTE_GPIO;r->flags|=POLARIS_ROM_ROUTE_I2C_GPIO;
 }
 if(r->flags&POLARIS_ROM_ROUTE_HPD){
  status=gpio_table(b,size,info,12,4,64,&at,&length,&count);if(status)return status;
  r->table_info|=((JcosU64)at<<32)|((JcosU64)length<<48);found=0;
  for(JcosU32 i=0;i<count;++i){
   const unsigned char *p=b+at+4+i*4;if(p[3]!=r->hpd_gpio)continue;
   if(++found!=1)return POLARIS_ROM_ROUTE_DUPLICATE;
   if(p[2]>=32)return POLARIS_ROM_ROUTE_GPIO;
   r->hpd_register=read16(p);r->hpd_shift=p[2];
  }
  if(found!=1)return POLARIS_ROM_ROUTE_GPIO;r->flags|=POLARIS_ROM_ROUTE_HPD_GPIO;
 }
 return POLARIS_ROM_ROUTE_OK;
}
void polaris_rom_routes_parse(const unsigned char *b,JcosU32 size,PolarisRomInfo *info){
 if(!info)return;
 for(JcosU32 i=0;i<POLARIS_ROM_CONNECTORS;++i)clear_route(&info->routes[i]);
 if(!b || size>POLARIS_ROM_LIMIT || info->status || info->count>POLARIS_ROM_CONNECTORS)return;
 RomObjectLayout layout;
 int valid=layout_init(b,size,info,&layout);
 for(JcosU32 i=0;i<info->count;++i){
  PolarisRomRoute route;clear_route(&route);
  JcosU32 links=(JcosU32)((info->connectors[i]>>16)&0xFFFFU),records=(JcosU32)((info->connectors[i]>>32)&0xFFFFU);
  JcosU32 status=valid ? POLARIS_ROM_ROUTE_OK:POLARIS_ROM_ROUTE_BOUNDS;
  if(!status && !links && !records)status=POLARIS_ROM_ROUTE_ABSENT;
  if(!status && links && links==records)status=POLARIS_ROM_ROUTE_BOUNDS;
  if(!status)status=read_links(&layout,links,&route);
  if(!status)status=read_records(&layout,records,&route);
  if(!status)status=resolve_gpio(b,size,info,&route);
  if(status)clear_route(&route);route.status=status;
  volatile unsigned char *to=(volatile unsigned char *)&info->routes[i];
  const unsigned char *from=(const unsigned char *)&route;
  for(JcosU32 n=0;n<sizeof(route);++n)to[n]=from[n];
 }
}