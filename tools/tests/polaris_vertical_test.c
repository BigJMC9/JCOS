#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../user/drivers/amd_polaris_vertical.h"
#include "../../kernel/tests/polaris_vertical_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"VERTICAL line%d:%s\n",__LINE__,#x);return 1;}}while(0)
static JcosU32 v[86];static JcosU64 out[18];
static int failed(PolarisVerticalStatus status){
 memset(out,0xA5,sizeof(out));CHECK(!polaris_vertical_plan(v,86,out) && out[0]==status);
 for(unsigned i=1;i<18;++i)CHECK(!out[i]);return 0;
}
int main(void){
 const JcosU64 golden[18]={0x10000ULL,0x0002020000020000ULL,0x00C0084007800898ULL,0x0029046104380465ULL,0x00050000002C0000ULL,0,0x0000010002000000ULL,0x0000758D00008000ULL,0x0000005000010201ULL,0x0000758F0000758DULL,0x0000046400000464ULL,0x0000046C00000000ULL,0x0000046C00000000ULL,0x0000046C00000001ULL,0x0000001100000002ULL,0x0000000000000002ULL,0x0000046400000000ULL,0x0000046400000001ULL};
 polaris_vertical_fixture(v);CHECK(polaris_vertical_plan(v,86,out));for(unsigned i=0;i<18;++i)CHECK(out[i]==golden[i]);
 const unsigned offsets[3]={0x6E20,0x6E24,0x6E28};
 for(unsigned f=0;f<3;++f){JcosU32 x=~0U;CHECK(jcos_polaris_vertical_offset(f,offsets[f]+4,&x) && x==offsets[f]);CHECK(!jcos_polaris_vertical_offset(f,offsets[f]+3,&x) && !x);}
 JcosU32 x=~0U;CHECK(!jcos_polaris_vertical_offset(3,~0ULL,&x) && !x);CHECK(!jcos_polaris_vertical_offset(~0U,~0ULL,&x) && !x);CHECK(!jcos_polaris_vertical_offset(0,~0ULL,0));
 for(unsigned i=0;i<86;++i){polaris_vertical_fixture(v);v[i]=~0U;CHECK(!failed(POLARIS_VERTICAL_ALL_ONES));}
 for(unsigned i=0;i<6;++i){polaris_vertical_fixture(v);v[80+i]^=1;CHECK(!failed(POLARIS_VERTICAL_CHANGED));}
 for(unsigned f=0;f<3;++f)for(unsigned bit=0;bit<32;++bit){polaris_vertical_fixture(v);v[80+f]=v[83+f]=1U<<bit;CHECK(!failed(POLARIS_VERTICAL_CONTROLS));}
 for(unsigned mask=0;mask<4;++mask){
  polaris_vertical_fixture(v);v[80]=v[83]=(mask&1)?0:1124;v[81]=v[84]=(mask&2)?0:1124;
  CHECK(polaris_vertical_plan(v,86,out));CHECK((unsigned)out[10]==v[80] && (unsigned)(out[10]>>32)==v[81]);
  CHECK(out[16]==((JcosU64)v[80]<<32) && out[17]==(1ULL|((JcosU64)v[81]<<32)));
  for(unsigned prefix=0;prefix<=3;++prefix){
   unsigned state[3]={v[80],v[81],0};
   for(unsigned i=0;i<prefix;++i){unsigned field=(unsigned)out[12+i];CHECK(field<3);state[field]=(unsigned)(out[12+i]>>32);}
   for(unsigned i=0;i<3;++i){unsigned field=(unsigned)out[15+i];CHECK(field<3);state[field]=(unsigned)(out[15+i]>>32);CHECK(!state[2]);}
   CHECK(state[0]==v[80] && state[1]==v[81] && !state[2]);
  }
 }
 const unsigned fields[]={27,28,29,30,31,32,33,34,35,36,37,38};
 for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i){polaris_vertical_fixture(v);v[fields[i]]^=1;v[fields[i]+34]^=1;CHECK(!polaris_vertical_plan(v,86,out));for(unsigned j=1;j<18;++j)CHECK(!out[j]);}
 polaris_vertical_fixture(v);v[39]=v[73]=0;CHECK(!failed(POLARIS_VERTICAL_PROFILE));
 polaris_vertical_fixture(v);v[39]|=0x1000000;v[73]|=0x1000000;CHECK(!failed(POLARIS_VERTICAL_PROFILE));
 polaris_vertical_fixture(v);v[33]^=1;CHECK(!failed(POLARIS_VERTICAL_TIMING));
 polaris_vertical_fixture(v);CHECK(jcos_polaris_vertical_read_admitted(v,80));
 for(unsigned i=0;i<80;++i){polaris_vertical_fixture(v);v[i]=~0U;CHECK(!jcos_polaris_vertical_read_admitted(v,80));}
 for(unsigned i=0;i<39;++i){polaris_vertical_fixture(v);v[i]^=1;CHECK(!jcos_polaris_vertical_read_admitted(v,80));}
 unsigned char *guard;
#ifdef _WIN32
 guard=VirtualAlloc(0,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(guard);DWORD previous;CHECK(VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous));
#else
 guard=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+4096,4096,PROT_NONE));
#endif
 polaris_vertical_fixture(v);
 for(unsigned count=0;count<=86;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);memset(out,0xA5,sizeof(out));CHECK(polaris_vertical_plan(p,count,out)==(count==86));for(unsigned i=0;i<18;++i)CHECK(out[i]==(count==86?golden[i]:i?0:POLARIS_VERTICAL_INVALID));}
 for(unsigned count=0;count<=80;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);CHECK(jcos_polaris_vertical_read_admitted(p,count)==(count==80));}
 CHECK(!polaris_vertical_plan((void *)1,87,out));CHECK(!polaris_vertical_plan(0,86,out));CHECK(!polaris_vertical_plan(v,86,0));CHECK(!jcos_polaris_vertical_read_admitted(0,80));CHECK(!jcos_polaris_vertical_read_admitted((void *)1,81));
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 puts("POLARIS VERTICAL: PASS;18 independent golden words,3 offset edges,86 allones,6 changed controls,96 paired unsafe controls,4 baseline pairs/16 interrupted prefixes restored,12 profile mutations,80 admission allones/39 changes,87+81 protected prefixes;RAM only, no GPU writes");return 0;
}
