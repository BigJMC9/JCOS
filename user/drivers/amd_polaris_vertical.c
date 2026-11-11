#include "amd_polaris_vertical.h"
#include "amd_polaris_timing.h"
static int refuse(JcosU64 *out,PolarisVerticalStatus status){out[0]=status;return 0;}
static JcosU64 step(JcosU32 field,JcosU32 value){return field|((JcosU64)value<<32);}
int polaris_vertical_plan(const JcosU32 *v,JcosU32 count,JcosU64 out[POLARIS_VERTICAL_WORDS]){
 if(!out)return 0;for(JcosU32 i=0;i<POLARIS_VERTICAL_WORDS;++i)out[i]=0;
 if(!v || count!=POLARIS_VERTICAL_VALUES)return refuse(out,POLARIS_VERTICAL_INVALID);
 for(JcosU32 i=0;i<count;++i)if(v[i]==~0U)return refuse(out,POLARIS_VERTICAL_ALL_ONES);
 for(JcosU32 i=0;i<3;++i)if(v[80+i]!=v[83+i])return refuse(out,POLARIS_VERTICAL_CHANGED);
 JcosU64 timing[POLARIS_TIMING_WORDS];
 if(!polaris_timing_observe(v,POLARIS_TIMING_VALUES,timing))return refuse(out,POLARIS_VERTICAL_TIMING);
 /* Exact previously observed inherited raster/route/stream, not a general mode
  * selector. Auto M remains raw and cannot establish a pixel clock/link rate. */
 if(timing[0]!=0x0002020000020000ULL || timing[1]!=0x00C0084007800898ULL ||
  timing[2]!=0x0029046104380465ULL || timing[3]!=0x00050000002C0000ULL || timing[4] ||
  timing[5]!=0x0000010002000000ULL || (JcosU32)timing[6]!=0x8000U ||
  timing[7]!=0x0000005000010201ULL || !v[39] || !v[73] ||
  (v[39]&~POLARIS_TIMING_M_MASK) || (v[73]&~POLARIS_TIMING_M_MASK) ||
  v[31]!=POLARIS_VERTICAL_BASE_TOTAL-1U)return refuse(out,POLARIS_VERTICAL_PROFILE);
 /* Initially inactive override only. Preserve exactly the admitted original
  * values; reject MC-training/force/mask/reserved bits, not just selectors. */
 if(v[82] || (v[80] && v[80]!=POLARIS_VERTICAL_BASE_TOTAL-1U) ||
  (v[81] && v[81]!=POLARIS_VERTICAL_BASE_TOTAL-1U))return refuse(out,POLARIS_VERTICAL_CONTROLS);
 const JcosU32 target=POLARIS_VERTICAL_TARGET_TOTAL-1U;
 out[0]=(JcosU64)POLARIS_VERTICAL_VERSION<<16;
 for(JcosU32 i=0;i<POLARIS_TIMING_WORDS;++i)out[1+i]=timing[i];
 out[10]=(JcosU64)v[80]|((JcosU64)v[81]<<32);out[11]=(JcosU64)v[82]|((JcosU64)target<<32);
 out[12]=step(POLARIS_VERTICAL_MIN,target);out[13]=step(POLARIS_VERTICAL_MAX,target);
 out[14]=step(POLARIS_VERTICAL_CONTROL,POLARIS_VERTICAL_MIN_SEL_MASK|POLARIS_VERTICAL_MAX_SEL_MASK);
 /* Disable override before restoring either bound, including interrupted
  * primary prefixes. Never depend on the failed driver's writable snapshot. */
 out[15]=step(POLARIS_VERTICAL_CONTROL,v[82]);out[16]=step(POLARIS_VERTICAL_MIN,v[80]);out[17]=step(POLARIS_VERTICAL_MAX,v[81]);
 return 1;
}
