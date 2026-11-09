/* Actual Polaris RPC/progress helpers; service/IPC/console doubles only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_driver.h"
#include "../../kernel/system_console.h"
#include "../../kernel/ipc.h"
#include "../../kernel/timer.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"driver liveness case%d line%d %s\n",bad,__LINE__,#x);exit(1);}}while(0)
static int bad;
static Process supervisor;
static PolarisDriver d;
static unsigned sends,calls,received,console_calls;
static u64 work=10,console_pid=31,console_inc=41,challenge;
static bool busy=true;
static bool adopting;
static unsigned readonly_calls;
Process *process_kernel(void){return &supervisor;}
bool timer_initialized(void){return bad!=20;}
u32 timer_frequency(void){return bad==21 ? 0 : 100;}
bool managed_service_start_ex(ManagedService *s,const ManagedServiceSpec *p,const ManagedServiceLaunchExtras *e){(void)s;(void)p;(void)e;CHECK(0);return false;}
bool managed_service_connect_kernel(ManagedService *s,ManagedServiceConnection *c){(void)s;(void)c;CHECK(0);return false;}
ManagedServiceState managed_service_state(ManagedService *s){return s->state;}
ManagedServiceStopResult managed_service_stop_bounded(ManagedService *s){(void)s;CHECK(0);return MANAGED_SERVICE_STOP_FAILED;}
bool program_instance_needs_cleanup(const ProgramInstance *p){(void)p;return false;}
u64 managed_service_process_id(ManagedService *s){return bad==22 ? 0 : s->program.process.id;}
bool managed_service_idle(ManagedService *s){(void)s;return !busy;}
bool program_borrowed_set_writable(ProgramInstance *p,u64 pid,bool rw){CHECK(p==&d.service.program&&pid==d.process_id&&!rw);++readonly_calls;return bad!=37;}
bool managed_service_call(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q,IpcMessage *r){
 CHECK(s==&d.service&&c==&d.connection&&q->word_count==4);
 if(adopting){
  CHECK(q->words[0]==POLARIS_NATIVE_HEADER(POLARIS_NATIVE_RETAINED)&&q->words[1]==123&&!q->words[2]&&!q->words[3]);++calls;
  memset(r,0,sizeof(*r));r->word_count=4;r->words[0]=POLARIS_NATIVE_REPLY;r->words[1]=c->incarnation;r->words[2]=POLARIS_NATIVE_RETAINED;r->words[3]=123;
  if(bad==30)r->word_count=3;if(bad==31)r->words[0]^=1;if(bad==32)r->words[1]^=1;
  if(bad==33)r->words[2]^=1;if(bad==34)r->words[3]^=1;return bad!=35;
 }
 CHECK(q->words[0]==POLARIS_NATIVE_HEADER(POLARIS_NATIVE_PING));
 CHECK(q->words[1]==challenge&&!q->words[2]&&!q->words[3]);++calls;
 memset(r,0,sizeof(*r));r->word_count=4;r->words[0]=POLARIS_NATIVE_REPLY;r->words[1]=c->incarnation;
 r->words[2]=d.role;r->words[3]=q->words[1];
 if(bad==1)r->word_count=3;if(bad==2)r->words[0]^=1;if(bad==3)r->words[1]^=1;
 if(bad==4)r->words[2]^=1;if(bad==5)r->words[3]^=1;return bad!=6;
}
bool managed_service_send_oneway(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q){
 CHECK(s==&d.service&&c==&d.connection&&q->word_count==4);
 CHECK(q->words[0]==POLARIS_NATIVE_HEADER(POLARIS_NATIVE_TEST_SPIN)||q->words[0]==POLARIS_NATIVE_HEADER(POLARIS_NATIVE_TEST_FAULT));
 CHECK(q->words[1]==d.role&&!q->words[2]&&!q->words[3]);challenge=(u32)q->words[0];++sends;return bad!=16;
}
bool ipc_receive_blocking_for(Process *p,CapabilityHandle cap,IpcMessage *r,u64 timeout){
 CHECK(p==&supervisor&&cap==d.connection.receive_handle&&timeout==200);++received;
 memset(r,0,sizeof(*r));r->word_count=4;r->words[0]=POLARIS_NATIVE_REPLY;r->words[1]=d.connection.incarnation;
 r->words[2]=challenge;r->words[3]=d.role;
 if(bad==10)r->word_count=3;if(bad==11)r->words[0]^=1;if(bad==12)r->words[1]^=1;
 if(bad==13)r->words[2]^=1;if(bad==14)r->words[3]^=1;return bad!=15;
}
bool system_console_running(void){return bad!=23;}
u64 system_console_process_id(void){return console_pid;}
u64 system_console_incarnation(void){return console_inc;}
u64 system_console_portal_write_count(void){return work;}
bool system_console_putchar(char c){
 CHECK(c=='.');++console_calls;if(bad!=25)++work;if(bad==26)++console_pid;if(bad==27)++console_inc;return bad!=24;
}
int main(void){
 memset(&d,0,sizeof(d));d.process_id=d.service.program.process.id=21;d.service.state=MANAGED_SERVICE_RUNNING;
 d.connection.incarnation=51;d.connection.receive_handle=61;
 for(d.role=1;d.role<=2;++d.role){
   for(unsigned n=0;n<3;++n){bad=0;challenge=n==2 ? ~0ULL : n;CHECK(polaris_driver_ping(&d,challenge));}
   for(bad=1;bad<=6;++bad){challenge=0x123456789ABCDEF0ULL;unsigned before=calls;CHECK(!polaris_driver_ping(&d,challenge)&&calls==before+1);}
   for(unsigned spin=0;spin<2;++spin){
     bad=0;CHECK(polaris_driver_diagnostic(&d,spin!=0));
     for(bad=10;bad<=16;++bad){unsigned before=sends,reads=received;CHECK(!polaris_driver_diagnostic(&d,spin!=0)&&sends==before+1&&received==reads+(bad==16 ? 0U : 1U));}
   }
 }
 for(bad=20;bad<=22;++bad){unsigned before=sends;CHECK(!polaris_driver_diagnostic(&d,true)&&sends==before);}
 d.role=1;PolarisDriverProgress p;bad=0;
 CHECK(polaris_driver_console_progress(&d,true,&p)&&p.writes_after==p.writes_before+1);
 busy=false;unsigned before=console_calls;CHECK(!polaris_driver_console_progress(&d,true,&p)&&console_calls==before);busy=true;
 for(bad=22;bad<=27;++bad){before=console_calls;CHECK(!polaris_driver_console_progress(&d,true,&p));CHECK(console_calls==before+(bad>=24 ? 1U : 0U));}
 bad=0;CHECK(!polaris_driver_ping(0,1)&&!polaris_driver_diagnostic(0,true)&&!polaris_driver_console_progress(0,true,&p));
 adopting=true;busy=false;d.role=1;d.process_id=d.service.program.process.id;bad=0;
 d.program_attempted=d.retained_only=false;CHECK(polaris_driver_attach_retained(&d,123)&&d.retained_only&&d.program_attempted);
 unsigned count=calls;CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count);
 for(bad=30;bad<=35;++bad){d.program_attempted=d.retained_only=false;count=calls;CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count+1&&d.retained_only&&d.program_attempted);CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count+1);}
 bad=37;d.program_attempted=d.retained_only=false;count=calls;CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count&&d.retained_only&&d.program_attempted);
 bad=0;d.program_attempted=d.retained_only=false;count=calls;d.role=2;CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count&&!d.program_attempted);d.role=1;busy=true;CHECK(!polaris_driver_attach_retained(&d,123)&&calls==count&&!d.program_attempted);busy=false;
 CHECK(!polaris_driver_attach_retained(&d,0)&&calls==count&&!d.program_attempted);
 puts("PASS: actual retained-entry RPC,six malformed/failed acknowledgements,whole-range RO guard failure,role/busy/zero/duplicate guards;attempt consumed before transport,no replay.");
 puts("PASS: actual heartbeat challenge/role/incarnation replies,12 forged/failed heartbeat cases,28 malformed/failed diagnostic ACK cases,no replay,missing timer/PID,exact independent console work and peer/identity/progress refusals; no hardware.");
 return 0;
}
