#include "polaris_native_test.h"
#include "polaris_vtotal_runtime_test.h"
#include "polaris_driver.h"
#include "pmm.h"
#include "physmap.h"
#include "terminal.h"
#include "lib.h"
#include "device_resource.h"
#include "address_space.h"
#include "process.h"
#include "endpoint.h"
#include "font8x8.h"
#include "pci_assignment.h"
#include "timer.h"
#include "scheduler.h"
#include "interrupts.h"
static PolarisDriver primary,rescue;
static u64 storage;
static PciAssignment model_assignment;
static CapabilityHandle model_cap;
static bool model_child;
static bool release_model(void) {
    Process *kernel=process_kernel();
    if (model_child) {
        if (!pci_assignment_child_release(&model_assignment,kernel->id)) return false;
        model_child=false;
    }
    if (model_cap) {
        if (!capability_revoke(process_capabilities(kernel),model_cap)) return false;
        model_cap=0;
    }
    if (!pci_assignment_valid(&model_assignment)) return true;
    if (pci_assignment_state(&model_assignment)==PCI_ASSIGNMENT_ASSIGNED &&
        !pci_assignment_begin_quiesce(&model_assignment,kernel->id)) return false;
    if (pci_assignment_state(&model_assignment)==PCI_ASSIGNMENT_QUIESCING &&
        !pci_assignment_complete_release(&model_assignment,kernel->id)) return false;
    return pci_assignment_destroy(&model_assignment);
}
static bool stop_all(void) {
    bool a=polaris_driver_stop(&primary),b=polaris_driver_stop(&rescue);
    if (!a || !b || !release_model()) return false;
    if (storage) {if (!frame_free_range(phys_to_frame(storage),768)) return false;storage=0;}
    return true;
}
/* Check every foreground/background bit of an asymmetric title against the
 * canonical MSB-first font, not merely whether any white pixel was drawn. */
