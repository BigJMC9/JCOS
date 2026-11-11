#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../user/drivers/amd_polaris_timing.h"
#include "../../kernel/tests/polaris_timing_fixture.h"
#include "../../kernel/tests/polaris_timing_actual_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"TIMING line%d:%s\n",__LINE__,#x);return 1;}}while(0)
static JcosU32 v[80];static JcosU64 out[9];
static int failed(PolarisTimingStatus status){
 memset(out,0xA5,sizeof(out));CHECK(!polaris_timing_observe(v,80,out) && out[0]==status);
 for(unsigned i=1;i<9;++i)CHECK(!out[i]);return 0;
}
int main(void){
 const JcosU64 golden[9]={0x0002020000020000ULL,0x0128052804000540ULL,0x0023032303000326ULL,0x0006000000880000ULL,0x0000000000000101ULL,0x0000010001000000ULL,0x0000335B00008000ULL,0x0000005000000001ULL,0x0000335B0000335BULL};
 polaris_timing_fixture(v);CHECK(polaris_timing_observe(v,80,out));for(unsigned i=0;i<9;++i)CHECK(out[i]==golden[i]);
 const JcosU32 offsets[14]={0x13290,0x6E00,0x6E04,0x6E08,0x6E0C,0x6E1C,0x6E34,0x6E38,0x6E3C,0x6E78,0x13284,0x132A0,0x132A4,0x132A8};
 for(unsigned f=0;f<14;++f){JcosU32 offset=~0U;CHECK(jcos_polaris_timing_offset(f,0x40000,&offset) && offset==offsets[f]);CHECK(jcos_polaris_timing_offset(f,offsets[f]+4,&offset));CHECK(!jcos_polaris_timing_offset(f,offsets[f]+3,&offset) && !offset);}
 JcosU32 offset=~0U;CHECK(!jcos_polaris_timing_offset(14,~0ULL,&offset) && !offset);CHECK(!jcos_polaris_timing_offset(~0U,~0ULL,&offset) && !offset);CHECK(!jcos_polaris_timing_offset(0,0x40000,0));
 for(unsigned i=0;i<80;++i){polaris_timing_fixture(v);v[i]=~0U;CHECK(!failed(POLARIS_TIMING_ALL_ONES));}
 for(unsigned i=0;i<40;++i){if(i==39)continue;polaris_timing_fixture(v);v[i]^=1U;CHECK(!failed(POLARIS_TIMING_CHANGED));}
 /* Paired invalid values exercise validation,not the earlier stability gate. */
 const struct {unsigned field,value;PolarisTimingStatus status;} bad[]={
 {0,0,POLARIS_TIMING_UNSUPPORTED},{9,1,POLARIS_TIMING_UNSUPPORTED},
 {1,0,POLARIS_TIMING_GEOMETRY},{2,(1321U<<16)|1320,POLARIS_TIMING_GEOMETRY},
 {2,(296U<<16)|1344,POLARIS_TIMING_GEOMETRY},{3,(136U<<16)|136,POLARIS_TIMING_GEOMETRY},
 {3,297U<<16,POLARIS_TIMING_GEOMETRY},{5,0,POLARIS_TIMING_GEOMETRY},
 {6,(804U<<16)|803,POLARIS_TIMING_GEOMETRY},{6,(35U<<16)|806,POLARIS_TIMING_GEOMETRY},
 {7,(6U<<16)|6,POLARIS_TIMING_GEOMETRY},{7,36U<<16,POLARIS_TIMING_GEOMETRY}};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){polaris_timing_fixture(v);v[26+bad[i].field]=v[60+bad[i].field]=bad[i].value;CHECK(!failed(bad[i].status));}
 polaris_timing_fixture(v);v[11]=v[45]=0x10400;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[22]=v[56]=0;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[1]=v[75]=0x10001;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[10]=v[44]=0;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[11]=v[45]=0x200;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[22]=v[56]=0x1000001;CHECK(!failed(POLARIS_TIMING_LINK));
 polaris_timing_fixture(v);v[28]=v[62]=(296U<<16)|1319;CHECK(polaris_timing_observe(v,80,out) && ((out[1]>>16)&65535)==1023);
 polaris_timing_fixture(v);v[27]=v[61]=2199;v[28]=v[62]=(192U<<16)|2112;v[29]=v[63]=44U<<16;
 v[31]=v[65]=1124;v[32]=v[66]=(41U<<16)|1121;v[33]=v[67]=5U<<16;
 CHECK(polaris_timing_observe(v,80,out) && ((out[1]>>16)&65535)==1920 && ((out[2]>>16)&65535)==1080);
 polaris_timing_fixture(v);CHECK(jcos_polaris_timing_read_admitted(v,26));
 for(unsigned i=0;i<26;++i){polaris_timing_fixture(v);v[i]=~0U;CHECK(!jcos_polaris_timing_read_admitted(v,26));}
 CHECK(!jcos_polaris_timing_read_admitted(0,26));CHECK(!jcos_polaris_timing_read_admitted((void *)1,27));

 const JcosU64 actual[9]={0x0002020000020000ULL,0x00C0084007800898ULL,0x0029046104380465ULL,0x00050000002C0000ULL,0x0000000000000000ULL,0x0000010002000000ULL,0x0000758D00008000ULL,0x0000005000010201ULL,0x0000758B0000758DULL};
 polaris_timing_actual_fixture(v);CHECK(polaris_timing_observe(v,80,out));for(unsigned i=0;i<9;++i)CHECK(out[i]==actual[i]);
 /* ManualM,staticN/GEN_EN and reservedM changes must still refuse. */
 polaris_timing_actual_fixture(v);v[37]=v[71]=0;CHECK(!failed(POLARIS_TIMING_CHANGED));
 polaris_timing_actual_fixture(v);v[73]^=0x1000000U;CHECK(!failed(POLARIS_TIMING_CHANGED));
 polaris_timing_actual_fixture(v);v[72]^=1U;CHECK(!failed(POLARIS_TIMING_CHANGED));
 polaris_timing_actual_fixture(v);v[71]^=POLARIS_TIMING_AUTO_MN_MASK;CHECK(!failed(POLARIS_TIMING_CHANGED));
 polaris_timing_fixture(v);v[37]=v[71]=0;CHECK(polaris_timing_observe(v,80,out) && out[8]==0x0000335B0000335BULL);
 polaris_timing_actual_fixture(v);v[39]=1;v[73]=0xFFFFFEU;CHECK(polaris_timing_observe(v,80,out) && out[8]==0x00FFFFFE00000001ULL);
 unsigned char *guard;
