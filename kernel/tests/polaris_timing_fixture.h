#ifndef JCOS_POLARIS_TIMING_FIXTURE_H
#define JCOS_POLARIS_TIMING_FIXTURE_H
#include "../../include/polaris_timing_protocol.h"
static inline void polaris_timing_fixture(JcosU32 v[POLARIS_TIMING_VALUES]){
 for(JcosU32 i=0;i<POLARIS_TIMING_VALUES;++i)v[i]=0;
 v[0]=v[74]=0x80410311;
 for(JcosU32 i=1;i<6;++i)v[i]=v[74+i]=0x80400310;
 v[10]=v[44]=0x101;v[11]=v[45]=0x400;v[22]=v[56]=0x1000000;
 const JcosU32 timing[14]={1,1343,(296U<<16)|1320,136U<<16,1,805,(35U<<16)|803,6U<<16,1,0,0x1000000,0x100,0x8000,0x335B};
 for(JcosU32 i=0;i<14;++i)v[26+i]=v[60+i]=timing[i];
}
#endif
