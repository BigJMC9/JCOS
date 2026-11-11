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
static unsigned phase,begins,polls,immediate,background,bg_policy,presents,gui_witness,reconnects;
static u64 ticks;
static unsigned launches,starts,restarts,vtotal_begins,last_prefix;
static bool restart_accept;
static u32 rescue_fault_begins;
static bool launch_accept=true,reserve_refused,transport_refused;
static bool shell,spin_seen,power_refused,gui,terminal_pending;
void terminal_writeln(const char *s){if(!strcmp(s,"OFF-MONITOR GUI ACTIVE=YES."))++gui_witness;}
bool graphical_session_active(void){return gui;}
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
 ++starts;if(reserve_refused)return false;
 CHECK(!w->active&&interval==100);live=w;callback=f;context=c;w->active=true;w->owner_id=1;w->thread.id=2;return true;
}
bool serial_worker_call(SerialWorker *w,u32 op,u64 arg,bool *ok){
 CHECK(w==live&&w->active);if(transport_refused){*ok=false;return false;}u32 r=callback(context,op,arg);*ok=(r&1)!=0;if(r&2)memset(w,0,sizeof(*w));return true;
}
void polaris_native_begin(const BootInfo *b){(void)b;++begins;phase=1;}
void polaris_native_begin_vtotal(const BootInfo *b,u32 prefix){CHECK(b&&prefix<=3);++vtotal_begins;last_prefix=prefix;phase=1;}
void polaris_native_begin_rescue_fault(const BootInfo *b,u32 prefix){CHECK(b&&prefix>=1&&prefix<=3);++rescue_fault_begins;last_prefix=prefix;phase=1;}
bool polaris_native_launch(const BootInfo *b){CHECK(b);++launches;phase=launch_accept?1:0;return launch_accept;}
bool polaris_native_restart(void){++restarts;return restart_accept;}
void polaris_native_rescue(void){phase=2;}
void polaris_native_present(void){++presents;}
bool polaris_native_reconnect(u64 old){++reconnects;return phase==2&&old==11;}
void polaris_native_finish(void){phase=3;}
void polaris_native_status(void){}
bool polaris_native_power_prepare(void){if(power_refused)return false;phase=3;return true;}
bool polaris_native_terminal(void){return !phase||(phase==3&&!terminal_pending);}
void polaris_native_poll(void){++polls;}
void polaris_native_abort(void){phase=3;}
bool polaris_native_can_diagnostic(u32 role){return (role==3?phase==1||phase==2:phase==role);}
void polaris_native_diagnostic(u32 role,bool spin){CHECK(role==1||role==2);++immediate;spin_seen=spin;}
void polaris_native_policy_diagnostic(bool spin){++immediate;spin_seen=spin;}
void polaris_native_background_diagnostic(u32 role,bool spin){CHECK(role==1||role==2);++background;spin_seen=spin;}
void polaris_native_background_policy_diagnostic(bool spin){++bg_policy;spin_seen=spin;}
int main(void){
 BootInfo boot;memset(&boot,0,sizeof(boot));
 CHECK(!polaris_watch_restart()&&!restarts);
 polaris_watch_collect();polaris_watch_present();CHECK(!presents);CHECK(!polaris_watch_reconnect(11)&&!reconnects);
 polaris_watch_begin(&boot);CHECK(phase==1&&begins==1&&live->active);
 polaris_watch_begin(&boot);CHECK(begins==1);polaris_watch_present();CHECK(presents==1&&phase==1&&live->active);
 unsigned prior=polls;polaris_watch_collect();CHECK(live->active&&phase==1&&polls==prior);
 power_refused=true;CHECK(!polaris_watch_power_prepare()&&live->active&&phase==1);power_refused=false;
 polaris_watch_arm(1,true);ticks=299;CHECK(callback(context,0,0)==1&&!background&&!immediate);
 ticks=300;CHECK(callback(context,0,0)==1&&!background); /* no Ring3 shell: cancel */
 shell=true;ticks=700;CHECK(callback(context,0,0)==1&&!background);
 polaris_watch_arm(1,true);polaris_watch_arm(3,false); /* cannot replace armed request */
 gui=true;ticks=1000;CHECK(callback(context,0,0)==1&&background==1&&spin_seen&&!immediate&&!bg_policy&&gui_witness==1);gui=false;
 ticks=1400;CHECK(callback(context,0,0)==1&&background==1);
 polaris_watch_arm(3,true);ticks=1700;CHECK(callback(context,0,0)==1&&bg_policy==1&&spin_seen&&!immediate);
 polaris_watch_diagnostic(1,false);CHECK(immediate==1&&!spin_seen);
 polaris_watch_policy_diagnostic(true);CHECK(immediate==2&&spin_seen);
 polaris_watch_arm(1,true);polaris_watch_finish();CHECK(!live->active&&phase==3);
 phase=0;polaris_watch_begin(&boot);ticks+=400;CHECK(callback(context,0,0)==1&&background==1);
 polaris_watch_arm(2,false);ticks+=400;CHECK(callback(context,0,0)==1&&background==1); /* wrong active role */
 polaris_watch_rescue();CHECK(phase==2);polaris_watch_status();
 CHECK(!polaris_watch_reconnect(0)&&!reconnects);CHECK(!polaris_watch_reconnect(12)&&reconnects==1&&live->active);
 CHECK(polaris_watch_reconnect(11)&&reconnects==2&&live->active);
 CHECK(polaris_watch_power_prepare()&&!live->active&&phase==3&&polls);
 phase=0;polaris_watch_begin(&boot);phase=3;terminal_pending=true;
 prior=polls;polaris_watch_collect();CHECK(live->active&&polls==prior);
 terminal_pending=false;polaris_watch_collect();CHECK(!live->active&&polls==prior);
 polaris_watch_collect();CHECK(!live->active&&polls==prior);
 phase=0;unsigned prior_starts=starts;CHECK(!polaris_watch_launch(0)&&starts==prior_starts);
 reserve_refused=true;CHECK(!polaris_watch_launch(&boot)&&!launches&&!live->active);reserve_refused=false;
 launch_accept=false;CHECK(!polaris_watch_launch(&boot)&&launches==1&&!live->active);
 launch_accept=true;CHECK(polaris_watch_launch(&boot)&&launches==2&&live->active);
 prior_starts=starts;CHECK(!polaris_watch_launch(&boot)&&starts==prior_starts&&launches==2);
 CHECK(!polaris_watch_restart()&&restarts==1&&live->active);restart_accept=true;
 CHECK(polaris_watch_restart()&&restarts==2&&live->active);
 polaris_watch_arm(1,true);CHECK(!polaris_watch_restart()&&restarts==2&&live->active);
 CHECK(polaris_watch_power_prepare()&&!live->active);
 transport_refused=true;CHECK(!polaris_watch_launch(&boot)&&launches==2&&live->active);transport_refused=false;
 CHECK(polaris_watch_power_prepare()&&!live->active);
 for(u32 prefix=0;prefix<=3;++prefix){phase=0;polaris_watch_begin_vtotal(&boot,prefix);CHECK(live->active&&vtotal_begins==prefix+1&&last_prefix==prefix);polaris_watch_begin_vtotal(&boot,0);CHECK(vtotal_begins==prefix+1);CHECK(polaris_watch_power_prepare()&&!live->active);}
 prior_starts=starts;polaris_watch_begin_vtotal(0,0);polaris_watch_begin_vtotal(&boot,4);CHECK(starts==prior_starts&&vtotal_begins==4);
 for(u32 prefix=1;prefix<=3;++prefix){
  phase=0;polaris_watch_begin_rescue_fault(&boot,prefix);CHECK(live->active&&rescue_fault_begins==prefix&&last_prefix==prefix);
  prior_starts=starts;polaris_watch_begin_rescue_fault(&boot,1);polaris_watch_begin_vtotal(&boot,0);
  CHECK(starts==prior_starts&&rescue_fault_begins==prefix);CHECK(polaris_watch_power_prepare()&&!live->active);
 }
 prior_starts=starts;polaris_watch_begin_rescue_fault(0,1);polaris_watch_begin_rescue_fault(&boot,0);polaris_watch_begin_rescue_fault(&boot,4);CHECK(starts==prior_starts);
 reserve_refused=true;polaris_watch_begin_rescue_fault(&boot,1);CHECK(!live->active&&rescue_fault_begins==3);reserve_refused=false;
 transport_refused=true;polaris_watch_begin_rescue_fault(&boot,1);CHECK(live->active&&rescue_fault_begins==3);transport_refused=false;
 CHECK(polaris_watch_power_prepare()&&!live->active);
 puts("PASS: rescue-prefix worker all3dispatches/null/zero/invalid/active/reservation/ambiguous-transport guards;one shared worker;CPU-only stop.");
 puts("PASS: vertical-mode worker normal/three fault prefixes,active/null/invalid guards and CPU-only stop;serialized callbacks.");
 puts("PASS: retained restart worker,inactive/refused/accepted/armed guards,no replay or GPU operation;core doubles.");
 puts("PASS: normal native launch adapter,null/active/reservation/refusal/success/ambiguous-transport paths;one serialized callback,no replay,retained transport failure;no hardware.");
 puts("PASS: production watch adapter serialized ownership,delayed minimum/shell/role guards,cancel/no replay,no background console-probe path,active-begin refusal and stop/power cleanup,read-only collection preserves active/pending backend and reaps terminal worker without core polling;core/IPC doubles,no hardware.");
 return 0;
}
