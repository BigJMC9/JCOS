#include "graphics_worker_test.h"
#include "serial_worker.h"
#include "graphics_supervisor.h"
#include "polaris_driver.h"
#include "supervisor.h"
#include "pmm.h"
#include "physmap.h"
#include "process.h"
#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "scheduler.h"
#include "interrupts.h"
#include "system_console.h"
#include "terminal.h"
#include "timer.h"
#include "lib.h"
#include "power.h"
static SerialWorker work;
static PolarisDriver primary,rescue;
static GraphicsSupervisorSession policy;
static u64 storage,challenge;
static volatile u32 stage;
static volatile bool unexpected;
enum {ARM=1,PRIMARY_SPIN,RESCUE_FAULT,POLICY_FAULT,POLICY_SPIN,CLOSE};
static bool clean_drivers(void) {
    bool a=polaris_driver_stop(&primary),b=polaris_driver_stop(&rescue);
    if(!a || !b)return false;
    if(storage){if(!frame_free_range(phys_to_frame(storage),768))return false;storage=0;}
    return true;
}
static bool decision(u32 event,u64 facts,u64 expected) {
    u64 action;
    return graphics_supervisor_request(&policy,event,facts,&action) && action==expected;
}
static u32 dispatch(void *unused,u32 operation,u64 argument) {
    (void)unused;(void)argument;
    if(operation==CLOSE || operation==SERIAL_WORKER_ABORT) {
        if(policy.session && !policy.failed)(void)decision(GRAPHICS_POLICY_CLOSE,0,GRAPHICS_POLICY_HEADLESS);
        bool ok=clean_drivers();stage=0;
        return SERIAL_WORKER_STOP|(ok?SERIAL_WORKER_OK:0);
    }
    if(operation==ARM) {
        storage=pmm_alloc_pages(768);if(!storage)return 0;
        ProgramBorrowedMappingSpec m;m.physical_base=storage;m.size=0x300000;
        m.virtual_base=POLARIS_NATIVE_FB_VA;m.flags=0;
        if(!polaris_driver_reserve(&primary,1,&m) || !polaris_driver_reserve(&rescue,2,&m) ||
            !graphics_supervisor_begin(&policy) || !decision(GRAPHICS_POLICY_PRIMARY,0,GRAPHICS_POLICY_KEEP))return 0;
        stage=1;return SERIAL_WORKER_OK;
    }
    if(operation==PRIMARY_SPIN)return polaris_driver_diagnostic(&primary,true)?SERIAL_WORKER_OK:0;
    if(operation==RESCUE_FAULT)return polaris_driver_diagnostic(&rescue,false)?SERIAL_WORKER_OK:0;
    if(operation==POLICY_FAULT)return supervisor_test_fault()?SERIAL_WORKER_OK:0;
    if(operation==POLICY_SPIN)return supervisor_test_spin()?SERIAL_WORKER_OK:0;
    if(operation!=SERIAL_WORKER_TICK)return 0;
    if(stage!=1 && stage!=2)return SERIAL_WORKER_OK;
    bool pl=stage==1 && managed_service_running(&primary.service);
    bool rl=managed_service_running(&rescue.service);
    u64 facts=(pl?GRAPHICS_POLICY_PRIMARY_LIVE:0)|(rl?GRAPHICS_POLICY_RESCUE_LIVE:0);
    bool pp=pl && polaris_driver_ping(&primary,++challenge);
    bool rp=rl && polaris_driver_ping(&rescue,++challenge);
    facts|=GRAPHICS_POLICY_PROBED|(pp?GRAPHICS_POLICY_PRIMARY_PONG:0)|(rp?GRAPHICS_POLICY_RESCUE_PONG:0);
    u64 action;
    if(!graphics_supervisor_request(&policy,GRAPHICS_POLICY_OBSERVE,facts,&action)) {
        if(!clean_drivers() || !supervisor_stop_identity(policy.process_id,policy.thread_id))unexpected=true;
        stage=3;return SERIAL_WORKER_OK;
    }
    if(action==GRAPHICS_POLICY_RECOVER && stage==1) {
        if(!polaris_driver_stop(&primary) || !decision(GRAPHICS_POLICY_RESCUED,0,GRAPHICS_POLICY_KEEP))unexpected=true;
        stage=2;
    } else if(action==GRAPHICS_POLICY_HEADLESS) {
        if(!clean_drivers())unexpected=true;
        stage=3;
    } else if(action!=GRAPHICS_POLICY_KEEP)unexpected=true;
    return SERIAL_WORKER_OK;
}
static bool call(u32 operation) {
    bool ok;
    return serial_worker_call(&work,operation,0,&ok) && ok;
}
static bool wait_stage(u32 expected,bool ping) {
    u64 start=timer_ticks(),work_tick=start,cookie;
    while(stage!=expected) {
        if(unexpected || timer_ticks()-start>=(u64)timer_frequency()*8)return false;
        if(timer_ticks()-work_tick>=timer_frequency()/10U) {
            work_tick=timer_ticks();
            if(!system_console_putchar('.'))return false;
            if(ping && (!supervisor_ping(0x574F524B45525049ULL,&cookie)||cookie!=0x574F524B45525049ULL))return false;
        }
        u64 flags;__asm__ volatile("pushfq; popq %0":"=r"(flags)::"memory");
        interrupts_disable();bool yielded=scheduler_yield();
        if(flags&(1ULL<<9))interrupts_enable();
        if(!yielded)return false;
    }
    return !unexpected;
}
static bool restore(void) {
    if(supervisor_running() && supervisor_idle())return true;
    if(supervisor_state()!=SUPERVISOR_STATE_STOPPED && !supervisor_stop())return false;
    u64 start=timer_ticks();
    while(scheduler_thread_count()!=1ULL) {
        u64 flags;__asm__ volatile("pushfq; popq %0":"=r"(flags)::"memory");
        interrupts_disable();bool ok=scheduler_yield();if(flags&(1ULL<<9))interrupts_enable();
        if(!ok || timer_ticks()-start>=(u64)timer_frequency()*2)return false;
    }
    return supervisor_start();
}
static bool cleanup(void) {
    if(serial_worker_present(&work)) {
        if(serial_worker_dead(&work)){if(!serial_worker_reap(&work))return false;}
        else if(!call(CLOSE))return false;
    }
    return clean_drivers() && restore();
}
static bool check(const char *name,bool ok) {
    terminal_write("  ");terminal_write(name);terminal_writeln(ok?": PASS":": FAILED");return ok;
}
void graphics_worker_test_cleanup_run(void) {check("WORKER / POLICY / RAM CLEANUP",cleanup());}
void graphics_worker_test_run(void) {
    terminal_writeln("AUTONOMOUS GRAPHICS WORKER TEST (RAM ONLY / NO GPU IO):");
    if(!cleanup())goto fail;
    u64 pages=pmm_stats().free_pages,kernel_threads=process_thread_count(process_kernel());
    u32 processes=process_object_count(),spaces=address_space_object_count(),threads=thread_object_count();
    u32 endpoints=endpoint_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
    for(u32 scenario=0;scenario<3;++scenario) {
        k_memset(&primary,0,sizeof(primary));k_memset(&rescue,0,sizeof(rescue));
        k_memset(&policy,0,sizeof(policy));stage=0;unexpected=false;challenge=0;
        u64 old_pid=supervisor_process_id(),old_tid=supervisor_thread_id();
        if(!check("PRIVATE PERIODIC WORKER / INDEPENDENT CONTROL THREAD",serial_worker_start(&work,dispatch,0,timer_frequency()/10U)&&
            work.thread.id!=thread_current()->id && call(ARM)))goto fail;
        if(scenario==0) {
            u64 before=system_console_portal_write_count(),cookie=0;
            bool parallel=true;
            for(u32 n=0;n<8;++n)parallel=parallel && system_console_putchar('.') &&
                supervisor_ping(0x434F4E54454E4445ULL,&cookie) && cookie==0x434F4E54454E4445ULL;
            if(!check("UNRELATED RING3 CONSOLE WORK / SERIALIZED SUPERVISOR CLIENTS",
                parallel && system_console_portal_write_count()>=before+8 && supervisor_process_id()==old_pid))goto fail;
            if(!check("PRIMARY SPIN -> ACTUAL RING3 RESCUE DECISION WITHOUT MONITOR POLL",call(PRIMARY_SPIN)&&wait_stage(2,true)))goto fail;
            if(!check("RESCUE FAULT -> ACTUAL RING3 HEADLESS DECISION WITHOUT MONITOR POLL",call(RESCUE_FAULT)&&wait_stage(3,true)))goto fail;
        } else {
            GraphicsSupervisorSession stale=policy;
            if(!check(scenario==1?"POLICY PROCESS FAULT / AUTONOMOUS FAIL-CLOSED":"POLICY PROCESS SPIN / AUTONOMOUS FAIL-CLOSED",
                call(scenario==1?POLICY_FAULT:POLICY_SPIN)&&wait_stage(3,false)))goto fail;
            if(!check("POLICY-LOSS CPU OWNERS DRAINED",managed_service_state(&primary.service)==MANAGED_SERVICE_STOPPED &&
                managed_service_state(&rescue.service)==MANAGED_SERVICE_STOPPED && !storage))goto fail;
            if(!check("FAILED POLICY STOPPED BY EXACT ID",supervisor_state()==SUPERVISOR_STATE_STOPPED) || !call(CLOSE))goto fail;
            if(!check("POWER READY WITH FAILED POLICY REMOVED",power_check_reboot()==POWER_RESULT_OK))goto fail;
            if(!restore())goto fail;
            u64 action;
            if(!check("REPLACEMENT POLICY IDENTITY / OLD SESSION REFUSED",
                supervisor_process_id()!=old_pid && supervisor_thread_id()!=old_tid &&
                supervisor_stop_identity(old_pid,old_tid) && supervisor_running() &&
                !graphics_supervisor_request(&stale,GRAPHICS_POLICY_CLOSE,0,&action)&&stale.failed))goto fail;
        }
        if(serial_worker_present(&work) && !call(CLOSE))goto fail;
        if(!check("WORKER STOPPED/REAPED ONLY AFTER CALLBACK RETURN / FULL BASELINE",
            !serial_worker_present(&work) && pmm_stats().free_pages==pages &&
            process_object_count()==processes && address_space_object_count()==spaces && thread_object_count()==threads &&
            endpoint_object_count()==endpoints && process_thread_count(process_kernel())==kernel_threads &&
            capability_table_count(process_capabilities(process_kernel()))==caps))goto fail;
    }
    terminal_writeln("AUTONOMOUS GRAPHICS WORKER TEST: PASS");return;
fail:
    terminal_writeln("AUTONOMOUS GRAPHICS WORKER TEST: FAILED");
}
