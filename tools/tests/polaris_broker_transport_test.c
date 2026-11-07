#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_driver.h"
#include "../../kernel/polaris_program_gate.h"
#include "../../user/drivers/amd_polaris_plane.h"
#include "../../kernel/ipc.h"
#include "../../kernel/timer.h"
#include "../../kernel/interrupts.h"
#include "../../kernel/scheduler.h"
#include "../../kernel/device_mmio.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"broker case%d line%d %s\n",bad,__LINE__,#x);exit(1);}}while(0)
static int bad;
static PolarisDriver d;
static Process kernel;
static u64 ticks;
static unsigned index_,sends,receives,replies,reads,writes,frames;
static bool io_admitted(void *p){(void)p;return true;}
static bool io_read(void *p,PolarisRebindRead f,u32 *v){
 (void)p;++reads;if(bad==9&&writes)return false;
 switch(f){case 0:*v=bad==7?1:0;break;case 1:*v=0xF4;break;case 2:*v=0x80410311;break;
 case 3:*v=0;break;case 4:*v=0x20;break;case 5:frames+=3;*v=100+frames;break;default:return false;}return true;
}
static bool io_write(void *p,PolarisDisplayRegister f,u32 v){
 (void)p;CHECK(writes<2&&f==(writes?POLARIS_REG_PRIMARY_LOW:POLARIS_REG_PRIMARY_HIGH)&&v==(writes?0:0xF4));
 ++writes;return bad!=8;
}
static bool io_wait(void *p){(void)p;CHECK(0);return false;}
Process *process_kernel(void){return &kernel;}
bool timer_initialized(void){return true;}
u32 timer_frequency(void){return bad==14&&receives?101:100;}
u64 timer_ticks(void){ticks+=bad==13?201:1;return ticks;}
bool device_mmio_interrupts_enabled(void){return true;}
bool scheduler_preemption_enabled(void){return true;}
void interrupts_disable(void){}
void interrupts_enable(void){}
bool scheduler_yield(void){return bad==22;}
bool managed_service_connection_current(ManagedService *s,const ManagedServiceConnection *c){return s==&d.service&&c==&d.connection;}
u64 managed_service_process_id(ManagedService *s){(void)s;return bad==15&&receives?201:d.process_id;}
bool managed_service_idle(ManagedService *s){(void)s;return bad!=22;}
bool managed_service_send_oneway(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q){
 CHECK(s==&d.service&&c==&d.connection&&q->word_count==4&&q->words[0]==POLARIS_NATIVE_HEADER(POLARIS_NATIVE_EXECUTE));
 CHECK(q->words[1]==13&&!q->words[2]&&!q->words[3]);++sends;return bad!=10;
}
static u64 expected_tag(unsigned n){
 if(n<=6)return POLARIS_REBIND_TAG(n,POLARIS_REBIND_READ,n-1);
 if(n==7||n==8)return POLARIS_REBIND_TAG(n,POLARIS_REBIND_WRITE,n==7?POLARIS_REG_PRIMARY_HIGH:POLARIS_REG_PRIMARY_LOW);
 if(n<=10)return POLARIS_REBIND_TAG(n,POLARIS_REBIND_READ,n==9?0:5);
 return POLARIS_REBIND_TAG(n,POLARIS_REBIND_READ,n-11);
}
bool ipc_receive_blocking_for(Process *p,CapabilityHandle cap,IpcMessage *q,u64 timeout){
 CHECK(p==&kernel&&cap==61&&timeout&&timeout<=200);++receives;++index_;
 memset(q,0,sizeof(*q));q->word_count=4;q->words[1]=51;
 if(index_==17||bad==19){q->words[0]=POLARIS_NATIVE_REPLY;q->words[2]=POLARIS_NATIVE_EXECUTE;q->words[3]=3;
  if(bad==20)++q->words[3];if(bad==21)++q->words[2];return true;}
 q->words[0]=POLARIS_REBIND_REQUEST;q->words[2]=expected_tag(index_);q->words[3]=index_==7?0xF4:0;
 if(bad==1)q->word_count=3;if(bad==2)q->words[0]^=1;if(bad==3)q->words[1]^=1;
 if(bad==4)q->words[2]^=1ULL<<32;if(bad==5)q->words[3]^=1;if(bad==6)q->words[2]^=1;
 return bad!=11;
}
bool ipc_send_blocking_for(Process *p,CapabilityHandle cap,const IpcMessage *r,u64 timeout){
 CHECK(p==&kernel&&cap==62&&timeout&&timeout<=200);
 CHECK(r->word_count==4&&r->words[0]==POLARIS_REBIND_RESPONSE&&r->words[1]==51&&r->words[2]==expected_tag(index_));
 ++replies;return bad!=12;
}
static PolarisProgramPlan plan(void){
 PolarisProgramPlan p;memset(&p,0,sizeof(p));p.incarnation=51;p.observation_id=13;p.gpu_address=0xF400000000ULL;p.crtc=0x80410311;
 p.witness.values[0][1]=p.witness.values[1][1]=0x20;p.witness.values[0][2]=100;p.witness.values[1][2]=110;
 PolarisDisplayObservation o={0};o.surface_gpu_address=p.gpu_address;o.linear_span_bytes=0x300000;o.surface_width=o.pitch_pixels=1024;o.surface_height=768;o.depth=2;o.array_mode=1;
 for(u32 i=0;i<5;++i){u32 v;CHECK(polaris_plane_recipe_step(&o,&p.witness,0,i,&p.tags[i],&v));p.values[i]=v;}return p;
}
int main(void){
 for(bad=0;bad<=22;++bad){
  memset(&d,0,sizeof(d));d.process_id=200;d.connection.incarnation=51;d.connection.receive_handle=61;d.connection.send_handle=62;
  ticks=0;index_=sends=receives=replies=reads=writes=frames=0;
  PolarisProgramPlan p=plan();PolarisProgramResult r;
  if(bad==16)d.program_attempted=true;if(bad==17)++p.incarnation;if(bad==18)p.observation_id=0;
  PolarisProgramIo io={0,io_admitted,io_write,io_read,io_wait};
  bool ok=polaris_driver_program(&d,&p,&io,&r);
  CHECK(ok==(bad==0)&&writes<=2);
  if(!bad)CHECK(r.complete&&r.reads==14&&r.write_attempts==2&&r.polls==1&&r.frame_delta==3&&receives==17&&replies==16);
  if(bad>=1&&bad<=6)CHECK(!reads&&!writes);
  if(bad==8)CHECK(writes==1&&r.write_attempts==1&&!r.complete);
  if(bad==9)CHECK(writes==2&&!r.complete);
  if(bad>=16&&bad<=18)CHECK(!sends&&!receives&&!writes);
  if(bad==19)CHECK(!reads&&!writes&&!r.complete);
  if(bad==20||bad==21||bad==22)CHECK(writes==2&&r.complete&&!ok);
  if(d.program_attempted){unsigned sent=sends,recv=receives;
   CHECK(!polaris_driver_program(&d,&p,&io,&r)&&sent==sends&&recv==receives);}
 }
 puts("PASS: production private broker success plus22 malformed,partial-IO,deadline,identity,completion/readiness and no-replay cases;no hardware.");
 return 0;
}
