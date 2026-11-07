#include "polaris_native.h"
#include "terminal.h"
#ifndef JCOS_POLARIS_NATIVE_WRITE
#define JCOS_POLARIS_NATIVE_WRITE 0
#endif
#if JCOS_POLARIS_NATIVE_WRITE
#include "polaris_driver.h"
#include "graphics_supervisor.h"
#include "supervisor.h"
#include "polaris_mmio.h"
#include "polaris_observation.h"
#include "polaris_read_profile.h"
#include "polaris_vram.h"
#include "display.h"
#include "framebuffer.h"
#include "device_mmio.h"
#include "timer.h"
#include "scheduler.h"
#include "serial.h"
#include "arch.h"
#include "lib.h"
static struct {
    PciAssignment assignment;
    PolarisMmioBinding binding;
    PolarisMmioHold hold;
    PolarisDriver primary,rescue;
    GraphicsSupervisorSession policy;
    CapabilityHandle cap;
    FramebufferInfo info;
    const BootInfo *boot;
    BootDisplayBar aperture;
    u64 hold_id,output_id,start,last_heartbeat,challenge,heartbeats;
    u32 frequency,stage,diagnostic_mask;
    bool attempted,mutated,original_render,recovery_authorized,policy_closed;
    PolarisDriver *writer;
} g;
static void recover_native(bool induce);
static bool policy_close(void) {
    if (!g.policy.session || g.policy_closed) return true;
    g.policy_closed=true;
    u64 action;
    bool ok=graphics_supervisor_request(&g.policy,GRAPHICS_POLICY_CLOSE,0,&action) &&
        action==GRAPHICS_POLICY_HEADLESS;
    if(!ok)g.policy.failed=true;
    return ok;
}
static bool stop_io(void) {
    bool ok=true;
    if (polaris_mmio_binding_valid(&g.binding)) ok=polaris_mmio_close(&g.binding) && ok;
    if (g.hold_id) {
        if (polaris_mmio_hold_release(&g.hold,g.hold_id)) g.hold_id=0; else ok=false;
    }
    if (polaris_mmio_binding_valid(&g.binding)) ok=polaris_mmio_cleanup(&g.binding) && ok;
    return ok;
}
static bool quarantine(void) {
    if (!pci_assignment_valid(&g.assignment)) return true;
    if (!pci_assignment_owner_process_id(&g.assignment)) return pci_assignment_destroy(&g.assignment);
    return pci_assignment_force_quarantine(&g.assignment,process_kernel()->id);
}
static bool drop_cap(void) {
    if (!g.cap) return true;
    if (!capability_revoke(process_capabilities(process_kernel()),g.cap)) return false;
    g.cap=0;return true;
}
static bool release_writer(PolarisDriver *d) {
    /* The live-service PID accessor returns zero after a fault. Keep the
     * original launch identity until its alias is gone and lease reclaimed. */
    u64 pid=d->process_id;
    if (!polaris_driver_stop(d)) return false;
    if (g.writer!=d) return true;
    DisplayLeaseState lease=display_direct_state();
    bool ok=lease==DISPLAY_LEASE_KERNEL || (lease==DISPLAY_LEASE_HANDOFF ? display_direct_abort() :
        lease==DISPLAY_LEASE_USER && display_direct_reclaim(pid));
    if (ok) g.writer=0;
    return ok;
}
static void failed(void) {
    terminal_writeln("NATIVE OPERATION FAILED; STOPPING OWNERS / RETAINING DEVICE QUARANTINE.");
    if (g.output_id) (void)framebuffer_output_revoke(g.output_id);
    bool q=quarantine(),a=true;
    if (g.writer) a=release_writer(g.writer);
    bool b=polaris_driver_stop(&g.primary),c=polaris_driver_stop(&g.rescue);
    bool clean=stop_io();g.stage=3;
    bool drained=q&&a&&b&&c&&clean;
    if (drained) drained=drop_cap();
    (void)policy_close();
    if(g.policy.failed)drained=supervisor_stop_identity(g.policy.process_id,g.policy.thread_id)&&drained;
    if (framebuffer_firmware_valid() && !g.writer) terminal_set_render_enabled(g.original_render);
    terminal_writeln(drained ? "CPU RESOURCES DRAINED; GPU PARENT QUARANTINED; SERIAL CONTROL ACTIVE." :
        "CLEANUP INCOMPLETE; EXACT LEDGERS/PINS RETAINED; SERIAL CONTROL ACTIVE.");
}
static void request_rescue(bool induce) {
    if (g.stage!=1) {terminal_writeln("REFUSED: PRIMARY NATIVE STAGE REQUIRED; RESCUE IS ONE-SHOT.");return;}
    u64 action;
    if (!graphics_supervisor_request(&g.policy,GRAPHICS_POLICY_REQUEST_RESCUE,0,&action) ||
        action!=GRAPHICS_POLICY_RECOVER) {failed();return;}
    g.recovery_authorized=true;
    recover_native(induce);
}
static bool admitted(void *unused) {
    (void)unused;
    return g.frequency && timer_frequency()==g.frequency &&
        timer_ticks()-g.start<(u64)g.frequency*2 && device_mmio_interrupts_enabled();
}
static bool wait_tick(void *unused) {
    (void)unused; u64 t=timer_ticks();
    for (u32 n=0;n<4000000U;++n) {
        if (!admitted(0)) return false;
        if (timer_ticks()!=t) return true;
        arch_pause();
    }
    return false;
}
static bool read_value(void *unused,PolarisRebindRead f,u32 *out) {
    (void)unused;
    static const u32 selectors[6]={POLARIS_REG_PRIMARY_LOW,POLARIS_REG_PRIMARY_HIGH,
        POLARIS_REG_CRTC_CONTROL,15+POLARIS_WITNESS_UPDATE,15+POLARIS_WITNESS_FLIP_CONTROL,15+POLARIS_WITNESS_FRAME_COUNT};
    if ((u32)f>=6 || !admitted(0)) return false;
    terminal_write("NATIVE READ OP=");terminal_write_u64(f);terminal_putchar('\n');
    bool ok=admitted(0) && polaris_mmio_rebind_read(&g.hold,g.hold_id,selectors[f],out);
    if (ok) {terminal_write(" VALUE=");terminal_write_hex(*out);terminal_putchar('\n');}
    return ok;
}
static bool write_value(void *unused,PolarisDisplayRegister f,u32 v) {
    (void)unused;
    terminal_write("GPU WRITE ");terminal_write_u64(f);terminal_write(" VALUE=");terminal_write_hex(v);terminal_putchar('\n');
    return admitted(0) && polaris_mmio_rebind_write(&g.hold,g.hold_id,f,v);
}
static bool execute(PolarisDriver *driver,PolarisProgramPlan *p) {
    PolarisProgramIo io;io.context=0;io.admitted=admitted;io.read=read_value;io.write=write_value;io.wait=wait_tick;
    PolarisProgramResult r;g.start=timer_ticks();g.frequency=timer_frequency();
    bool ok=polaris_driver_program(driver,p,&io,&r);
    terminal_writeln("RING3 SEQUENCER / INDEPENDENT KERNEL GATE; NO RAW REGISTER GRANT.");
    terminal_write("RING3 PROGRAM OWNER PID=");terminal_write_u64(driver->process_id);
    terminal_write(" INCARNATION=");terminal_write_u64(driver->connection.incarnation);terminal_putchar('\n');
    terminal_write("REBIND RESULT: WRITES ATTEMPTED=");terminal_write_u64(r.write_attempts);
    terminal_write(" READS=");terminal_write_u64(r.reads);terminal_write(" POLLS=");terminal_write_u64(r.polls);
    terminal_write(" FRAME DELTA=");terminal_write_u64(r.frame_delta);terminal_putchar('\n');
    return ok;
}
static bool initial_read(u32 pipe,u32 selector,u32 *v) {
    if (!admitted(0)) return false;
    terminal_write("NATIVE PREFLIGHT PIPE=");terminal_write_u64(pipe);
    terminal_write(" SELECTOR=");terminal_write_u64(selector);terminal_putchar('\n');
    if (!admitted(0)) return false;
    bool ok=false;
    if (g.mutated) ok=pipe==0 && polaris_mmio_rebind_read(&g.hold,g.hold_id,selector,v);
    else if (selector<15) ok=polaris_mmio_read_held(&g.hold,g.hold_id,process_kernel(),pipe,(PolarisDisplayRegister)selector,v);
    else if (selector<18) ok=polaris_mmio_read_witness_held(&g.hold,g.hold_id,process_kernel(),pipe,(PolarisPlaneWitnessRegister)(selector-15),v);
    else if (selector<20) ok=polaris_mmio_read_location_held(&g.hold,g.hold_id,process_kernel(),(PolarisLocationRegister)(selector-18),v);
    if (ok) {terminal_write(" VALUE=");terminal_write_hex(*v);terminal_putchar('\n');}
    return ok;
}
static bool observe(PolarisPlaneWitness *w) {
    u32 values[15],again[15],locations[2][2];
    g.frequency=timer_frequency();g.start=timer_ticks();
    if (!g.mutated) {
        PolarisObservationBatch b;polaris_observation_init(&b);
        for (u32 i=0;i<6;++i) {u32 p,f,v;
            if (!polaris_observation_next(&b,&p,&f) || !initial_read(p,f,&v) || !polaris_observation_accept(&b,v)) return false;
        }
        if (b.pipe!=0) return false;
    }
    for (u32 sample=0;sample<2;++sample) {
        for (u32 f=0;f<2;++f) if (!initial_read(0,18+f,&locations[sample][f])) return false;
        for (u32 f=0;f<3;++f) if (!initial_read(0,15+f,&w->values[sample][f])) return false;
        u32 *row=sample ? again : values;
        for (u32 f=0;f<15;++f) if (!initial_read(0,f,&row[f])) return false;
        if (!sample && !wait_tick(0)) return false;
    }
    for (u32 f=0;f<15;++f) if (values[f]!=again[f]) return false;
    PolarisVramObservation v;
    return !polaris_plane_blockers(w) && !w->values[0][0] && !w->values[1][0] &&
        polaris_vram_observe(g.boot,&g.aperture,values,locations[0],locations[1],&v) &&
        v.surface_gpu_address==0xF400000000ULL && v.surface_cpu_address==g.info.physical_base;
}
static bool show(PolarisDriver *d,bool rescue) {
    ProgramBorrowedMappingSpec mapping;FramebufferInfo info;u64 fb;
    if (!d->process_id || managed_service_process_id(&d->service)!=d->process_id ||
        !framebuffer_native_install(&g.info,rescue,&g.output_id)) return false;
    g.writer=d;
    if (!display_direct_prepare_native(g.output_id,POLARIS_NATIVE_FB_VA,&mapping,&info,&fb) ||
        mapping.physical_base!=g.info.physical_base || mapping.size!=0x300000 ||
        !program_borrowed_set_writable(&d->service.program,managed_service_process_id(&d->service),true) ||
        !display_direct_commit(&d->service.program.process) || !polaris_driver_draw(d)) return false;
    g.last_heartbeat=timer_ticks();
    return true;
}
void polaris_native_begin(const BootInfo *boot) {
    terminal_writeln("POLARIS NATIVE REBIND V1 / EXPLICIT SUPERVISED EXPERIMENT");
    if (g.attempted) {terminal_writeln("REFUSED: ONE NATIVE ATTEMPT PER BOOT; NO ASSIGNMENT REUSE.");return;}
    PolarisReadReport report;
    const u32 local=POLARIS_READ_BLOCK_TARGET|POLARIS_READ_BLOCK_BAR_PLAN|POLARIS_READ_BLOCK_POWER|
        POLARIS_READ_BLOCK_CPU|POLARIS_READ_BLOCK_UART|POLARIS_READ_BLOCK_SCANOUT;
    if (!boot || !polaris_read_profile_inspect(boot,&report) || (report.blockers&local) ||
        !serial_available() || !timer_initialized() || !scheduler_preemption_enabled() ||
        !device_mmio_interrupts_enabled() || display_direct_state()!=DISPLAY_LEASE_KERNEL ||
        !framebuffer_firmware_valid() || !framebuffer_writers_idle() || !framebuffer_info(&g.info) ||
        g.info.physical_base!=0x50000000 || g.info.size!=0x300000 || !g.info.direct_map_safe ||
        g.info.width!=1024 || g.info.height!=768 || g.info.pixels_per_scanline!=1024 || g.info.pixel_format!=1) {
        terminal_writeln("REFUSED: EXACT BOARD/GOP/D0/CPU/SERIAL/KERNEL-LEASE PROFILE REQUIRED.");return;
    }
    const PciDevice *selected=0;
    for (u32 i=0;i<pci_device_count();++i) if (display_probe_matches_gop(boot,pci_device(i))) {
        if (selected) return;
        selected=pci_device(i);
    }
    if (!selected) return;
    g.attempted=true;g.boot=boot;g.original_render=terminal_render_enabled();
    terminal_writeln("PROFILE: OPERATIONAL GPU / UNCHANGED INHERITED DP TIMING; NO HOTPLUG/POWER/RESET.");
    terminal_writeln("RECOVERY SCOPE: SOFTWARE OWNER FAILURE; NO NEW IRQ/DMA; NO GPU-HANG GUARANTEE.");
    DisplayProbeInfo inventory;
    if (!display_probe_read(selected,&inventory) || !display_probe_firmware_bar(boot,selected,&inventory,0,&g.aperture)) goto fail;
    ProgramBorrowedMappingSpec mapping;
    mapping.physical_base=g.info.physical_base;mapping.size=g.info.size;mapping.virtual_base=POLARIS_NATIVE_FB_VA;mapping.flags=VM_UNCACHED;
    if (!polaris_driver_reserve(&g.primary,1,&mapping) || !polaris_driver_reserve(&g.rescue,2,&mapping)) goto fail;
    terminal_writeln("PRIMARY AND SEPARATE RESCUE ELF/FONT/IPC/RO FRAMEBUFFER RESOURCES RESERVED.");
    if (!graphics_supervisor_begin(&g.policy)) goto fail;
    terminal_writeln("RING3 GRAPHICS POLICY SESSION ADMITTED; DEVICE AUTHORITY REMAINS KERNEL ENFORCED.");
    if (!pci_assignment_create(&g.assignment,selected) || !pci_assignment_claim_supervised(&g.assignment,&g.cap) ||
        !polaris_mmio_prepare_supervised(&g.binding,boot,&g.assignment,g.cap) ||
        !polaris_mmio_prepare_location(&g.binding,boot,process_kernel(),polaris_mmio_binding_identity(&g.binding)) ||
        !polaris_mmio_hold_acquire(&g.hold,&g.binding,polaris_mmio_binding_identity(&g.binding),process_kernel(),&g.hold_id)) goto fail;
    terminal_set_render_enabled(false);
    PolarisPlaneWitness witness;PolarisProgramPlan plan;
    if (!observe(&witness) || !polaris_driver_plan(&g.primary,1,&witness,&plan)) goto fail;
    if (!managed_service_idle(&g.rescue.service) || !display_retire_firmware_scanout()) goto fail;
    g.mutated=true;terminal_writeln("GOP PERMANENTLY RETIRED BEFORE RW MMIO; TWO FIXED ADDRESS WRITES ONLY.");
    if (!polaris_mmio_enable_rebind(&g.hold,g.hold_id) || !execute(&g.primary,&plan)) goto fail;
    g.stage=1;
    u64 action;
    if (!graphics_supervisor_request(&g.policy,GRAPHICS_POLICY_PRIMARY,0,&action) ||
        action!=GRAPHICS_POLICY_KEEP) goto fail;
    if (!show(&g.primary,false)) {request_rescue(false);return;}
    terminal_writeln("PRIMARY RING3 PATTERN ACTIVE; VERIFY MONITOR. USE polaris-rescue OVER SERIAL.");
    return;
fail:
    failed();
}
static void recover_native(bool induce) {
    if (g.stage!=1 || !g.recovery_authorized) {failed();return;}
    g.recovery_authorized=false;
    /* The kernel revokes first, then induces the supported process fault.
     * No failed heap/register dump or parent reassignment supplies recovery. */
    g.stage=4;
    if ((g.output_id && !framebuffer_output_revoke(g.output_id)) || !quarantine()) {failed();return;}
    IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=POLARIS_NATIVE_HEADER(POLARIS_NATIVE_FAULT);
    if (induce) (void)managed_service_call(&g.primary.service,&g.primary.connection,&q,&r);
    if (!release_writer(&g.primary) || !polaris_mmio_arm_recovery(&g.hold,g.hold_id)) {failed();return;}
    terminal_writeln("OLD PRIMARY STOPPED/REAPED; MMIO PARENT STILL QUARANTINED; RESERVED RESCUE ENTERING.");
    PolarisPlaneWitness witness;PolarisProgramPlan plan;
    if (!observe(&witness) || !polaris_driver_plan(&g.rescue,2,&witness,&plan) ||
        !execute(&g.rescue,&plan) || !show(&g.rescue,true)) {failed();return;}
    g.stage=2;
    u64 action;
    if (!graphics_supervisor_request(&g.policy,GRAPHICS_POLICY_RESCUED,0,&action) ||
        action!=GRAPHICS_POLICY_KEEP) {failed();return;}
    terminal_writeln("FRESH RING3 RESCUE PATTERN ACTIVE; VERIFY MONITOR. USE polaris-finish FOR CONSOLE.");
}
void polaris_native_rescue(void) {request_rescue(true);}
void polaris_native_poll(void) {
    if (g.stage!=1 && g.stage!=2) return;
    if(!supervisor_running() || supervisor_process_id()!=g.policy.process_id ||
        supervisor_thread_id()!=g.policy.thread_id) {
        terminal_writeln("RING3 GRAPHICS POLICY IDENTITY LOST; FAILING CLOSED.");failed();return;
    }
    if (!timer_initialized() || !g.frequency || timer_frequency()!=g.frequency ||
        !device_mmio_interrupts_enabled() || !scheduler_preemption_enabled()) {failed();return;}
    bool primary=g.stage==1 && managed_service_state(&g.primary.service)==MANAGED_SERVICE_RUNNING;
    bool rescue=managed_service_state(&g.rescue.service)==MANAGED_SERVICE_RUNNING;
    bool healthy=rescue && (g.stage==2 || primary),probed=false,pp=false,rp=false;
    u64 facts=(primary ? GRAPHICS_POLICY_PRIMARY_LIVE : 0) |
        (rescue ? GRAPHICS_POLICY_RESCUE_LIVE : 0);
    u64 now=timer_ticks();
    if (healthy) {
        if (now-g.last_heartbeat<(u64)g.frequency) return;
        g.last_heartbeat=now;
        if (g.challenge==~0ULL) {failed();return;}
        if (g.stage==1) pp=polaris_driver_ping(&g.primary,++g.challenge);
        if (g.challenge==~0ULL) {failed();return;}
        rp=polaris_driver_ping(&g.rescue,++g.challenge);
        probed=true;
        facts|=GRAPHICS_POLICY_PROBED | (pp ? GRAPHICS_POLICY_PRIMARY_PONG : 0) |
            (rp ? GRAPHICS_POLICY_RESCUE_PONG : 0);
    }
    u64 action;
    if (!graphics_supervisor_request(&g.policy,GRAPHICS_POLICY_OBSERVE,facts,&action)) {
        terminal_writeln("RING3 GRAPHICS POLICY UNAVAILABLE/STALE; FAILING CLOSED.");failed();return;
    }
    if (action==GRAPHICS_POLICY_KEEP) {
        /* A faulty policy cannot retain a known-dead/nonresponding CPU writer. */
        if (!healthy || (probed && (!rp || (g.stage==1 && !pp)))) {failed();return;}
        if (probed) ++g.heartbeats;
        return;
    }
    terminal_write("RING3 GRAPHICS POLICY ACTION=");terminal_write_u64(action);terminal_putchar('\n');
    if (action==GRAPHICS_POLICY_RECOVER && g.stage==1 && rescue && (!probed || rp)) {
        terminal_writeln(primary ? "PRIMARY HEARTBEAT FAILED; RING3 POLICY REQUESTED RESERVED RESCUE." :
            "PRIMARY TERMINAL OUTCOME; RING3 POLICY REQUESTED RESERVED RESCUE.");
        g.recovery_authorized=true;recover_native(false);return;
    }
    if (action==GRAPHICS_POLICY_HEADLESS) {
        terminal_writeln("RING3 GRAPHICS POLICY SELECTED HEADLESS; NO FURTHER GPU OPERATION.");
    } else terminal_writeln("RING3 GRAPHICS POLICY ACTION REFUSED BY KERNEL STAGE/LIVENESS GUARD.");
    failed();
}
static void diagnostic(u32 role,bool spin,bool probe) {
    PolarisDriver *d=role==1 ? &g.primary : &g.rescue;
    if ((role!=1 && role!=2) || (role==1 ? g.stage!=1 : g.stage!=2) ||
        g.writer!=d || (g.diagnostic_mask&(1U<<role))) {
        terminal_writeln("REFUSED: ONE DIAGNOSTIC PER ACTIVE PRIMARY/RESCUE; NO GPU RETRY.");return;
    }
    g.diagnostic_mask|=1U<<role;
    if (!polaris_driver_diagnostic(d,spin)) {
        terminal_writeln("DIAGNOSTIC DELIVERY/ACK FAILED; RETAINING QUARANTINE.");failed();return;
    }
    if(probe) {
        PolarisDriverProgress p;
        bool progress=polaris_driver_console_progress(d,spin,&p);
        terminal_putchar('\n');
        terminal_write("INDEPENDENT RING3 CONSOLE PID=");terminal_write_u64(p.console_process_id);
        terminal_write(" INC=");terminal_write_u64(p.console_incarnation);
        terminal_write(" WRITES BEFORE=");terminal_write_u64(p.writes_before);
        terminal_write(" AFTER=");terminal_write_u64(p.writes_after);
        terminal_writeln(progress ? " PROGRESS: PASS" : " PROGRESS: FAILED");
        if (!progress) {failed();return;}
    }
    terminal_writeln(spin ? "SPIN ACKNOWLEDGED; WATCHDOG WILL OBSERVE UNRESPONSIVE DRIVER." :
        "FAULT ACKNOWLEDGED; WATCHDOG WILL OBSERVE DRIVER EXIT.");
}
void polaris_native_diagnostic(u32 role,bool spin) {diagnostic(role,spin,true);}
void polaris_native_background_diagnostic(u32 role,bool spin) {diagnostic(role,spin,false);}
bool polaris_native_can_diagnostic(u32 role) {
    if(role==3)return (g.stage==1 || g.stage==2) && !(g.diagnostic_mask&8U);
    return (role==1 ? g.stage==1 : role==2 && g.stage==2) && !(g.diagnostic_mask&(1U<<role));
}
bool polaris_native_terminal(void) {return !g.attempted || g.stage==3;}
void polaris_native_abort(void) {if(g.attempted)failed();}
static void policy_diagnostic(bool spin,bool probe) {
    if((g.stage!=1 && g.stage!=2) || !g.writer || (g.diagnostic_mask&8U) ||
        supervisor_process_id()!=g.policy.process_id || supervisor_thread_id()!=g.policy.thread_id) {
        terminal_writeln("REFUSED: ONE POLICY DIAGNOSTIC DURING ACTIVE NATIVE OUTPUT.");return;
    }
    g.diagnostic_mask|=8U;
    bool injected=spin ? supervisor_test_spin() : supervisor_test_fault();
    PolarisDriverProgress p;
    bool progress=injected && (!probe || polaris_driver_console_progress(g.writer,false,&p));
    if(!injected){terminal_writeln("POLICY DIAGNOSTIC DELIVERY FAILED; FAILING CLOSED.");failed();return;}
    if(!progress){terminal_writeln("POLICY FAILURE INDEPENDENT CONSOLE WORK: FAILED");failed();return;}
    if(probe) {
        terminal_write("POLICY FAILURE CONSOLE PID=");terminal_write_u64(p.console_process_id);
        terminal_write(" INC=");terminal_write_u64(p.console_incarnation);
        terminal_write(" WRITES BEFORE=");terminal_write_u64(p.writes_before);
        terminal_write(" AFTER=");terminal_write_u64(p.writes_after);terminal_writeln(" PROGRESS: PASS");
    }
    terminal_writeln(spin ? "POLICY SPIN ACKNOWLEDGED; INDEPENDENT WORKER WILL FAIL CLOSED." :
        "POLICY FAULT OBSERVED; INDEPENDENT WORKER WILL FAIL CLOSED.");
}
void polaris_native_policy_diagnostic(bool spin) {policy_diagnostic(spin,true);}
void polaris_native_background_policy_diagnostic(bool spin) {policy_diagnostic(spin,false);}
void polaris_native_status(void) {
    terminal_write("POLARIS STATUS: STAGE=");terminal_write_u64(g.stage);
    terminal_write(" HEARTBEATS=");terminal_write_u64(g.heartbeats);
    terminal_write(" DIAGNOSTIC MASK=");terminal_write_hex(g.diagnostic_mask);terminal_putchar('\n');
    terminal_writeln("ONCE-PER-BOOT GPU BUDGET; STATUS PERFORMS NO GPU IO.");
}
void polaris_native_finish(void) {
    if (g.stage!=1 && g.stage!=2) {terminal_writeln("NO VALIDATED NATIVE/RESCUE OUTPUT TO FINISH.");return;}
    if (!release_writer(g.stage==1 ? &g.primary : &g.rescue) ||
        !polaris_driver_stop(&g.primary) || !polaris_driver_stop(&g.rescue) ||
        !quarantine() || !stop_io() || !drop_cap() || !policy_close()) {failed();return;}
    g.stage=3;terminal_set_render_enabled(true);
    terminal_writeln("VALIDATED NATIVE/RESCUE CONSOLE RECLAIMED; ORIGINAL GOP REMAINS RETIRED.");
    terminal_writeln("GPU PARENT QUARANTINED; NO DMA/IRQ/RESET OR NEW MODE CLAIM.");
}
bool polaris_native_power_prepare(void) {
    if (!g.attempted) return true;
    g.stage=3; /* Power transition never initiates another GPU operation. */
    if (g.writer && !release_writer(g.writer)) return false;
    if (!polaris_driver_stop(&g.primary) || !polaris_driver_stop(&g.rescue) ||
        !quarantine() || !stop_io() || !drop_cap()) return false;
    (void)policy_close();
    if(g.policy.failed && !supervisor_stop_identity(g.policy.process_id,g.policy.thread_id))return false;
    if (framebuffer_output_valid()) terminal_set_render_enabled(true);
    return true;
}
#else
void polaris_native_background_diagnostic(u32 r,bool s){(void)r;(void)s;}
void polaris_native_background_policy_diagnostic(bool s){(void)s;}
bool polaris_native_can_diagnostic(u32 r){(void)r;return false;}
bool polaris_native_terminal(void){return true;}
void polaris_native_abort(void){}
void polaris_native_policy_diagnostic(bool spin){(void)spin;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
bool polaris_native_power_prepare(void) {return true;}
void polaris_native_poll(void) {}
void polaris_native_diagnostic(u32 role,bool spin) {(void)role;(void)spin;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_native_status(void) {terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_native_begin(const BootInfo *boot) {(void)boot;terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_native_rescue(void) {terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
void polaris_native_finish(void) {terminal_writeln("POLARIS NATIVE WRITES DISABLED IN THIS BUILD.");}
#endif
