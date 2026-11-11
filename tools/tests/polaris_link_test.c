#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../user/drivers/amd_polaris_link.h"
#include "../../kernel/tests/polaris_link_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"LINK line%d:%s\n",__LINE__,#x);return 1;}}while(0)
static JcosU32 v[52];static JcosU64 out[4];
static int failed(PolarisLinkStatus status){
 memset(out,0xA5,sizeof(out));CHECK(!polaris_link_observe(v,52,out) && out[0]==status);
 for(unsigned i=1;i<4;++i)CHECK(!out[i]);return 0;
}
int main(void){
 const JcosU64 golden[4]={0x0001040200010000ULL,0x0000010500000010ULL,0x0100000250000200ULL,0x0000003400010001ULL};
 polaris_link_fixture(v);CHECK(polaris_link_observe(v,52,out));for(unsigned i=0;i<4;++i)CHECK(out[i]==golden[i]);
 unsigned offsets=0;
 for(unsigned unit=0;unit<7;++unit)for(unsigned field=0;field<3;++field){
  JcosU32 offset=~0U;int valid=!(unit==6 && field==2);JcosU32 expected=((unit==6 ? 0x5400U:0x4A00U+unit*0x100U)+(field==0 ? 0x48U:field==1 ? 0x47U:0))*4;
  CHECK(jcos_polaris_link_offset(unit,(PolarisLinkRegister)field,0x40000,&offset)==valid);
  if(valid){CHECK(offset==expected);CHECK(jcos_polaris_link_offset(unit,(PolarisLinkRegister)field,expected+4,&offset) && offset==expected);CHECK(!jcos_polaris_link_offset(unit,(PolarisLinkRegister)field,expected+3,&offset) && !offset);++offsets;}else CHECK(!offset);
 }
 JcosU32 offset=~0U;CHECK(!jcos_polaris_link_offset(7,POLARIS_LINK_ENABLE,0x40000,&offset) && !offset);CHECK(!jcos_polaris_link_offset(0,(PolarisLinkRegister)3,0x40000,&offset) && !offset);CHECK(!jcos_polaris_link_offset(~0U,POLARIS_LINK_ENABLE,~0ULL,&offset) && !offset);CHECK(!jcos_polaris_link_offset(0,POLARIS_LINK_ENABLE,0x40000,0));
 for(unsigned i=0;i<52;++i){polaris_link_fixture(v);v[i]=~0U;CHECK(!failed(POLARIS_LINK_ALL_ONES));}
 for(unsigned i=0;i<26;++i){polaris_link_fixture(v);v[i]^=1U;CHECK(!failed(POLARIS_LINK_CHANGED));}
 polaris_link_fixture(v);v[2]=v[48]=0;CHECK(!failed(POLARIS_LINK_NO_CRTC));
 polaris_link_fixture(v);v[1]=v[47]=0x10001;CHECK(!failed(POLARIS_LINK_CRTC_AMBIGUOUS));
 polaris_link_fixture(v);v[2]=v[48]=1;CHECK(!failed(POLARIS_LINK_CRTC_UNSUPPORTED));
 polaris_link_fixture(v);v[2]=v[48]=0x1010001;CHECK(!failed(POLARIS_LINK_CRTC_UNSUPPORTED));
 polaris_link_fixture(v);v[14]=v[34]=0;CHECK(!failed(POLARIS_LINK_NO_BACKEND));
 polaris_link_fixture(v);v[18]=v[38]=1;CHECK(!failed(POLARIS_LINK_BACKEND_AMBIGUOUS));
 const unsigned selectors[3]={0,3,64};
 for(unsigned i=0;i<3;++i){polaris_link_fixture(v);v[15]=v[35]=selectors[i]<<8;CHECK(!failed(POLARIS_LINK_SELECTOR));}
 polaris_link_fixture(v);v[21]=v[41]=1;CHECK(!failed(POLARIS_LINK_SELECTOR));
 /* BackendG is valid;frontendG is deliberately outside six-stream hardware. */
 polaris_link_fixture(v);v[14]=v[34]=0;v[18]=v[38]=1;v[19]=v[39]=0x50000200;CHECK(polaris_link_observe(v,52,out) && ((out[0]>>40)&255)==6 && (out[1]&0xffffffffU)==64);
 polaris_link_fixture(v);for(unsigned mode=0;mode<8;++mode){v[15]=v[35]=0x50000200U|(mode<<16);CHECK(polaris_link_observe(v,52,out) && (out[0]>>56)==mode);}
 unsigned char *guard;
#ifdef _WIN32
 guard=VirtualAlloc(0,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(guard);DWORD previous;CHECK(VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous));
#else
 guard=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+4096,4096,PROT_NONE));
#endif
 polaris_link_fixture(v);
 for(unsigned count=0;count<=52;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);memset(out,0xA5,sizeof(out));CHECK(polaris_link_observe(p,count,out)==(count==52));for(unsigned i=0;i<4;++i)CHECK(out[i]==(count==52 ? golden[i]:i ? 0:POLARIS_LINK_INVALID));}
 CHECK(!polaris_link_observe((void *)1,53,out) && out[0]==POLARIS_LINK_INVALID);CHECK(!polaris_link_observe(0,52,out) && out[0]==POLARIS_LINK_INVALID);CHECK(!polaris_link_observe(v,52,0));
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 printf("POLARIS LINK: PASS;%u golden offsets/aperture edges,52allones,26changed positions,ambiguity/mux/CRTC/refusal cases,backendG/rawmodes,53protectedprefixes;no deviceaccess\n",offsets);return 0;
}