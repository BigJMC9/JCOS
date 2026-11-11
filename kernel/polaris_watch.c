#include "polaris_watch.h"
#include "terminal.h"
#ifndef JCOS_POLARIS_NATIVE_WRITE
#define JCOS_POLARIS_NATIVE_WRITE 0
#endif
#if JCOS_POLARIS_NATIVE_WRITE
#include "polaris_native.h"
#include "serial_worker.h"
#include "timer.h"
#include "userspace_shell.h"
#include "graphical_session.h"
static SerialWorker worker;
enum {BEGIN=1,RESCUE,DIAGNOSTIC,POLICY,STATUS,FINISH,POWER,ARM,PRESENT,COLLECT,RECONNECT,LAUNCH,RESTART,VTOTAL,VTOTAL_FAULT1,VTOTAL_FAULT2,VTOTAL_FAULT3};
static u32 pending_role;
static bool pending_spin;
static u64 pending_since;
static u32 dispatch(void *unused,u32 op,u64 arg) {
    (void)unused;
    switch(op) {
    case SERIAL_WORKER_TICK:
        polaris_native_poll();
        if(pending_role && timer_ticks()-pending_since>=(u64)timer_frequency()*3) {
            u32 role=pending_role;pending_role=0;
            if(!userspace_shell_active() || !polaris_native_can_diagnostic(role)) {
                terminal_writeln("DELAYED DIAGNOSTIC REFUSED: ACTIVE RING3 SHELL/ROLE REQUIRED.");
            } else {
                terminal_write("OFF-MONITOR DIAGNOSTIC: WORKER THREAD=");terminal_write_u64(worker.thread.id);
                terminal_write(" CONTROL THREAD=");terminal_write_u64(worker.owner_id);
                terminal_writeln(" RING3 SHELL ACTIVE.");
                terminal_writeln(graphical_session_active() ? "OFF-MONITOR GUI ACTIVE=YES." : "OFF-MONITOR GUI ACTIVE=NO.");
                if(role==3)polaris_native_background_policy_diagnostic(pending_spin);
                else polaris_native_background_diagnostic(role,pending_spin);
            }
        }
        return SERIAL_WORKER_OK;
    case SERIAL_WORKER_ABORT:pending_role=0;polaris_native_abort();return SERIAL_WORKER_STOP;
    case VTOTAL:case VTOTAL_FAULT1:case VTOTAL_FAULT2:case VTOTAL_FAULT3:
        polaris_native_begin_vtotal((const BootInfo*)arg,op-VTOTAL);break;
    case BEGIN:polaris_native_begin((const BootInfo*)arg);break;
    case RESCUE:polaris_native_rescue();break;
    case DIAGNOSTIC:polaris_native_diagnostic((u32)(arg>>1),(arg&1)!=0);break;
    case POLICY:polaris_native_policy_diagnostic(arg!=0);break;
    case ARM:
        if(pending_role || !polaris_native_can_diagnostic((u32)(arg>>1))) {
            terminal_writeln("REFUSED: DELAYED DIAGNOSTIC ALREADY ARMED OR ROLE INACTIVE.");return 0;
        }
        pending_role=(u32)(arg>>1);pending_spin=(arg&1)!=0;pending_since=timer_ticks();
        terminal_writeln("DELAYED DIAGNOSTIC ARMED: ENTER usershell; THREE-SECOND MINIMUM; NO CONSOLE RPC FROM WORKER.");
        break;
    case STATUS:
        polaris_native_status();
        terminal_write("GRAPHICS WORKER THREAD=");terminal_write_u64(worker.thread.id);
        terminal_write(" PERIODIC CALLS=");terminal_write_u64(worker.periodic_calls);terminal_putchar('\n');
        break;
    case PRESENT:polaris_native_present();break;
    case RESTART:
    case LAUNCH:
    case RECONNECT: {
        if(op==RESTART && pending_role)return 0;
        bool connected=op==RESTART ? polaris_native_restart() : op==LAUNCH ? polaris_native_launch((const BootInfo*)arg) : polaris_native_reconnect(arg);
        bool terminal=polaris_native_terminal();
        if(terminal)pending_role=0;
        return (connected?SERIAL_WORKER_OK:0)|(terminal?SERIAL_WORKER_STOP:0);
    }
    /* A readiness query may collect completed CPU worker resources. It never
     * polls the active backend or changes an active/pending graphics stage. */
    case COLLECT:
        if(polaris_native_terminal()){pending_role=0;return SERIAL_WORKER_OK|SERIAL_WORKER_STOP;}
        return SERIAL_WORKER_OK;
    case FINISH:polaris_native_finish();break;
    case POWER: {
        pending_role=0;
        bool ready=polaris_native_power_prepare();
        /* A refused active-GUI transition must keep backend supervision. */
        return (polaris_native_terminal()?SERIAL_WORKER_STOP:0)|(ready?SERIAL_WORKER_OK:0);
    }
    default:return 0;
    }
    bool terminal=polaris_native_terminal();
    if(terminal)pending_role=0;
    return SERIAL_WORKER_OK|(terminal?SERIAL_WORKER_STOP:0);
}
static bool call(u32 op,u64 arg) {
    bool ok=false;
    if(!serial_worker_call(&worker,op,arg,&ok) || !ok) {
        terminal_writeln("GRAPHICS WORKER REQUEST INCOMPLETE; NO REPLAY / RETAINED UNTIL SAFE DRAIN.");
        return false;
    }
    return true;
}
static bool inactive(void) {
    if(serial_worker_dead(&worker))return serial_worker_reap(&worker);
    return !serial_worker_present(&worker);
}
void polaris_watch_begin(const BootInfo *boot) {
    if(!inactive()){terminal_writeln("REFUSED: GRAPHICS WORKER ALREADY OWNS NATIVE STATE.");return;}
    if(!serial_worker_start(&worker,dispatch,0,timer_frequency())) {
        terminal_writeln("REFUSED: GRAPHICS WORKER RESERVATION INCOMPLETE; NO GPU ACCESS.");return;
    }
    (void)call(BEGIN,(u64)boot);
}
void polaris_watch_begin_vtotal(const BootInfo *boot,u32 prefix){
    if(!boot || prefix>3)return;
    if(!inactive()){terminal_writeln("REFUSED: GRAPHICS WORKER ALREADY OWNS NATIVE STATE.");return;}
    if(!serial_worker_start(&worker,dispatch,0,timer_frequency())){terminal_writeln("REFUSED: GRAPHICS WORKER RESERVATION INCOMPLETE; NO GPU ACCESS.");return;}
    (void)call(VTOTAL+prefix,(u64)boot);
}
bool polaris_watch_launch(const BootInfo *boot) {
    if (!boot || !inactive())return false;
    if (!serial_worker_start(&worker,dispatch,0,timer_frequency()))return false;
    bool accepted=false;
    if (!serial_worker_call(&worker,LAUNCH,(u64)boot,&accepted)) {
        terminal_writeln("GUI NATIVE TRANSPORT INCOMPLETE; NO REPLAY.");return false;
    }
    return accepted;
}
void polaris_watch_rescue(void){if(inactive())polaris_native_rescue();else(void)call(RESCUE,0);}
void polaris_watch_diagnostic(u32 role,bool spin){if(inactive())polaris_native_diagnostic(role,spin);else(void)call(DIAGNOSTIC,((u64)role<<1)|(spin?1:0));}
void polaris_watch_policy_diagnostic(bool spin){if(inactive())polaris_native_policy_diagnostic(spin);else(void)call(POLICY,spin?1:0);}
void polaris_watch_arm(u32 role,bool spin){if(inactive())terminal_writeln("REFUSED: ACTIVE GRAPHICS WORKER REQUIRED.");else(void)call(ARM,((u64)role<<1)|(spin?1:0));}
void polaris_watch_status(void){if(inactive())polaris_native_status();else(void)call(STATUS,0);}
bool polaris_watch_restart(void) {
    if(inactive())return false;
    bool accepted=false;
    if(!serial_worker_call(&worker,RESTART,0,&accepted)) {
        terminal_writeln("PRIMARY CPU RESTART TRANSPORT INCOMPLETE; NO REPLAY.");return false;
    }
    return accepted;
}
bool polaris_watch_reconnect(u64 old_output) {
    if(!old_output || inactive())return false;
    bool accepted=false;
    if(!serial_worker_call(&worker,RECONNECT,old_output,&accepted)) {
        terminal_writeln("GUI RECONNECT TRANSPORT INCOMPLETE; NO REPLAY.");return false;
    }
    return accepted;
}
void polaris_watch_present(void){if(inactive())terminal_writeln("REFUSED: ACTIVE GRAPHICS WORKER REQUIRED.");else(void)call(PRESENT,0);}
void polaris_watch_finish(void){if(inactive())polaris_native_finish();else(void)call(FINISH,0);}
void polaris_watch_collect(void) {
    if(!inactive())(void)call(COLLECT,0);
}
bool polaris_watch_power_prepare(void) {
    if(inactive())return polaris_native_power_prepare();
    return call(POWER,0) && !serial_worker_present(&worker);
}
#else
void polaris_watch_begin_vtotal(const BootInfo *b,u32 p){(void)b;(void)p;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
bool polaris_watch_restart(void){return false;}
bool polaris_watch_launch(const BootInfo *boot){(void)boot;return false;}
bool polaris_watch_reconnect(u64 old_output){(void)old_output;return false;}
void polaris_watch_collect(void){}
void polaris_watch_present(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_arm(u32 r,bool s){(void)r;(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_begin(const BootInfo *b){(void)b;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_rescue(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_diagnostic(u32 r,bool s){(void)r;(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_policy_diagnostic(bool s){(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_status(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_finish(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
bool polaris_watch_power_prepare(void){return true;}
#endif