#ifdef _WIN32
 guard=VirtualAlloc(0,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(guard);DWORD previous;CHECK(VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous));
#else
 guard=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+4096,4096,PROT_NONE));
#endif
 polaris_timing_fixture(v);
 for(unsigned count=0;count<=80;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);memset(out,0xA5,sizeof(out));CHECK(polaris_timing_observe(p,count,out)==(count==80));for(unsigned i=0;i<9;++i)CHECK(out[i]==(count==80 ? golden[i]:i ? 0:POLARIS_TIMING_INVALID));}
 for(unsigned count=0;count<=26;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);CHECK(jcos_polaris_timing_read_admitted(p,count)==(count==26));}
 CHECK(!polaris_timing_observe((void *)1,81,out) && out[0]==POLARIS_TIMING_INVALID);CHECK(!polaris_timing_observe(0,80,out) && out[0]==POLARIS_TIMING_INVALID);CHECK(!polaris_timing_observe(v,80,0));
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 puts("POLARIS TIMING: PASS;14 golden offsets/aperture edges,80allones,39staticchanged,12geometry/stream/interlace,6route refusals,independentraster sizes,26admissionallones,actualautoM pair/manualM-N-GEN-reserved refusals,81+27protectedprefixes;no deviceaccess");return 0;
}
