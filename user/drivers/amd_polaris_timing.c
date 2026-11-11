#include "amd_polaris_timing.h"
#include "amd_polaris_link.h"
static int refuse(JcosU64 *out,PolarisTimingStatus status){out[0]=status;return 0;}
int polaris_timing_observe(const JcosU32 *v,JcosU32 count,JcosU64 out[POLARIS_TIMING_WORDS]){
 if(!out)return 0;for(JcosU32 i=0;i<POLARIS_TIMING_WORDS;++i)out[i]=0;
 if(!v || count!=POLARIS_TIMING_VALUES)return refuse(out,POLARIS_TIMING_INVALID);
 for(JcosU32 i=0;i<count;++i)if(v[i]==~0U)return refuse(out,POLARIS_TIMING_ALL_ONES);
 for(JcosU32 i=0;i<6;++i)if(v[i]!=v[74+i])return refuse(out,POLARIS_TIMING_CHANGED);
 /* M is a documented automatic24-bit measurement when GEN_EN is set.
  * Retain both samples;never mask a static control/N/route change. */
 for(JcosU32 i=0;i<34;++i)if(i!=33 && v[6+i]!=v[40+i])return refuse(out,POLARIS_TIMING_CHANGED);
 if(v[39]!=v[73] && (!(v[37]&POLARIS_TIMING_AUTO_MN_MASK) ||
  ((v[39]^v[73])&~POLARIS_TIMING_M_MASK)))return refuse(out,POLARIS_TIMING_CHANGED);
 JcosU32 link[POLARIS_LINK_VALUES];JcosU64 route[POLARIS_LINK_WORDS];
 for(JcosU32 i=0;i<26;++i)link[i]=v[i];
 for(JcosU32 i=0;i<20;++i)link[26+i]=v[40+i];
 for(JcosU32 i=0;i<6;++i)link[46+i]=v[74+i];
 if(!jcos_polaris_timing_read_admitted(v,26) || !polaris_link_observe(link,POLARIS_LINK_VALUES,route))return refuse(out,POLARIS_TIMING_LINK);
 const JcosU32 *t=v+26;
 if(!(t[0]&POLARIS_TIMING_STREAM_MASK) || (t[9]&POLARIS_TIMING_INTERLACE_MASK))return refuse(out,POLARIS_TIMING_UNSUPPORTED);
 JcosU32 ht=(t[1]&POLARIS_TIMING_COORD_MASK)+1,hs=t[2]&POLARIS_TIMING_COORD_MASK,he=(t[2]&POLARIS_TIMING_HIGH_COORD_MASK)>>16;
 JcosU32 vt=(t[5]&POLARIS_TIMING_COORD_MASK)+1,vs=t[6]&POLARIS_TIMING_COORD_MASK,ve=(t[6]&POLARIS_TIMING_HIGH_COORD_MASK)>>16;
 JcosU32 hss=t[3]&POLARIS_TIMING_COORD_MASK,hse=(t[3]&POLARIS_TIMING_HIGH_COORD_MASK)>>16;
 JcosU32 vss=t[7]&POLARIS_TIMING_COORD_MASK,vse=(t[7]&POLARIS_TIMING_HIGH_COORD_MASK)>>16;
 /* Progressive observed raster,including any borders;do not guess wraparound
  * or equate raster size with the separately qualified GOP surface size. */
 if(hs>=ht || he>=hs || vs>=vt || ve>=vs ||
  hss>=hse || hse>he || vss>=vse || vse>ve)return refuse(out,POLARIS_TIMING_GEOMETRY);
 out[0]=(route[0]&~0xFFFF0000ULL)|((JcosU64)POLARIS_TIMING_VERSION<<16);
 out[1]=(JcosU64)ht|((JcosU64)(hs-he)<<16)|((JcosU64)hs<<32)|((JcosU64)he<<48);
 out[2]=(JcosU64)vt|((JcosU64)(vs-ve)<<16)|((JcosU64)vs<<32)|((JcosU64)ve<<48);
 out[3]=(JcosU64)hss|((JcosU64)hse<<16)|((JcosU64)vss<<32)|((JcosU64)vse<<48);
 out[4]=(JcosU64)(t[4]&POLARIS_TIMING_POL_MASK)|((JcosU64)(t[8]&POLARIS_TIMING_POL_MASK)<<8);
 out[5]=(JcosU64)t[10]|((JcosU64)t[11]<<32);
 out[6]=(JcosU64)t[12]|((JcosU64)t[13]<<32);
 out[7]=(JcosU64)t[0]|((JcosU64)POLARIS_TIMING_VALUES<<32);
 out[8]=(JcosU64)v[39]|((JcosU64)v[73]<<32);
 return 1;
}
