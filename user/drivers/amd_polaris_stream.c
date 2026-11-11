#include "amd_polaris_stream.h"
#include "amd_polaris_timing.h"
static int refuse(JcosU64 *out,PolarisStreamStatus status){out[0]=status;return 0;}
int polaris_stream_observe(const JcosU32 *v,JcosU32 count,JcosU64 out[POLARIS_STREAM_WORDS]){
 if(!out)return 0;for(JcosU32 i=0;i<POLARIS_STREAM_WORDS;++i)out[i]=0;
 if(!v || count!=POLARIS_STREAM_VALUES)return refuse(out,POLARIS_STREAM_INVALID);
 for(JcosU32 i=0;i<count;++i)if(v[i]==~0U)return refuse(out,POLARIS_STREAM_ALL_ONES);
 for(JcosU32 f=0;f<POLARIS_STREAM_FIELDS;++f)if(v[86+f]!=v[91+f])return refuse(out,POLARIS_STREAM_CHANGED);
 JcosU64 timing[POLARIS_TIMING_WORDS];
 if(!jcos_polaris_stream_read_admitted(v,POLARIS_VERTICAL_VALUES) ||
  !polaris_timing_observe(v,POLARIS_TIMING_VALUES,timing))return refuse(out,POLARIS_STREAM_BASELINE);
 /* Correlate the two extended stream/FE witnesses with their earlier samples.
  * Unknown FIFO/lock bits remain raw diagnostics, never authority to clear them. */
 if(v[86]!=v[26] || v[88]!=v[22])return refuse(out,POLARIS_STREAM_ROUTE);
 out[0]=(JcosU64)POLARIS_STREAM_VERSION<<16;
 for(JcosU32 i=0;i<POLARIS_TIMING_WORDS;++i)out[1+i]=timing[i];
 out[10]=(JcosU64)v[86]|((JcosU64)v[87]<<32);out[11]=(JcosU64)v[88]|((JcosU64)v[89]<<32);out[12]=v[90];
 out[13]=((v[87]&POLARIS_STREAM_FIFO_RESET_MASK)?POLARIS_STREAM_FIFO_RESET:0U)|
  ((v[89]&POLARIS_STREAM_UPDATE_LOCK_MASK)?POLARIS_STREAM_UPDATE_HELD:0U)|
  (((v[89]&~POLARIS_STREAM_UPDATE_LOCK_MASK) || (v[90]&~POLARIS_STREAM_UPDATE_MODE_MASK))?POLARIS_STREAM_UPDATE_OTHER:0U)|
  ((v[87]&~POLARIS_STREAM_FIFO_RESET_MASK)?POLARIS_STREAM_FIFO_OTHER:0U);
 return 1;
}
