#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/serial_worker.h"
#include "../../kernel/ipc.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"serial worker line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static Process kernel;
static Thread main_thread,other_thread;
static Thread *current=&main_thread;
static SerialWorker w;
static unsigned step,fault,objects,caps,threads,sends,revoke_count,revoke_fault,reply_fault;
static u64 next_handle=10,ticks;
static IpcMessage command;
Process *process_kernel(void){return &kernel;}
Thread *thread_current(void){return current;}
CapabilityTable *process_capabilities(Process *p){CHECK(p==&kernel);return &kernel.capabilities;}
void interrupts_disable(void){}
void interrupts_enable(void){}
bool timer_initialized(void){return true;}
u32 timer_frequency(void){return 100;}
u64 timer_ticks(void){return ++ticks;}
bool scheduler_preemption_enabled(void){return true;}
bool thread_wait_active(const Thread *t){return t->wait_kind!=THREAD_WAIT_NONE;}
static bool pass(void){return ++step!=fault;}
bool endpoint_create(Endpoint *e){if(!pass())return false;e->initialized=true;e->id=++objects;return true;}
bool endpoint_closed(const Endpoint *e){return e->closed;}
bool ipc_endpoint_close(Endpoint *e){e->closed=true;return true;}
bool endpoint_destroy(Endpoint *e){CHECK(e->initialized&&e->closed);e->initialized=false;--objects;return true;}
bool capability_insert(CapabilityTable *t,void *o,CapabilityType type,CapabilityRights rights,CapabilityHandle *out){
 (void)t;(void)o;(void)type;(void)rights;if(!pass())return false;*out=++next_handle;++caps;return true;
}
bool capability_revoke(CapabilityTable *t,CapabilityHandle h){(void)t;CHECK(h);if(++revoke_count==revoke_fault)return false;--caps;return true;}
bool thread_create(Thread *t,Process *p){CHECK(p==&kernel);if(!pass())return false;t->id=200+threads;t->process=p;t->state=THREAD_STATE_READY;++threads;return true;}
bool thread_prepare_kernel(Thread *t,ThreadEntry entry,void *argument){if(!pass())return false;t->entry=entry;t->argument=argument;return true;}
bool scheduler_add(Thread *t){if(!pass())return false;t->on_run_queue=true;return true;}
bool thread_destroy(Thread *t){CHECK(!t->on_run_queue&&(t->state==THREAD_STATE_DEAD||!w.published));--threads;t->id=0;return true;}
bool scheduler_yield(void){return true;}
void scheduler_exit_current(void){abort();}
bool ipc_send_blocking_for(Process *p,CapabilityHandle h,const IpcMessage *q,u64 timeout){
 CHECK(p==&kernel&&h==w.request_send&&timeout==200);++sends;command=*q;return reply_fault!=1;
}
bool ipc_receive_blocking_for(Process *p,CapabilityHandle h,IpcMessage *r,u64 timeout){
 CHECK(p==&kernel&&h==w.reply_receive&&timeout==3000);
 if(reply_fault==2)return false;
 memset(r,0,sizeof(*r));r->word_count=4;r->words[0]=0x53574F524B000002ULL;
 r->words[1]=command.words[1];r->words[2]=command.words[2]==9?3:command.words[2]==8?0:1;r->words[3]=w.thread.id;
 if(reply_fault==3)r->word_count=3;
 if(reply_fault==4)++r->words[0];
 if(reply_fault==5)++r->words[1];
 if(reply_fault==6)r->words[2]=4;
 if(reply_fault==7)++r->words[3];
 if(command.words[2]==9){w.thread.state=THREAD_STATE_DEAD;w.thread.on_run_queue=false;w.active=false;}
 return true;
}
static u32 callback(void *c,u32 op,u64 arg){(void)c;(void)op;(void)arg;return 1;}
static void reset(void){
 CHECK(!objects&&!caps&&!threads);memset(&w,0,sizeof(w));step=fault=sends=revoke_count=revoke_fault=reply_fault=0;
 current=&main_thread;main_thread.id=1;main_thread.process=&kernel;main_thread.state=THREAD_STATE_RUNNING;main_thread.on_run_queue=true;
 other_thread=main_thread;other_thread.id=2;
}
static void dead(void){w.thread.state=THREAD_STATE_DEAD;w.thread.on_run_queue=false;w.active=false;}
int main(void){
 for(unsigned fail=1;fail<=9;++fail){
  reset();fault=fail;CHECK(!serial_worker_start(&w,callback,0,10));CHECK(!objects&&!caps&&!threads&&!serial_worker_present(&w));
 }
 reset();CHECK(serial_worker_start(&w,callback,0,10));CHECK(!serial_worker_reap(&w)&&threads==1&&objects==2&&caps==4);
 bool ok;current=&other_thread;CHECK(!serial_worker_call(&w,1,0,&ok)&&!sends);current=&main_thread;
 CHECK(!serial_worker_call(&w,0,0,&ok)&&!sends);
 CHECK(serial_worker_call(&w,8,0,&ok)&&!ok&&!w.poisoned);
 CHECK(serial_worker_call(&w,1,0,&ok)&&ok&&w.sequence==2);
 CHECK(serial_worker_call(&w,9,0,&ok)&&ok&&!serial_worker_present(&w)&&!objects&&!caps&&!threads);
 for(unsigned bad=1;bad<=7;++bad){
  reset();CHECK(serial_worker_start(&w,callback,0,10));reply_fault=bad;
  CHECK(!serial_worker_call(&w,1,0,&ok)&&w.poisoned);
  unsigned before=sends;CHECK(!serial_worker_call(&w,1,0,&ok)&&sends==before);
  CHECK(!serial_worker_reap(&w)&&threads&&objects&&caps);
  dead();CHECK(serial_worker_reap(&w)&&!threads&&!objects&&!caps);
 }
 reset();CHECK(serial_worker_start(&w,callback,0,10));w.sequence=~0ULL;
 CHECK(!serial_worker_call(&w,1,0,&ok)&&w.poisoned&&!sends);dead();CHECK(serial_worker_reap(&w));
 reset();CHECK(serial_worker_start(&w,callback,0,10));dead();revoke_fault=2;
 CHECK(!serial_worker_reap(&w)&&!threads&&objects==2&&caps==3);
 revoke_fault=0;CHECK(serial_worker_reap(&w)&&!serial_worker_present(&w)&&!objects&&!caps);
 puts("PASS: serial worker9 constructor failures,7 malformed/ambiguous RPC cases,owner/invalid request refusal,no replay,live-stack retention,callback-stop barrier and retryable partial teardown;no threads/hardware executed.");
 return 0;
}
