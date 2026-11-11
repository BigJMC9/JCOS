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
static unsigned diagnostic_prefix,diagnostic_tail;
static PolarisDriver d;
static Process kernel;
static u64 ticks;
static unsigned index_,sends,receives,replies,reads,writes,frames,reg[3];
static bool io_admitted(void *p){(void)p;return true;}
static bool io_read(void *p,u32 f,u32 *v){
 (void)p;++reads;if(bad==9&&writes)return false;
 if(f<3)*v=reg[f];else if(f==3)*v=0x80410311;else if(f==4)*v=1124;else if(f==5){frames+=3;*v=100+frames;}else return false;
 if(bad==7&&f==0)*v=1;return true;
}
static bool io_write(void *p,u32 f,u32 v){
 (void)p;CHECK(writes<3&&f==(diagnostic_prefix?(writes+2)%3:writes)&&v==(diagnostic_prefix?0:writes<2?1132U:0x11U));reg[f]=v;++writes;return bad!=8;
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
 CHECK(s==&d.service&&c==&d.connection&&q->word_count==4&&q->words[0]==POLARIS_NATIVE_HEADER(diagnostic_prefix?POLARIS_NATIVE_RESCUE_VTOTAL_FAULT:POLARIS_NATIVE_VTOTAL));
 CHECK(q->words[1]==(diagnostic_prefix?2:1)&&q->words[2]==diagnostic_prefix&&!q->words[3]);++sends;return bad!=10;
}
static u64 expected_tag(unsigned n){
 if(n<=6)return POLARIS_VTOTAL_TAG(n,POLARIS_VTOTAL_READ,n-1);
 if(n<=9)return POLARIS_VTOTAL_TAG(n,POLARIS_VTOTAL_WRITE,diagnostic_prefix?(n-7+2)%3:n-7);
 if(n<=11)return POLARIS_VTOTAL_TAG(n,POLARIS_VTOTAL_READ,n==10?2:5);
 return POLARIS_VTOTAL_TAG(n,POLARIS_VTOTAL_READ,n-12);
}
bool ipc_receive_blocking_for(Process *p,CapabilityHandle cap,IpcMessage *q,u64 timeout){
 CHECK(p==&kernel&&cap==61&&timeout&&timeout<=200);++receives;++index_;
 memset(q,0,sizeof(*q));q->word_count=4;q->words[1]=51;
 if(diagnostic_prefix&&index_==7+diagnostic_prefix){
  if(diagnostic_tail==0)return false; /* Actual fault / no next message. */
  if(diagnostic_tail==2){q->words[0]=POLARIS_NATIVE_REPLY;q->words[2]=POLARIS_NATIVE_RESCUE_VTOTAL_FAULT;q->words[3]=3;return true;}
  if(diagnostic_tail==3){q->words[0]=POLARIS_VTOTAL_REQUEST;q->words[2]=POLARIS_VTOTAL_TAG(index_,POLARIS_VTOTAL_WAIT,0);return true;}
  /* tail1 continues the valid sequence: next store (prefix1/2), or posted read
   * (prefix3). The production broker must stop this before any IO. */
 }
 if(index_==18||bad==19){q->words[0]=POLARIS_NATIVE_REPLY;q->words[2]=POLARIS_NATIVE_VTOTAL;q->words[3]=3;
  if(bad==20)++q->words[3];if(bad==21)++q->words[2];return true;}
 q->words[0]=POLARIS_VTOTAL_REQUEST;q->words[2]=expected_tag(index_);q->words[3]=diagnostic_prefix?0:index_==7||index_==8?1132:index_==9?0x11:0;
 if(bad==1)q->word_count=3;if(bad==2)q->words[0]^=1;if(bad==3)q->words[1]^=1;
 if(bad==4)q->words[2]^=1ULL<<32;if(bad==5)q->words[3]^=1;if(bad==6)q->words[2]^=1;
 return bad!=11;
}
bool ipc_send_blocking_for(Process *p,CapabilityHandle cap,const IpcMessage *r,u64 timeout){
 CHECK(p==&kernel&&cap==62&&timeout&&timeout<=200);
 CHECK(r->word_count==4&&r->words[0]==POLARIS_VTOTAL_RESPONSE&&r->words[1]==51&&r->words[2]==expected_tag(index_));
 ++replies;return bad!=12;
}
int main(void){
 for(bad=0;bad<=22;++bad){
  memset(&d,0,sizeof(d));d.process_id=200;d.connection.incarnation=51;d.connection.receive_handle=61;d.connection.send_handle=62;
  memset(reg,0,sizeof(reg));d.role=1;d.program_attempted=true;
  ticks=0;index_=sends=receives=replies=reads=writes=frames=0;
  PolarisVtotalResult r;
  if(bad==16)d.vtotal_attempted=true;if(bad==17)d.retained_only=true;if(bad==18)d.program_attempted=false;
  PolarisVtotalIo io={0,io_admitted,io_read,io_write,io_wait};
  bool ok=polaris_driver_vtotal(&d,&io,0,&r);
  CHECK(ok==(bad==0)&&writes<=3);
  if(!bad)CHECK(r.complete&&r.reads==14&&r.write_attempts==3&&r.polls==1&&r.frame_delta==3&&receives==18&&replies==17);
  if(bad>=1&&bad<=6)CHECK(!reads&&!writes);
  if(bad==8)CHECK(writes==1&&r.write_attempts==1&&!r.complete);
  if(bad==9)CHECK(writes==3&&!r.complete);
  if(bad>=16&&bad<=18)CHECK(!sends&&!receives&&!writes);
  if(bad==19)CHECK(!reads&&!writes&&!r.complete);
  if(bad==20||bad==21||bad==22)CHECK(writes==3&&r.complete&&!ok);
  if(d.vtotal_attempted){unsigned sent=sends,recv=receives;
   CHECK(!polaris_driver_vtotal(&d,&io,0,&r)&&sent==sends&&recv==receives);}
 }
 for(diagnostic_prefix=1;diagnostic_prefix<=3;++diagnostic_prefix){
  for(diagnostic_tail=0;diagnostic_tail<4;++diagnostic_tail){
   bad=0;memset(&d,0,sizeof(d));d.process_id=200;d.connection.incarnation=51;d.connection.receive_handle=61;d.connection.send_handle=62;d.role=2;
   reg[0]=reg[1]=1132;reg[2]=17;ticks=0;index_=sends=receives=replies=reads=writes=frames=0;
   PolarisVtotalIo io={0,io_admitted,io_read,io_write,io_wait};PolarisVtotalResult r;
   CHECK(!polaris_driver_rescue_vtotal_fault(&d,&io,diagnostic_prefix,&r));
   CHECK(d.vtotal_attempted&&writes==diagnostic_prefix&&reads==6&&r.reads==6&&r.write_attempts==diagnostic_prefix&&!r.polls&&!r.complete);
   CHECK(receives==7+diagnostic_prefix&&replies==6+diagnostic_prefix);
   unsigned sent=sends,recv=receives;
   CHECK(!polaris_driver_vtotal(&d,&io,0,&r)&&!polaris_driver_rescue_vtotal_fault(&d,&io,diagnostic_prefix,&r)&&sent==sends&&recv==receives);
  }
  for(bad=1;bad<=6;++bad){
   memset(&d,0,sizeof(d));d.process_id=200;d.connection.incarnation=51;d.connection.receive_handle=61;d.connection.send_handle=62;d.role=2;
   reg[0]=reg[1]=1132;reg[2]=17;ticks=0;index_=sends=receives=replies=reads=writes=frames=0;
   PolarisVtotalIo io={0,io_admitted,io_read,io_write,io_wait};PolarisVtotalResult r;
   CHECK(!polaris_driver_rescue_vtotal_fault(&d,&io,diagnostic_prefix,&r)&&!reads&&!writes);
  }
 }
 diagnostic_prefix=1;diagnostic_tail=0;bad=0;
 for(unsigned guard=0;guard<9;++guard){
  memset(&d,0,sizeof(d));d.process_id=200;d.role=2;d.connection.incarnation=51;d.connection.receive_handle=61;d.connection.send_handle=62;
  unsigned prefix=1;
  if(guard==0)prefix=0;if(guard==1)prefix=4;if(guard==2)d.role=1;if(guard==3)d.role=3;
  if(guard==4)d.program_attempted=true;if(guard==5)d.retained_only=true;if(guard==6)d.vtotal_attempted=true;if(guard==7)d.process_id=0;
  ticks=0;index_=sends=receives=replies=reads=writes=frames=0;PolarisVtotalIo io={0,io_admitted,io_read,io_write,io_wait};PolarisVtotalResult r;
  CHECK(!polaris_driver_rescue_vtotal_fault(&d,guard==8?0:&io,prefix,&r)&&!sends&&!receives&&!reads&&!writes);
 }
 puts("POLARIS RESCUE PREFIX TRANSPORT: PASS;all3prefixes x fault/continued-valid-IO/forged-completion/wait tails;18malformed-first-request refusals;9admission guards;noIOpastprefix/no completion/replay;no hardware.");
 puts("POLARIS VTOTAL TRANSPORT: PASS;production broker success plus22 envelope,partial-IO,deadline,identity,completion/readiness and no-replay cases;no hardware.");
 return 0;
}
