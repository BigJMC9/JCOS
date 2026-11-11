#include "amd_polaris_link.h"
#include "../../include/amd_polaris_registers.h"
static int refuse(JcosU64 *out,PolarisLinkStatus status){out[0]=status;return 0;}
int polaris_link_observe(const JcosU32 *v,JcosU32 count,JcosU64 out[POLARIS_LINK_WORDS]){
 if(!out)return 0;for(JcosU32 i=0;i<POLARIS_LINK_WORDS;++i)out[i]=0;
 if(!v || count!=POLARIS_LINK_VALUES)return refuse(out,POLARIS_LINK_INVALID);
 for(JcosU32 i=0;i<POLARIS_LINK_VALUES;++i)if(v[i]==~0U)return refuse(out,POLARIS_LINK_ALL_ONES);
 for(JcosU32 i=0;i<6;++i)if(v[i]!=v[46+i])return refuse(out,POLARIS_LINK_CHANGED);
 for(JcosU32 i=0;i<20;++i)if(v[6+i]!=v[26+i])return refuse(out,POLARIS_LINK_CHANGED);
 JcosU32 crtc=0,running=0;
 for(JcosU32 i=0;i<6;++i){
  JcosU32 enabled=v[i]&(POLARIS_CRTC_MASTER_EN_MASK|POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK);
  if(enabled && enabled!=(POLARIS_CRTC_MASTER_EN_MASK|POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK))return refuse(out,POLARIS_LINK_CRTC_UNSUPPORTED);
  if(enabled){if(v[i]&POLARIS_CRTC_DISP_READ_REQUEST_DISABLE_MASK)return refuse(out,POLARIS_LINK_CRTC_UNSUPPORTED);crtc=i;++running;}
 }
 if(running!=1)return refuse(out,running ? POLARIS_LINK_CRTC_AMBIGUOUS:POLARIS_LINK_NO_CRTC);
 JcosU32 backend=0,enabled=0,mask=0;
 for(JcosU32 i=0;i<POLARIS_LINK_BACKENDS;++i)if(v[6+i*2]&POLARIS_LINK_ENABLE_MASK){backend=i;mask|=1U<<i;++enabled;}
 if(enabled!=1)return refuse(out,enabled ? POLARIS_LINK_BACKEND_AMBIGUOUS:POLARIS_LINK_NO_BACKEND);
 JcosU32 be=v[7+backend*2],select=(be&POLARIS_LINK_FRONTEND_MASK)>>POLARIS_LINK_FRONTEND_SHIFT;
 if(!select || (select&(select-1U)) || select>=(1U<<6))return refuse(out,POLARIS_LINK_SELECTOR);
 JcosU32 frontend=0;while((1U<<frontend)!=select)++frontend;
 JcosU32 fe=v[20+frontend];if((fe&POLARIS_LINK_CRTC_MASK)!=crtc)return refuse(out,POLARIS_LINK_SELECTOR);
 out[0]=((JcosU64)POLARIS_LINK_VERSION<<16)|((JcosU64)crtc<<32)|((JcosU64)backend<<40)|((JcosU64)frontend<<48)|((JcosU64)((be&POLARIS_LINK_MODE_MASK)>>POLARIS_LINK_MODE_SHIFT)<<56);
 out[1]=(JcosU64)mask|((JcosU64)((be&POLARIS_LINK_HPD_MASK)>>POLARIS_LINK_HPD_SHIFT)<<32)|((JcosU64)((fe&POLARIS_LINK_FE_CLOCK_MASK)!=0)<<40);
 out[2]=(JcosU64)be|((JcosU64)fe<<32);out[3]=(JcosU64)v[crtc]|((JcosU64)POLARIS_LINK_VALUES<<32);
 return 1;
}