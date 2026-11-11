#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../user/drivers/amd_polaris_stream.h"
#include "../../kernel/tests/polaris_stream_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"STREAM line%d:%s\n",__LINE__,#x);return 1;}}while(0)
static JcosU32 v[96];static JcosU64 out[14];
static int failed(PolarisStreamStatus status){
 memset(out,0xA5,sizeof(out));CHECK(!polaris_stream_observe(v,96,out) && out[0]==status);
 for(unsigned i=1;i<14;++i)CHECK(!out[i]);return 0;
}
int main(void){
 polaris_stream_fixture(v);CHECK(polaris_stream_observe(v,96,out));for(unsigned i=0;i<14;++i)CHECK(out[i]==polaris_stream_golden[i]);
 const unsigned offsets[5]={0x13290,0x13294,0x13000,0x6EF4,0x6EF8};
 for(unsigned f=0;f<5;++f){JcosU32 x=~0U;CHECK(jcos_polaris_stream_offset(f,offsets[f]+4,&x) && x==offsets[f]);CHECK(!jcos_polaris_stream_offset(f,offsets[f]+3,&x)&&!x);}
 JcosU32 x=~0U;CHECK(!jcos_polaris_stream_offset(5,~0ULL,&x)&&!x);CHECK(!jcos_polaris_stream_offset(~0U,~0ULL,&x)&&!x);CHECK(!jcos_polaris_stream_offset(0,~0ULL,0));
 for(unsigned i=0;i<96;++i){polaris_stream_fixture(v);v[i]=~0U;CHECK(!failed(POLARIS_STREAM_ALL_ONES));}
 for(unsigned i=86;i<96;++i){polaris_stream_fixture(v);v[i]^=1;CHECK(!failed(POLARIS_STREAM_CHANGED));}
 for(unsigned i=0;i<86;++i){
  polaris_stream_fixture(v);v[i]^=1;
  if(i==39 || i==73){CHECK(polaris_stream_observe(v,96,out));continue;}
  CHECK(!failed(POLARIS_STREAM_BASELINE));
 }
 for(unsigned f=0;f<5;f+=2){if(f==4)break;polaris_stream_fixture(v);v[86+f]^=1;v[91+f]^=1;CHECK(!failed(POLARIS_STREAM_ROUTE));}
 /* Preserve every raw FIFO/lock/mode bit without inventing a clearing recipe. */
 for(unsigned f=1;f<5;++f){if(f==2)continue;for(unsigned bit=0;bit<32;++bit){
  polaris_stream_fixture(v);v[86+f]=v[91+f]=1U<<bit;CHECK(polaris_stream_observe(v,96,out));
  unsigned expected=f==1?(bit?POLARIS_STREAM_FIFO_OTHER:POLARIS_STREAM_FIFO_RESET):f==3?(bit?POLARIS_STREAM_UPDATE_OTHER:POLARIS_STREAM_UPDATE_HELD):(bit<3?0:POLARIS_STREAM_UPDATE_OTHER);
  CHECK(out[13]==expected);CHECK(f==1?(out[10]>>32)==(1ULL<<bit):f==3?(out[11]>>32)==(1ULL<<bit):out[12]==(1ULL<<bit));
 }}
 unsigned char *guard;
#ifdef _WIN32
 guard=VirtualAlloc(0,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(guard);DWORD previous;CHECK(VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous));
#else
 guard=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+4096,4096,PROT_NONE));
#endif
 polaris_stream_fixture(v);
 for(unsigned count=0;count<=96;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);memset(out,0xA5,sizeof(out));CHECK(polaris_stream_observe(p,count,out)==(count==96));for(unsigned i=0;i<14;++i)CHECK(out[i]==(count==96?polaris_stream_golden[i]:i?0:POLARIS_STREAM_INVALID));}
 for(unsigned count=0;count<=86;++count){JcosU32 *p=(void *)(guard+4096-count*4);memcpy(p,v,count*4);CHECK(jcos_polaris_stream_read_admitted(p,count)==(count==86));}
 CHECK(!polaris_stream_observe((void *)1,97,out));CHECK(!polaris_stream_observe(0,96,out));CHECK(!polaris_stream_observe(v,96,0));CHECK(!jcos_polaris_stream_read_admitted((void *)1,87));
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 puts("POLARIS STREAM: PASS;14 independent golden words,5offset edges,96allones,10changed,84baseline refusals/2autoM values,2route mismatches,96raw state bits,97+87 protected prefixes;noGPU IO or write plan");return 0;
}