static bool title_matches(const u32 *pixels,bool is_rescue) {
    const char *title=is_rescue ? "JA OS - FRESH RING3 RESCUE DISPLAY" :
        "JA OS - RING3 POLARIS SAME-BUFFER REBIND";
    u32 bg=is_rescue ? 0x182030U : 0x103018U;
    if (pixels[0]!=bg || pixels[1024U*768U-1U]!=bg) return false;
    for (u32 n=0;title[n];++n) for (u32 row=0;row<8;++row) for (u32 col=0;col<8;++col) {
        u32 expected=(font8x8[(u8)title[n]][row]&(1U<<(7U-col))) ? 0xFFFFFFU : bg;
        if (pixels[(40U+row)*1024U+32U+n*8U+col]!=expected) return false;
    }
    return true;
}
static bool wait_failed(PolarisDriver *d) {
    if (!timer_initialized() || !timer_frequency()) return false;
    u64 start=timer_ticks(),limit=(u64)timer_frequency()*2ULL;
    while (managed_service_state(&d->service)==MANAGED_SERVICE_RUNNING) {
        if (timer_ticks()-start>=limit) return false;
        u64 flags;__asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
        interrupts_disable();(void)scheduler_yield();
        if (flags&(1ULL<<9)) interrupts_enable();
    }
    return managed_service_state(&d->service)==MANAGED_SERVICE_FAILED;
}
/* Actual Ring3 sequence and private broker; effectors touch counters only. */
typedef struct {u32 writes,frame;bool abort_after_writes;} RamProgram;
static bool ram_admitted(void *c){(void)c;return true;}
static bool ram_write(void *c,PolarisDisplayRegister field,u32 value) {
    RamProgram *r=c;
    if(r->writes>=2 || field!=(r->writes?POLARIS_REG_PRIMARY_LOW:POLARIS_REG_PRIMARY_HIGH) ||
       value!=(r->writes?0U:0xF4U))return false;
    ++r->writes;return true;
}
static bool ram_read(void *c,PolarisRebindRead field,u32 *out) {
    RamProgram *r=c;
    if(r->abort_after_writes && r->writes)return false;
    switch(field) {
    case POLARIS_REBIND_LOW:*out=0;break;
    case POLARIS_REBIND_HIGH:*out=0xF4;break;
    case POLARIS_REBIND_CRTC:*out=0x80410311;break;
    case POLARIS_REBIND_UPDATE:*out=0;break;
    case POLARIS_REBIND_FLIP:*out=0x20;break;
    case POLARIS_REBIND_FRAME:r->frame+=3;*out=r->frame;break;
    default:return false;
    }
    return true;
}
static bool ram_wait(void *c){(void)c;return true;}
static bool program_ram(PolarisDriver *d,PolarisProgramPlan *plan) {
    RamProgram r={0};PolarisProgramIo io={&r,ram_admitted,ram_write,ram_read,ram_wait};
    PolarisProgramResult result;
    return polaris_driver_program(d,plan,&io,&result) && result.complete &&
        result.reads==14 && result.write_attempts==2 && result.polls==1 &&
        result.frame_delta==3 && r.writes==2;
}
static void report(const char *s,bool pass){terminal_write("  ");terminal_write(s);terminal_writeln(pass ? ": PASS" : ": FAILED");}
void polaris_native_test_cleanup_run(void){report("RESERVED PRIMARY/RESCUE/PAGES CLEANUP",stop_all());}
void polaris_native_test_run(void) {
    terminal_writeln("POLARIS NATIVE / RESERVED RESCUE TEST (RAM ONLY):");
    if (!stop_all()) goto fail;
    u64 pages=pmm_stats().free_pages;
    u32 processes=process_object_count(),spaces=address_space_object_count(),objects=device_resource_object_count(),endpoints=endpoint_object_count();
    CapabilityHandle refused=~0ULL;
    bool supervised=pci_device_count() && pci_assignment_create(&model_assignment,pci_device(0)) &&
        !pci_assignment_claim(&model_assignment,process_kernel(),&refused) && !refused &&
        pci_assignment_claim_supervised(&model_assignment,&model_cap) &&
        pci_assignment_authorized(&model_assignment,process_kernel(),model_cap);
    if (supervised) {
        model_child=pci_assignment_child_acquire(&model_assignment,process_kernel(),model_cap);
        supervised=model_child && release_model();
    }
    report("SUPERVISOR-OWNED ASSIGNMENT / ORDINARY KERNEL CLAIM REFUSED (MODEL)",supervised);
    if (!supervised) goto fail;
    storage=pmm_alloc_pages(768);if (!storage) goto fail;
    ProgramBorrowedMappingSpec m;
    m.physical_base=storage;m.size=0x300000;m.virtual_base=POLARIS_NATIVE_FB_VA;m.flags=0;
    PolarisPlaneWitness w;k_memset(&w,0,sizeof(w));w.values[0][1]=w.values[1][1]=0x20;w.values[0][2]=100;w.values[1][2]=110;
    for (u32 scenario=0;scenario<2;++scenario) {
        k_memset(phys_to_virt(storage),0,0x300000);
        bool ready=polaris_driver_reserve(&primary,1,&m) && polaris_driver_reserve(&rescue,2,&m);
        u64 pid=managed_service_process_id(&primary.service),rid=managed_service_process_id(&rescue.service);
        ready=ready && pid && rid && pid!=rid && primary.process_id==pid && rescue.process_id==rid &&
            primary.connection.incarnation!=rescue.connection.incarnation;
        frame_t frame;vm_flags_t flags;
        ready=ready && address_space_query_page(process_address_space(&rescue.service.program.process),m.virtual_base,&frame,&flags) &&
            frame==phys_to_frame(storage) && flags==VM_USER;
        report("INDEPENDENT PRIMARY/RESCUE ELF / PRIVATE STATE / RO RESERVED PAGES",ready);
        if (!ready) goto fail;
        PolarisProgramPlan plan;
        bool planned=polaris_driver_plan(&primary,scenario*2+1,&w,&plan);
        report("ACTUAL RING3 FIVE-STEP PLAN / NO MMIO AUTHORITY",planned);if (!planned) goto fail;
        bool submitted=program_ram(&primary,&plan);
        report("ACTUAL RING3 EXECUTOR / PRIVATE BROKER / INDEPENDENT KERNEL GATE (RAM)",submitted);
        if(!submitted)goto fail;
        bool stale=!program_borrowed_set_writable(&primary.service.program,pid+1,true);
        report("STALE PROCESS ID WRITE PROMOTION REFUSED",stale);if (!stale) goto fail;
        if (scenario) {
            if (!program_borrowed_set_writable(&primary.service.program,pid,true) || !polaris_driver_draw(&primary)) goto fail;
            bool oriented=title_matches(phys_to_virt(storage),false);
            report("PRIMARY FULL TITLE / MSB-FIRST GLYPH ORIENTATION",oriented);if (!oriented) goto fail;
            IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=POLARIS_NATIVE_HEADER(POLARIS_NATIVE_SPIN);
            bool bounded=!managed_service_call(&primary.service,&primary.connection,&q,&r);
            report("PRIMARY SPIN / BOUNDED RPC FAILURE",bounded);if (!bounded) goto fail;
        } else {
            u64 before=pmm_stats().free_pages;
            bool downgraded=program_borrowed_set_writable(&primary.service.program,pid,true) &&
                program_borrowed_set_writable(&primary.service.program,pid,false) &&
                address_space_query_page(process_address_space(&primary.service.program.process),m.virtual_base,&frame,&flags) &&
                frame==phys_to_frame(storage) && flags==VM_USER && before==pmm_stats().free_pages;
            report("ACTUAL RW TO RO DOWNGRADE / NO ALLOCATION / WRITE REVOKED",downgraded);if(!downgraded)goto fail;
            bool denied=!polaris_driver_draw(&primary);
            report("PRIMARY STORE THROUGH RO RESERVATION FAULTS",denied);if (!denied) goto fail;
        }
        bool retained_id=!managed_service_process_id(&primary.service) && primary.process_id==pid;
        report("FAILED LIVE PID HIDDEN / ORIGINAL LAUNCH ID RETAINED",retained_id);if (!retained_id) goto fail;
        ManagedServiceConnection old=primary.connection;
        if (!polaris_driver_stop(&primary)) goto fail;
        bool old_denied=!managed_service_connection_current(&primary.service,&old) &&
            !managed_service_process_id(&primary.service) && primary.process_id==pid;
        report("OLD OWNER STOPPED/REAPED / STALE CONNECTION REFUSED",old_denied);if (!old_denied) goto fail;
        bool untouched=address_space_query_page(process_address_space(&rescue.service.program.process),m.virtual_base,&frame,&flags) && flags==VM_USER;
        report("RESCUE SURVIVES PRIMARY FAILURE WITH RO MAPPING",untouched);if (!untouched) goto fail;
        if (!polaris_driver_plan(&rescue,scenario*2+2,&w,&plan) || !program_ram(&rescue,&plan)) goto fail;
        u64 before=pmm_stats().free_pages;
        bool drawn=program_borrowed_set_writable(&rescue.service.program,rid,true) &&
            before==pmm_stats().free_pages && polaris_driver_draw(&rescue);
        drawn=drawn && title_matches(phys_to_virt(storage),true);
        report("RESCUE RW UPGRADE WITHOUT ALLOCATION / EXACT MSB-FIRST TITLE PIXELS",drawn);if (!drawn) goto fail;
        if (!polaris_driver_stop(&rescue)) goto fail;
    }
    /* Private acknowledged branches let tests observe liveness without using
     * a failed driver's reply as proof of cleanup or resuming normal RPC. */
    for (u32 test=0;test<4;++test) {
        bool spinning=(test&1U)!=0;
        u32 role=test<2 ? 1U : 2U;
        if (!polaris_driver_reserve(&primary,1,&m) || !polaris_driver_reserve(&rescue,2,&m)) goto fail;
        bool heartbeat=polaris_driver_ping(&primary,0x123456789ABCDEF0ULL) &&
            polaris_driver_ping(&rescue,~0ULL);
        report("FRESH HEARTBEAT CHALLENGE / EXACT ROLE AND INCARNATION",heartbeat);if (!heartbeat) goto fail;
        PolarisDriver *target=role==1 ? &primary : &rescue;
        PolarisProgramPlan plan;u64 pid=target->process_id;
        if (!polaris_driver_plan(target,100U+test,&w,&plan) || !program_ram(target,&plan) ||
            !program_borrowed_set_writable(&target->service.program,pid,true) ||
            !polaris_driver_draw(target) || !title_matches(phys_to_virt(storage),role==2) ||
            !polaris_driver_diagnostic(target,spinning)) goto fail;
        PolarisDriverProgress work;
        bool progress=polaris_driver_console_progress(target,spinning,&work);
        report(spinning ? "ACKNOWLEDGED SPIN / INDEPENDENT RING3 CONSOLE WORK" :
            "ACKNOWLEDGED FAULT / INDEPENDENT RING3 CONSOLE WORK",progress);if (!progress) goto fail;
        bool detected=spinning ? !polaris_driver_ping(target,0x123U) : wait_failed(target);
        report(role==1 ? "PRIMARY FAULT/SPIN LIVENESS FAILURE OBSERVED" :
            "RESCUE FAULT/SPIN LIVENESS FAILURE OBSERVED",detected);if (!detected) goto fail;
        bool stopped=polaris_driver_stop(&primary);
        stopped=polaris_driver_stop(&rescue) && stopped;
        stopped=stopped && target->process_id==pid && !managed_service_process_id(&target->service) &&
            !program_instance_needs_cleanup(&target->service.program);
        report("FAULT/SPIN OWNERS REAPED / LAUNCH ID RETAINED",stopped);if (!stopped) goto fail;
    }
    if(!polaris_driver_reserve(&primary,1,&m))goto fail;
    PolarisProgramPlan incomplete;
    if(!polaris_driver_plan(&primary,200,&w,&incomplete))goto fail;
    RamProgram partial={0};partial.abort_after_writes=true;
    PolarisProgramIo partial_io={&partial,ram_admitted,ram_write,ram_read,ram_wait};
    PolarisProgramResult partial_result;
    bool aborted=!polaris_driver_program(&primary,&incomplete,&partial_io,&partial_result) &&
        partial.writes==2 && !partial_result.complete && incomplete.consumed;
    PolarisProgramResult refused_result;frame_t partial_frame;vm_flags_t partial_flags;
    aborted=aborted && !polaris_driver_program(&primary,&incomplete,&partial_io,&refused_result) &&
        partial.writes==2 &&
        address_space_query_page(process_address_space(&primary.service.program.process),m.virtual_base,&partial_frame,&partial_flags) &&
        partial_frame==phys_to_frame(storage) && partial_flags==VM_USER;
    report("PARTIAL BROKER FAILURE / NO COMPLETION OR REPLAY / RO ALIAS RETAINED",aborted);
    if(!aborted)goto fail;
    if(!polaris_driver_stop(&primary) || !polaris_driver_stop(&rescue))goto fail;
    u64 old_pid=primary.process_id,old_inc=primary.connection.incarnation;
    k_memset(phys_to_virt(storage),0,0x300000);
    if(!polaris_driver_reserve(&primary,1,&m) || !polaris_driver_reserve(&rescue,2,&m))goto fail;
    u64 adopted=0x123456789ABCDEFULL;
    bool retained=primary.process_id!=old_pid && primary.connection.incarnation!=old_inc &&
        polaris_driver_attach_retained(&primary,adopted) && primary.retained_only && primary.program_attempted &&
        polaris_driver_ping(&primary,adopted+1) && !polaris_driver_attach_retained(&primary,adopted) &&
        !polaris_driver_attach_retained(&rescue,adopted) && !polaris_driver_plan(&primary,300,&w,&incomplete) &&
        !polaris_driver_program(&primary,&incomplete,&partial_io,&refused_result) && !polaris_driver_draw(&primary);
    for(u32 i=0;retained && i<768;++i){
        frame_t f;vm_flags_t flags;
        retained=address_space_query_page(process_address_space(&primary.service.program.process),m.virtual_base+(u64)i*VM_PAGE_SIZE,&f,&flags) &&
            f==phys_to_frame(storage)+i && flags==VM_USER;
    }
    for(u32 i=0;retained && i<1024U*768U;++i)if(((u32*)phys_to_virt(storage))[i])retained=false;
    report("ACTUAL FRESH PRIMARY RETAINED ENTRY / WHOLE RANGE RO / PROGRAM AND DRAW REFUSED",retained);if(!retained)goto fail;
    IpcMessage invalid,answer;k_memset(&invalid,0,sizeof(invalid));invalid.word_count=4;
    const u32 ops[]={POLARIS_NATIVE_RETAINED,POLARIS_NATIVE_CONFIG,POLARIS_NATIVE_EXECUTE,POLARIS_NATIVE_DRAW};
    for(u32 i=0;i<ARRAY_COUNT(ops);++i){
        invalid.words[0]=POLARIS_NATIVE_HEADER(ops[i]);invalid.words[1]=ops[i]==POLARIS_NATIVE_DRAW ? 1 : adopted;
        if(!managed_service_call(&primary.service,&primary.connection,&invalid,&answer) ||
           answer.word_count!=4 || answer.words[0]!=POLARIS_NATIVE_REPLY || answer.words[1]!=primary.connection.incarnation || answer.words[2] || answer.words[3])goto fail;
    }
    retained=polaris_driver_ping(&primary,adopted+2);
    report("ACTUAL RETAINED DRIVER REJECTS DUPLICATE CONFIG EXECUTE DRAW / HEARTBEAT SURVIVES",retained);if(!retained)goto fail;
    if (!stop_all()) goto fail;
    if(!polaris_vtotal_runtime_test())goto fail;
    bool baseline=pages==pmm_stats().free_pages && processes==process_object_count() && spaces==address_space_object_count() &&
        objects==device_resource_object_count() && endpoints==endpoint_object_count();
    report("ALL PROCESSES/MAPPINGS/CAPS/PAGES RESTORED",baseline);if (!baseline) goto fail;
    terminal_writeln("POLARIS NATIVE / RESERVED RESCUE TEST: PASS");return;
fail:
    report("FAILURE CLEANUP / RETAINED RESOURCES",stop_all());
    terminal_writeln("POLARIS NATIVE / RESERVED RESCUE TEST: FAILED");
}
