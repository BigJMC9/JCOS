#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_watch.h"
#include "../../kernel/polaris_native.h"
#include "../../kernel/serial_worker.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"watch line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static SerialWorker *live;
static SerialWorkerDispatch callback;
static void *context;
static unsigned phase,begins,polls,immediate,background,bg_policy;
static u64 ticks;
static bool shell,spin_seen;
void terminal_writeln(const char *s){(void)s;}
void terminal_write(const char *s){(void)s;}
void terminal_write_u64(u64 n){(void)n;}
void terminal_putchar(char c){(void)c;}
u32 timer_frequency(void){return 100;}
u64 timer_ticks(void){return ticks;}
bool userspace_shell_active(void){return shell;}
bool serial_worker_present(const SerialWorker *w){return w->active;}
bool serial_worker_dead(const SerialWorker *w){(void)w;return false;}
bool serial_worker_reap(SerialWorker *w){CHECK(!w->active);return true;}
bool serial_worker_start(SerialWorker *w,SerialWorkerDispatch f,void *c,u32 interval){
 CHECK(!w->active&&interval==100);live=w;callback=f;context=c;w->active=true;w->owner_id=1;w->thread.id=2;return true;
}
bool serial_worker_call(SerialWorker *w,u32 op,u64 arg,bool *ok){
 CHECK(w==live&&w->active);u32 r=callback(context,op,arg);*ok=(r&1)!=0;if(r&2)memset(w,0,sizeof(*w));return true;
}
void polaris_native_begin(const BootInfo *b){(void)b;++begins;phase=1;}
void polaris_native_rescue(void){phase=2;}
void polaris_native_finish(void){phase=3;}
void polaris_native_status(void){}
bool polaris_native_power_prepare(void){phase=3;return true;}
bool polaris_native_terminal(void){return !phase||phase==3;}
void polaris_native_poll(void){++polls;}
void polaris_native_abort(void){phase=3;}
bool polaris_native_can_diagnostic(u32 role){return (role==3?phase==1||phase==2:phase==role);}
void polaris_native_diagnostic(u32 role,bool spin){CHECK(role==1||role==2);++immediate;spin_seen=spin;}
void polaris_native_policy_diagnostic(bool spin){++immediate;spin_seen=spin;}
void polaris_native_background_diagnostic(u32 role,bool spin){CHECK(role==1||role==2);++background;spin_seen=spin;}
void polaris_native_background_policy_diagnostic(bool spin){++bg_policy;spin_seen=spin;}
int main(void){
 BootInfo boot;memset(&boot,0,sizeof(boot));
 polaris_watch_begin(&boot);CHECK(phase==1&&begins==1&&live->active);
 polaris_watch_begin(&boot);CHECK(begins==1);
 polaris_watch_arm(1,true);ticks=299;CHECK(callback(context,0,0)==1&&!background&&!immediate);
 ticks=300;CHECK(callback(context,0,0)==1&&!background); /* no Ring3 shell: cancel */
 shell=true;ticks=700;CHECK(callback(context,0,0)==1&&!background);
 polaris_watch_arm(1,true);polaris_watch_arm(3,false); /* cannot replace armed request */
 ticks=1000;CHECK(callback(context,0,0)==1&&background==1&&spin_seen&&!immediate&&!bg_policy);
 ticks=1400;CHECK(callback(context,0,0)==1&&background==1);
 polaris_watch_arm(3,true);ticks=1700;CHECK(callback(context,0,0)==1&&bg_policy==1&&spin_seen&&!immediate);
 polaris_watch_diagnostic(1,false);CHECK(immediate==1&&!spin_seen);
 polaris_watch_policy_diagnostic(true);CHECK(immediate==2&&spin_seen);
 polaris_watch_arm(1,true);polaris_watch_finish();CHECK(!live->active&&phase==3);
 phase=0;polaris_watch_begin(&boot);ticks+=400;CHECK(callback(context,0,0)==1&&background==1);
 polaris_watch_arm(2,false);ticks+=400;CHECK(callback(context,0,0)==1&&background==1); /* wrong active role */
 polaris_watch_rescue();CHECK(phase==2);polaris_watch_status();
 CHECK(polaris_watch_power_prepare()&&!live->active&&phase==3&&polls);
 puts("PASS: production watch adapter serialized ownership,delayed minimum/shell/role guards,cancel/no replay,no background console-probe path,active-begin refusal and stop/power cleanup;core/IPC doubles,no hardware.");
 return 0;
}
