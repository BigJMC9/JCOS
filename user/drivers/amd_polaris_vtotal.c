#include "amd_polaris_vtotal.h"
static int read_value(const PolarisUserVtotalIo *io,JcosU32 f,JcosU32 *v){
 *v=0;return io->exchange(io->context,POLARIS_VTOTAL_READ,f,0,v) && *v!=~0U;
}
int polaris_user_vtotal(const PolarisUserVtotalIo *io,int restore,JcosU32 *delta){
 if(!delta)return 0;*delta=0;if(!io || !io->exchange || (restore!=0 && restore!=1))return 0;
 JcosU32 before[6],unused;
 for(JcosU32 f=0;f<6;++f)if(!read_value(io,f,&before[f]))return 0;
 if(before[3]!=0x80410311U || before[4]!=1124 || (before[5]&0xFF000000U))return 0;
 if(!restore){if(before[0] || before[1] || before[2])return 0;}
 else if((before[0]!=0 && before[0]!=1132) || (before[1]!=0 && before[1]!=1132) ||
  (before[2]!=0 && before[2]!=0x11) || (before[1] && !before[0]) || (before[2] && (!before[0] || !before[1])))return 0;
 for(JcosU32 n=0;n<3;++n){
  JcosU32 f=restore?(n+2)%3:n,value=restore?0:(n<2?1132:0x11);
  if(!io->exchange(io->context,POLARIS_VTOTAL_WRITE,f,value,&unused))return 0;
 }
 JcosU32 posted,baseline;
 if(!read_value(io,2,&posted) || posted!=(restore?0U:0x11U) || !read_value(io,5,&baseline) || (baseline&0xFF000000U))return 0;
 for(JcosU32 poll=0;poll<64;++poll){
  JcosU32 v[6];for(JcosU32 f=0;f<6;++f)if(!read_value(io,f,&v[f]))return 0;
  if(v[0]!=(restore?0U:1132U) || v[1]!=(restore?0U:1132U) || v[2]!=(restore?0U:0x11U) ||
   v[3]!=0x80410311U || v[4]!=1124 || (v[5]&0xFF000000U))return 0;
  JcosU32 progress=(v[5]-baseline)&0xFFFFFFU;if(progress>512)return 0;
  if(progress>=2){*delta=progress;return 1;}
  if(poll+1<64 && !io->exchange(io->context,POLARIS_VTOTAL_WAIT,0,0,&unused))return 0;
 }
 return 0;
}
