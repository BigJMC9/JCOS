#include "native_presentation_test.h"
#include "address_space.h"
#include "capability.h"
#include "compositor_service.h"
#include "display.h"
#include "display_service.h"
#include "endpoint.h"
#include "framebuffer.h"
#include "graphical_session.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "terminal.h"
#include "thread.h"
#include "display_service_protocol.h"

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 address_spaces;
    u32 threads;
    u32 endpoints;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    bool terminal_render;
} DisplayServiceBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(DisplayServiceBaseline *out) {
    if (!out) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->threads = thread_object_count();
    out->endpoints = endpoint_object_count();
    out->exit_queues = process_exit_queue_object_count();
    out->kernel_caps = capability_table_count(caps);
    out->kernel_threads = process_thread_count(kernel);
    out->scheduler_threads = scheduler_thread_count();
    out->terminal_render = terminal_render_enabled();
    return true;
}

static bool baseline_matches(const DisplayServiceBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        terminal_render_enabled() == baseline->terminal_render;
}


/* Never manufactures native output or runs a GPU operation. A positive run
 * needs a real, previously validated generation and drained old owners. */
static bool g_owned;
static bool output_retained(u64 id) {
    FramebufferScanoutStatus scanout;
    framebuffer_scanout_status(&scanout);
    return id && framebuffer_output_matches(id) && !framebuffer_firmware_valid() &&
        scanout.state == FRAMEBUFFER_SCANOUT_RETIRED;
}
static bool clean(void) {
    if (g_owned && !graphical_session_cleanup()) return false;
    g_owned=false;
    return graphical_session_drained() && !graphical_session_active() &&
        display_service_state()==MANAGED_SERVICE_STOPPED &&
        compositor_service_state()==MANAGED_SERVICE_STOPPED &&
        !display_service_output_identity() && !display_service_surface_count() &&
        !display_service_input_client_count() && display_direct_state()==DISPLAY_LEASE_KERNEL &&
        !display_direct_owner_process_id();
}
void native_presentation_test_cleanup_run(void) {
    report("NATIVE PRESENTATION CLEANUP",clean());
}
void native_presentation_test_run(void) {
    terminal_writeln("NATIVE PRESENTATION TEST:");
    FramebufferOutputStatus output;
    framebuffer_output_status(&output);
    DisplayServiceBaseline baseline;
    if (g_owned || graphical_session_active() ||
        display_service_state()!=MANAGED_SERVICE_STOPPED ||
        compositor_service_state()!=MANAGED_SERVICE_STOPPED ||
        display_direct_state()!=DISPLAY_LEASE_KERNEL ||
        display_direct_owner_process_id() || !capture_baseline(&baseline)) goto fail;
    if (!output.identity) {
        bool refused=!graphical_session_take_reconnect() && !display_service_start_native(0) &&
            !display_service_start_native(1) && !display_service_start_native(~0ULL) &&
            clean() && baseline_matches(&baseline);
        report("NATIVE ADMISSION REFUSED WITHOUT VALIDATED GENERATION",refused);
        terminal_writeln("ADMISSION ONLY; NO NATIVE PRESENTATION OR GPU QUALIFICATION.");
        terminal_writeln(refused ? "NATIVE PRESENTATION TEST: PASS" : "NATIVE PRESENTATION TEST: FAILED");
        return;
    }
    if (!output_retained(output.identity) ||
        (output.kind!=FRAMEBUFFER_OUTPUT_NATIVE && output.kind!=FRAMEBUFFER_OUTPUT_RESCUE)) goto fail;
    bool admission=!display_service_start() &&
        !display_service_start_native(output.identity+1ULL) && clean() && baseline_matches(&baseline);
    report("RETIRED GOP AND STALE GENERATION REFUSED",admission);
    if (!admission) goto fail;
    u64 previous_pid=0,previous_incarnation=0;
    for (u32 cycle=0;cycle<3U;++cycle) {
        g_owned=true;
        bool started=graphical_session_start() && graphical_session_active() && !graphical_session_drained() &&
            display_service_running() && compositor_service_running() &&
            display_service_output_identity()==output.identity && output_retained(output.identity) &&
            display_direct_state()==DISPLAY_LEASE_USER && !terminal_render_enabled();
        u64 pid=display_service_process_id(),inc=display_service_incarnation();
        started=started && pid && inc && pid!=previous_pid && inc!=previous_incarnation &&
            pid!=compositor_service_process_id() && display_direct_owner_process_id()==pid;
        report("FRESH NORMAL GUI ON RETAINED NATIVE GENERATION",started);
        if (!started) goto fail;
        previous_pid=pid;previous_incarnation=inc;
        KeyEvent event={0},observed={0};
        event.key=KEY_CHARACTER;event.character='N';event.pressed=true;
        u32 count=0,sample=0;
        bool presented=graphical_session_handle_event(&event) && graphical_session_event_count()==1ULL &&
            graphical_session_last_event(&observed) && observed.key==event.key &&
            observed.character==event.character && observed.pressed &&
            display_service_compose(&count,&sample) && count==1U;
        report("NATIVE OUTPUT / REAL APP INPUT / SEPARATE COMPOSITOR PRESENT",presented);
        if (!presented) goto fail;
        if (cycle) {
            ProcessExitInfo exit={0};
            bool contained=cycle==1U ? graphical_session_test_fault_display_service(&exit) :
                graphical_session_test_fault_compositor(&exit);
            contained=contained && exit.reason==PROCESS_EXIT_FAULT && exit.vector==6ULL &&
                exit.process_id && exit.thread_id && output_retained(output.identity) &&
                display_direct_state()==DISPLAY_LEASE_USER && display_direct_owner_process_id()==pid &&
                !terminal_render_enabled() && (cycle==1U ? compositor_service_running() : display_service_running());
            report(cycle==1U ? "NATIVE PRESENTER FAULT / LEASE RETAINED / COMPOSITOR AND CLIENT SURVIVE" :
                "NATIVE COMPOSITOR FAULT / PRESENTER AND CLIENT SURVIVE",contained);
            if (!contained) goto fail;
        }
        bool reclaimed=clean() && !graphical_session_take_reconnect() && output_retained(output.identity) && baseline_matches(&baseline);
        report("NATIVE CONSOLE AND FULL RESOURCE BASELINE RESTORED",reclaimed);
        if (!reclaimed) goto fail;
    }
    terminal_writeln("NATIVE PRESENTATION TEST: PASS");
    terminal_writeln("REUSED VALIDATED SCANOUT; NO GPU RESET, NEW MODE OR RESCUE REARM.");
    return;
fail:
    if (g_owned) (void)clean();
    terminal_writeln("NATIVE PRESENTATION TEST: FAILED");
}
