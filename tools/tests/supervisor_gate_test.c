#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/supervisor.c"
static Process test_kernel;
static Thread test_main;
static u64 test_ticks;
static unsigned yields;
static int release_owner;
Process *process_kernel(void){return &test_kernel;}
Thread *thread_current(void){return &test_main;}
void interrupts_disable(void){}
void interrupts_enable(void){}
bool timer_initialized(void){return true;}
u32 timer_frequency(void){return 100;}
u64 timer_ticks(void){return ++test_ticks;}
bool scheduler_yield(void){++yields;if(release_owner)g_rpc_owner=0;return true;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"supervisor gate line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 test_main.id=1;test_main.process=&test_kernel;
 CHECK(supervisor_call_enter()&&g_rpc_owner==1);
 CHECK(!supervisor_call_enter()&&g_rpc_owner==1&&!yields);
 supervisor_call_leave();CHECK(!g_rpc_owner);
 g_rpc_owner=2;release_owner=1;CHECK(supervisor_call_enter()&&g_rpc_owner==1&&yields==1);
 supervisor_call_leave();g_rpc_owner=2;release_owner=0;test_ticks=0;
 CHECK(!supervisor_call_enter()&&g_rpc_owner==2);
 supervisor_call_leave();CHECK(g_rpc_owner==2);
 puts("PASS: actual supervisor admission gate,recursive refusal,serialized contenders,bounded admission timeout and retained foreign owner;no IPC replay or hardware.");
 return 0;
}
