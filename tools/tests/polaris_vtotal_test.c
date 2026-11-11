#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_vtotal_gate.h"
#include "../../kernel/tests/polaris_vtotal_actual_fixture.h"
#include "../../user/drivers/amd_polaris_vtotal.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"VTOTAL role%d mode%u tamper%d bit%u line%d: %s\n",restore,mode,tamper,bit,__LINE__,#x);exit(1);}}while(0)
static int restore,tamper=-1;static unsigned mode,bit,payload_bit,prefix;
typedef struct {PolarisVtotalGate gate;unsigned reg[3],requests,reads,writes,waits,frame;bool denied;} Model;
static Model m;
static bool admitted(void *p){return !((Model*)p)->denied;}
static bool read_io(void *p,u32 f,u32 *v){
 Model *s=p;++s->reads;
 if(f<3)*v=s->reg[f];else if(f==3)*v=0x80410311;else if(f==4)*v=1124;
 else if(f==5){if(mode!=2)s->frame=(s->frame+(mode==3 && s->writes && s->reads>8?513:3))&0xFFFFFFU;*v=s->frame;}else return false;
 if(mode==4 && f==2 && s->writes==3)*v^=1;
 return true;
}
static bool write_io(void *p,u32 f,u32 v){
 Model *s=p;CHECK(s->writes<3 && f==(restore?(s->writes+2)%3:s->writes) && v==(restore?0:s->writes<2?1132:0x11));
 s->reg[f]=v;++s->writes;return mode!=5;
}
static bool wait_io(void *p){++((Model*)p)->waits;return true;}
static int exchange(void *p,JcosU32 op,JcosU32 f,JcosU32 v,JcosU32 *out){
 Model *s=p;u64 tag=POLARIS_VTOTAL_TAG(s->requests+1,op,f),payload=v;
 bool corrupt=(int)s->requests==tamper;++s->requests;
 if(corrupt){if(payload_bit)payload^=1ULL<<bit;else tag^=1ULL<<bit;}
 PolarisVtotalIo io={s,admitted,read_io,write_io,wait_io};unsigned before=s->reads+s->writes+s->waits;
 bool ok=polaris_vtotal_gate_step(&s->gate,&io,tag,payload,out);
 if(corrupt){CHECK(!ok && !*out && s->gate.poisoned && before==s->reads+s->writes+s->waits);CHECK(!polaris_vtotal_gate_step(&s->gate,&io,POLARIS_VTOTAL_TAG(s->requests,op,f),v,out));}
 return ok;
}
static int run(void){
 memset(&m,0,sizeof(m));m.frame=mode==1?0xFFFFF9:100;
 if(restore){if(prefix>=1)m.reg[0]=1132;if(prefix>=2)m.reg[1]=1132;if(prefix>=3)m.reg[2]=0x11;}
 polaris_vtotal_gate_init(&m.gate,restore!=0);PolarisUserVtotalIo io={&m,exchange};u32 delta=~0U;
 int ok=polaris_user_vtotal(&io,restore,&delta);
 CHECK(m.writes<=3 && m.gate.result.write_attempts==m.writes && m.gate.result.read_attempts==m.reads);
 if(ok){CHECK(m.gate.result.complete && delta==m.gate.result.frame_delta && delta>=2 && delta<=512);unsigned before=m.reads;u32 out=1;PolarisVtotalIo k={&m,admitted,read_io,write_io,wait_io};CHECK(!polaris_vtotal_gate_step(&m.gate,&k,1,0,&out)&&!out&&m.reads==before);}
 else CHECK(!delta);
 return ok;
}
int main(void){
 for(restore=0;restore<2;++restore){prefix=restore?3:0;
  for(mode=0;mode<6;++mode){CHECK(run()==(mode<2));if(mode==2)CHECK(m.gate.result.polls==64 && m.waits==63 && m.requests==458 && m.gate.result.read_attempts==392);}
  mode=0;for(tamper=0;tamper<17;++tamper)for(bit=0;bit<64;++bit)for(payload_bit=0;payload_bit<2;++payload_bit)CHECK(!run());tamper=-1;
 }
 restore=1;mode=0;for(prefix=0;prefix<=3;++prefix){CHECK(run());CHECK(!m.reg[0]&&!m.reg[1]&&!m.reg[2]);}
 const unsigned invalid[][3]={{1,0,0},{0,1132,0},{1132,0,0x11},{1132,1132,1},{1133,1132,0},{1132,1133,0},{1132,1132,0x10011}};
 for(unsigned n=0;n<sizeof(invalid)/sizeof(invalid[0]);++n){memset(&m,0,sizeof(m));memcpy(m.reg,invalid[n],sizeof(m.reg));polaris_vtotal_gate_init(&m.gate,true);PolarisUserVtotalIo io={&m,exchange};u32 delta;CHECK(!polaris_user_vtotal(&io,1,&delta)&&!m.writes&&!delta);}
 u32 snapshot[86];polaris_vtotal_actual_fixture(snapshot);CHECK(jcos_polaris_vtotal_snapshot_admitted(snapshot,86));
 for(unsigned i=0;i<86;++i)for(bit=0;bit<32;++bit){if((i==39||i==73)&&bit<24)continue;polaris_vtotal_actual_fixture(snapshot);snapshot[i]^=1U<<bit;CHECK(!jcos_polaris_vtotal_snapshot_admitted(snapshot,86));}
 for(unsigned count=0;count<86;++count)CHECK(!jcos_polaris_vtotal_snapshot_admitted((void*)1,count));
 CHECK(!jcos_polaris_vtotal_snapshot_admitted((void*)1,87));CHECK(!jcos_polaris_vtotal_snapshot_admitted(0,86));
 puts("POLARIS VTOTAL: PASS;primary/rescue success,wrap,64poll exhaustion,readback/store/frame failures;4352 request tag/payload corruptions before IO;all4 interrupted primary prefixes restored;7 invalid prefixes refuse;2704 snapshot bit corruptions;no GPU access");return 0;
}
