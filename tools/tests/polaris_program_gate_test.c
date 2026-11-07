#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_program_gate.h"
#include "../../user/drivers/amd_polaris_plane.h"
#include "../../user/drivers/amd_polaris_program.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"gate mode%u tamper%d bit%u line%d: %s\n",mode,tamper,bit,__LINE__,#x);exit(1);}}while(0)
static u32 mode,bit;
static int tamper=-1;
static bool payload_bit;
typedef struct {PolarisProgramGate gate;u32 requests,reads,writes,waits,frame;bool denied;} Model;
static Model m;
static PolarisProgramPlan plan(void) {
 PolarisProgramPlan p;memset(&p,0,sizeof(p));
 p.incarnation=11;p.observation_id=13;p.gpu_address=0xF400000000ULL;p.crtc=0x80410311;
 p.witness.values[0][1]=p.witness.values[1][1]=0x20;
 p.witness.values[0][2]=100;p.witness.values[1][2]=110;
 PolarisDisplayObservation o={0};o.surface_gpu_address=p.gpu_address;o.linear_span_bytes=0x300000;
 o.surface_width=o.pitch_pixels=1024;o.surface_height=768;o.depth=2;o.array_mode=1;
 for(u32 i=0;i<5;++i){u32 v;CHECK(polaris_plane_recipe_step(&o,&p.witness,0,i,&p.tags[i],&v));p.values[i]=v;}
 return p;
}
static bool admitted(void *p){return !((Model*)p)->denied;}
static bool read_io(void *p,PolarisRebindRead f,u32 *v) {
 Model *s=p;++s->reads;
 switch(f){
 case POLARIS_REBIND_LOW:*v=mode==5?1:0;break;
 case POLARIS_REBIND_HIGH:*v=0xF4;break;
 case POLARIS_REBIND_CRTC:*v=0x80410311;break;
 case POLARIS_REBIND_UPDATE:*v=mode==1&&!s->waits&&s->writes?POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK:0;break;
 case POLARIS_REBIND_FLIP:*v=0x20;break;
 case POLARIS_REBIND_FRAME:
  if(mode!=3)s->frame=(s->frame+(mode==4&&s->writes&&s->reads>8?513:3))&POLARIS_CRTC_FRAME_COUNT_MASK;
  *v=s->frame;break;
 default:return false;
 }
 return true;
}
static bool write_io(void *p,PolarisDisplayRegister f,u32 v){
 Model *s=p;CHECK(s->writes<2 && f==(s->writes?POLARIS_REG_PRIMARY_LOW:POLARIS_REG_PRIMARY_HIGH) && v==(s->writes?0:0xF4));++s->writes;return true;
}
static bool wait_io(void *p){++((Model*)p)->waits;return true;}
static int exchange(void *p,JcosU32 operation,JcosU32 field,JcosU32 value,JcosU32 *out){
 Model *s=p;u64 tag=POLARIS_REBIND_TAG(s->requests+1,operation,field),payload=value;
 bool corrupt=(int)s->requests==tamper;++s->requests;
 if(corrupt){if(payload_bit)payload^=1ULL<<bit;else tag^=1ULL<<bit;}
 PolarisProgramIo io={s,admitted,write_io,read_io,wait_io};
 u32 before=s->reads+s->writes+s->waits;
 bool ok=polaris_program_gate_step(&s->gate,&io,tag,payload,out);
 if(corrupt){CHECK(!ok&&!*out&&s->gate.poisoned&&before==s->reads+s->writes+s->waits);
  CHECK(!polaris_program_gate_step(&s->gate,&io,POLARIS_REBIND_TAG(s->requests,operation,field),value,out));}
 return ok;
}
static int run(void){
 memset(&m,0,sizeof(m));m.frame=mode==2?0xFFFFF9:100;
 PolarisProgramPlan p=plan();CHECK(polaris_program_gate_init(&m.gate,&p)&&p.consumed);
 PolarisUserProgramIo io={&m,exchange};u32 delta=~0U;
 int ok=polaris_user_program(&io,0x20,&delta);
 CHECK(m.writes<=2&&m.gate.result.write_attempts==m.writes&&m.gate.result.reads==m.reads);
 if(ok){CHECK(m.gate.result.complete&&delta==m.gate.result.frame_delta&&delta>=2&&delta<=512);
  u32 out=1;PolarisProgramIo k={&m,admitted,write_io,read_io,wait_io};unsigned before=m.reads;
  CHECK(!polaris_program_gate_step(&m.gate,&k,1,0,&out)&&!out&&m.reads==before);}
 else CHECK(!delta);
 return ok;
}
int main(void){
 for(mode=0;mode<6;++mode){tamper=-1;CHECK(run()==(mode<3));
  if(mode==3)CHECK(m.gate.result.polls==64&&m.waits==63&&m.requests==POLARIS_REBIND_MESSAGES);}
 mode=0;
 for(tamper=0;tamper<16;++tamper)for(bit=0;bit<64;++bit)for(unsigned which=0;which<2;++which){payload_bit=which!=0;CHECK(!run());}
 tamper=-1;
 for(unsigned row=0;row<2;++row)for(unsigned i=0;i<5;++i)for(bit=0;bit<64;++bit){
  PolarisProgramPlan p=plan();if(row)p.values[i]^=1ULL<<bit;else p.tags[i]^=1ULL<<bit;
  PolarisProgramGate g;CHECK(!polaris_program_gate_init(&g,&p)&&p.consumed);
 }
 puts("PASS: real Ring3 sequencer/independent kernel gate;pending/wrap/exhaustion and bad-state paths;2048 request-tag/payload bit refusals before IO;640 plan corruptions;no replay or device access.");
 return 0;
}
