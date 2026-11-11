#include "amd_polaris_rom.h"
/* Direct firmware paths only. Raw IDs/capability bits confer no device access. */
typedef struct {JcosU32 first,end;} TopoSpan;
typedef struct {
 const unsigned char *b;const PolarisRomInfo *info;
 JcosU32 size,header,starts[6],encoders,path_end;
 TopoSpan tables[5];
} TopoLayout;
static unsigned short rd16(const unsigned char *b){return (unsigned short)(b[0]|((unsigned short)b[1]<<8));}
static int fits(JcosU32 end,JcosU32 at,JcosU32 n){return at<=end && n<=end-at;}
static int overlaps(JcosU32 at,JcosU32 n,TopoSpan s){return s.end && at<s.end && s.first<at+n;}
static int id_valid(JcosU32 id){
 JcosU32 t=(id>>12)&7U;
 return !(id&0x8800U) && (id&0x700U) && (t==1 || t==2 || t==3 || t==4 || t==6 || t==7) && ((id&255U) || t==1);
}
static JcosU32 limit(const TopoLayout *l,JcosU32 at,int encoder_data){
 JcosU32 end=l->size;
 for(JcosU32 i=0;i<6;++i)if(l->starts[i]>at && l->starts[i]<end)end=l->starts[i];
 for(JcosU32 i=0;i<l->info->count;++i){
  JcosU64 c=l->info->connectors[i];JcosU32 links=(JcosU32)((c>>16)&65535),records=(JcosU32)((c>>32)&65535);
  if(links>at && links<end)end=links;if(records>at && records<end)end=records;
 }
 if(encoder_data)for(JcosU32 i=0;i<l->encoders;++i){
  const unsigned char *p=l->b+l->starts[2]+4+i*8;
  JcosU32 links=rd16(p+2),records=rd16(p+4);
  if(links>at && links<end)end=links;if(records>at && records<end)end=records;
 }
 return end;
}
static int data(const TopoLayout *l,JcosU32 at,JcosU32 n){
 if(at<l->header || !fits(l->size,at,n))return 0;
 for(JcosU32 i=0;i<5;++i)if(overlaps(at,n,l->tables[i]))return 0;
 if(l->path_end && overlaps(at,n,(TopoSpan){l->starts[5],l->path_end}))return 0;
 /* Conservative connector partitions protect valid inventory and its padding. */
 for(JcosU32 i=0;i<l->info->count;++i)for(JcosU32 f=0;f<2;++f){
  JcosU32 start=(JcosU32)((l->info->connectors[i]>>(16+f*16))&65535);
  if(start && overlaps(at,n,(TopoSpan){start,limit(l,start,0)}))return 0;
 }
 return 1;
}
static JcosU32 layout(const unsigned char *b,JcosU32 size,const PolarisRomInfo *info,TopoLayout *l){
 if(!fits(size,info->object_header,info->object_bytes))return POLARIS_ROM_TOPOLOGY_BOUNDS;
 l->b=b+info->object_header;l->info=info;l->size=info->object_bytes;
 l->header=info->revision==3 ? 18U:16U;l->encoders=0;l->path_end=0;
 if(l->size<l->header)return POLARIS_ROM_TOPOLOGY_BOUNDS;
 const JcosU32 fields[5]={6,8,10,12,16};
 for(JcosU32 i=0;i<5;++i){
  JcosU32 at=i==4 && info->revision!=3 ? 0:rd16(l->b+fields[i]);
  l->starts[i]=at;l->tables[i]=(TopoSpan){0,0};if(!at)continue;
  if(at<l->header || !fits(l->size,at,4))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  JcosU32 count=l->b[at],bytes=4+count*8;
  if(count>POLARIS_ROM_ENCODER_LIMIT || !fits(l->size,at,bytes) || (!i && count!=info->count))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  for(JcosU32 j=0;j<i;++j)if(overlaps(at,bytes,l->tables[j]))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  l->tables[i]=(TopoSpan){at,at+bytes};
 }
 l->starts[5]=rd16(l->b+14);
 if(!l->starts[5] || !l->starts[2])return POLARIS_ROM_TOPOLOGY_ABSENT;
 JcosU32 at=l->starts[2];l->encoders=l->b[at];
 if(!l->encoders)return POLARIS_ROM_TOPOLOGY_ABSENT;
 if(l->b[at+1] || l->b[at+2] || l->b[at+3])return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 for(JcosU32 i=0;i<l->encoders;++i){
  const unsigned char *p=l->b+at+4+i*8;JcosU32 id=rd16(p),links=rd16(p+2),records=rd16(p+4);
  if(!id_valid(id) || ((id>>12)&7U)!=2 || rd16(p+6))return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
  if(!links || !records || links<l->header || records<l->header || !fits(l->size,links,1) || !fits(l->size,records,1))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  for(JcosU32 j=0;j<i;++j)if(rd16(l->b+at+4+j*8)==id)return POLARIS_ROM_TOPOLOGY_DUPLICATE;
 }
 at=l->starts[5];JcosU32 end=limit(l,at,1);
 if(!fits(end,at,4) || !data(l,at,4))return POLARIS_ROM_TOPOLOGY_BOUNDS;
 if(l->b[at+1]!=1)return POLARIS_ROM_TOPOLOGY_REVISION;
 if(l->b[at+2] || l->b[at+3])return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 JcosU32 count=l->b[at];if(!count)return POLARIS_ROM_TOPOLOGY_ABSENT;
 if(count>POLARIS_ROM_PATH_LIMIT)return POLARIS_ROM_TOPOLOGY_BOUNDS;
 at+=4;
 for(JcosU32 i=0;i<count;++i){
  if(!fits(end,at,8))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  JcosU32 bytes=rd16(l->b+at+2);
  if(bytes<10 || (bytes&1) || (bytes-8)/2>POLARIS_ROM_PATH_OBJECTS || !fits(end,at,bytes) || !data(l,at,bytes))return POLARIS_ROM_TOPOLOGY_BOUNDS;
  JcosU32 connector=rd16(l->b+at+4),gpu=rd16(l->b+at+6);
  if(!id_valid(connector) || ((connector>>12)&7U)!=3 || !id_valid(gpu) || ((gpu>>12)&7U)!=1)return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
  for(JcosU32 n=8;n<bytes;n+=2)if(!id_valid(rd16(l->b+at+n)))return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
  at+=bytes;
 }
 l->path_end=at;return POLARIS_ROM_TOPOLOGY_OK;
}
static JcosU32 capabilities(const TopoLayout *l,JcosU32 at,JcosU32 *caps,JcosU32 *records,JcosU32 *other){
 JcosU32 end=limit(l,at,1),found=0;*caps=*records=*other=0;
 for(JcosU32 n=0;n<=POLARIS_ROM_RECORD_LIMIT;++n){
  if(!fits(end,at,1) || !data(l,at,1))return POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
  JcosU32 type=l->b[at];
  if(type==255)return found ? POLARIS_ROM_TOPOLOGY_OK:POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
  if(!fits(end,at,2) || !data(l,at,2))return POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
  JcosU32 bytes=l->b[at+1];
  if(!type && !bytes)return found ? POLARIS_ROM_TOPOLOGY_OK:POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
  if(n==POLARIS_ROM_RECORD_LIMIT || bytes<2 || !fits(end,at,bytes) || !data(l,at,bytes))return POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
  if(type<1 || type>22)return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
  if(type==20){
   if(bytes!=4)return POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
   if(found++)return POLARIS_ROM_TOPOLOGY_DUPLICATE;*caps=rd16(l->b+at+2);
  }else ++*other;
  ++*records;at+=bytes;
 }
 return POLARIS_ROM_TOPOLOGY_BAD_RECORDS;
}
static JcosU32 one(const TopoLayout *l,JcosU32 index,JcosU64 words[4]){
 JcosU32 connector=(JcosU32)(l->info->connectors[index]&65535),at=l->starts[5]+4,path=0,matches=0;
 for(JcosU32 i=0;i<l->b[l->starts[5]];++i){
  if(rd16(l->b+at+4)==connector){path=at;++matches;}at+=rd16(l->b+at+2);
 }
 if(!matches)return POLARIS_ROM_TOPOLOGY_ABSENT;
 if(matches!=1)return POLARIS_ROM_TOPOLOGY_DUPLICATE;
 if(rd16(l->b+path+2)!=10)return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 JcosU32 tag=rd16(l->b+path),gpu=rd16(l->b+path+6),encoder=rd16(l->b+path+8);
 if(!tag || !(tag&rd16(l->b+4)) || ((encoder>>12)&7U)!=2)return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
 const PolarisRomRoute *route=&l->info->routes[index];
 if(route->status || !(route->flags&POLARIS_ROM_ROUTE_LINKS))return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
 if(route->source_count!=1 || route->dest_count)return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 if(route->sources[0]!=encoder)return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
 JcosU32 entry=0;
 for(JcosU32 i=0;i<l->encoders;++i)if(rd16(l->b+l->starts[2]+4+i*8)==encoder)entry=l->starts[2]+4+i*8;
 if(!entry)return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
 JcosU32 links=rd16(l->b+entry+2),records_at=rd16(l->b+entry+4),end=limit(l,links,1);
 if(!fits(end,links,1) || !data(l,links,1))return POLARIS_ROM_TOPOLOGY_BOUNDS;
 if(l->b[links]!=1)return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 if(!fits(end,links,6) || !data(l,links,6))return POLARIS_ROM_TOPOLOGY_BOUNDS;
 if(l->b[links+3]!=1)return POLARIS_ROM_TOPOLOGY_UNSUPPORTED;
 if(rd16(l->b+links+1)!=gpu || rd16(l->b+links+4)!=connector)return POLARIS_ROM_TOPOLOGY_INCONSISTENT;
 JcosU32 caps=0,records=0,other=0,status=capabilities(l,records_at,&caps,&records,&other);if(status)return status;
 words[0]=((JcosU64)POLARIS_ROM_TOPOLOGY_FLAGS<<16)|(1ULL<<24)|((JcosU64)records<<32)|((JcosU64)other<<40);
 words[1]=(JcosU64)tag|((JcosU64)gpu<<16)|((JcosU64)encoder<<32);
 words[2]=(JcosU64)caps|((JcosU64)gpu<<16)|((JcosU64)connector<<32);
 words[3]=(JcosU64)path|((JcosU64)entry<<16)|((JcosU64)links<<32)|((JcosU64)records_at<<48);
 return POLARIS_ROM_TOPOLOGY_OK;
}
void polaris_rom_topology_parse(const unsigned char *b,JcosU32 size,PolarisRomInfo *info){
 if(!info)return;
 for(JcosU32 i=0;i<POLARIS_ROM_CONNECTORS;++i)for(JcosU32 f=0;f<POLARIS_ROM_TOPOLOGY_WORDS;++f)info->topology[i][f]=0;
 if(!b || size>POLARIS_ROM_LIMIT || info->status || info->count>POLARIS_ROM_CONNECTORS)return;
 TopoLayout l;JcosU32 common=layout(b,size,info,&l);
 for(JcosU32 i=0;i<info->count;++i){
  JcosU64 words[4]={0,0,0,0};JcosU32 status=common ? common:one(&l,i,words);
  if(status)info->topology[i][0]=status;
  else for(JcosU32 f=0;f<4;++f)info->topology[i][f]=words[f];
 }
}