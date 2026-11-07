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
static SerialWorker worker;
enum {BEGIN=1,RESCUE,DIAGNOSTIC,POLICY,STATUS,FINISH,POWER,ARM};
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
                if(role==3)polaris_native_background_policy_diagnostic(pending_spin);
                else polaris_native_background_diagnostic(role,pending_spin);
            }
        }
        return SERIAL_WORKER_OK;
    case SERIAL_WORKER_ABORT:pending_role=0;polaris_native_abort();return SERIAL_WORKER_STOP;
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
    case FINISH:polaris_native_finish();break;
    case POWER:pending_role=0;return SERIAL_WORKER_STOP|(polaris_native_power_prepare()?SERIAL_WORKER_OK:0);
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
void polaris_watch_rescue(void){if(inactive())polaris_native_rescue();else(void)call(RESCUE,0);}
void polaris_watch_diagnostic(u32 role,bool spin){if(inactive())polaris_native_diagnostic(role,spin);else(void)call(DIAGNOSTIC,((u64)role<<1)|(spin?1:0));}
void polaris_watch_policy_diagnostic(bool spin){if(inactive())polaris_native_policy_diagnostic(spin);else(void)call(POLICY,spin?1:0);}
void polaris_watch_arm(u32 role,bool spin){if(inactive())terminal_writeln("REFUSED: ACTIVE GRAPHICS WORKER REQUIRED.");else(void)call(ARM,((u64)role<<1)|(spin?1:0));}
void polaris_watch_status(void){if(inactive())polaris_native_status();else(void)call(STATUS,0);}
void polaris_watch_finish(void){if(inactive())polaris_native_finish();else(void)call(FINISH,0);}
bool polaris_watch_power_prepare(void) {
    if(inactive())return polaris_native_power_prepare();
    return call(POWER,0) && !serial_worker_present(&worker);
}
#else
void polaris_watch_arm(u32 r,bool s){(void)r;(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_begin(const BootInfo *b){(void)b;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_rescue(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_diagnostic(u32 r,bool s){(void)r;(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_policy_diagnostic(bool s){(void)s;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_status(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_watch_finish(void){terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
bool polaris_watch_power_prepare(void){return true;}
#endif
