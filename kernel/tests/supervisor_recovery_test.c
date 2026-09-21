#include "supervisor_recovery_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"

#define SUPERVISOR_RECOVERY_COOKIE 0x523642315245434FULL

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 queues;
    u32 tables;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 runnable;
} SupervisorRecoveryBaseline;

static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool supervisor_healthy(void) {
    u64 reply = 0;
    return supervisor_state() == SUPERVISOR_STATE_RUNNING && supervisor_running() && supervisor_idle() &&
        supervisor_stack_guarded() && supervisor_ping(SUPERVISOR_RECOVERY_COOKIE, &reply) &&
        reply == SUPERVISOR_RECOVERY_COOKIE && supervisor_idle();
}

static SupervisorRecoveryBaseline baseline_take(void) {
    SupervisorRecoveryBaseline b;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    b.free_pages = pmm_stats().free_pages;
    b.processes = process_object_count();
    b.spaces = address_space_object_count();
    b.threads = thread_object_count();
    b.endpoints = endpoint_object_count();
    b.queues = process_exit_queue_object_count();
    b.tables = capability_table_object_count();
    b.kernel_caps = caps ? capability_table_count(caps) : 0U;
    b.kernel_threads = kernel ? process_thread_count(kernel) : 0ULL;
    b.runnable = scheduler_thread_count();
    return b;
}

static bool baseline_matches(const SupervisorRecoveryBaseline *b) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return b && kernel && caps && pmm_stats().free_pages == b->free_pages &&
        process_object_count() == b->processes && address_space_object_count() == b->spaces &&
        thread_object_count() == b->threads && endpoint_object_count() == b->endpoints &&
        process_exit_queue_object_count() == b->queues && capability_table_object_count() == b->tables &&
        capability_table_count(caps) == b->kernel_caps && process_thread_count(kernel) == b->kernel_threads &&
        scheduler_thread_count() == b->runnable;
}

static bool restore_supervisor(void) {
    if (supervisor_running()) return supervisor_healthy();
    SupervisorLifecycleState state = supervisor_state();
    if (state != SUPERVISOR_STATE_STOPPED && !supervisor_recover()) return false;
    if (scheduler_thread_count() != 1ULL) return false;
    if (!supervisor_start()) return false;
    return supervisor_healthy();
}

void supervisor_recovery_test_cleanup_run(void) {
    terminal_writeln("SUPERVISOR RECOVERY CLEANUP RETRY:");
    check("SUPERVISOR RESTORED", restore_supervisor());
}

static bool graceful_case(const SupervisorRecoveryBaseline *baseline) {
    u64 old_pid = supervisor_process_id();
    u64 old_tid = supervisor_thread_id();
    SupervisorStopResult result = supervisor_stop_bounded();
    ProcessExitInfo info;
    bool exit = supervisor_last_exit_info(&info);
    bool stopped = result == SUPERVISOR_STOP_GRACEFUL && supervisor_last_stop_result() == result &&
        supervisor_state() == SUPERVISOR_STATE_STOPPED && !supervisor_running() && exit &&
        info.reason == PROCESS_EXIT_NORMAL && info.process_id == old_pid && info.thread_id == old_tid;
    if (!check("GRACEFUL STOP -> FULLY REAPED", stopped)) return false;

    if (!supervisor_start()) return check("GRACEFUL RESTART", false);
    bool replacement = supervisor_healthy() && supervisor_process_id() != old_pid &&
        supervisor_thread_id() != old_tid && baseline_matches(baseline);
    return check("GRACEFUL RESTART / NEW INCARNATION / BASELINE", replacement);
}

static bool forced_timeout_case(const SupervisorRecoveryBaseline *baseline) {
    u64 old_pid = supervisor_process_id();
    u64 old_tid = supervisor_thread_id();
    bool armed = supervisor_test_arm_shutdown_hang();
    if (!check("NONCOOPERATIVE SHUTDOWN ARMED", armed)) return false;

    u64 ticks_before = timer_ticks();
    u64 preempt_before = scheduler_preemption_count();
    SupervisorStopResult result = supervisor_stop_bounded();
    u64 tick_delta = timer_ticks() - ticks_before;
    u64 preempt_delta = scheduler_preemption_count() - preempt_before;

    ProcessExitInfo info;
    bool exit = supervisor_last_exit_info(&info);
    bool forced = result == SUPERVISOR_STOP_FORCED && supervisor_last_stop_result() == result &&
        supervisor_state() == SUPERVISOR_STATE_STOPPED && !supervisor_running() && exit &&
        info.reason == PROCESS_EXIT_TERMINATED && info.process_id == old_pid && info.thread_id == old_tid &&
        tick_delta > 0ULL && preempt_delta > 0ULL;
    if (!check("TIMEOUT -> FORCED TERMINATION / REAP", forced)) return false;

    if (!supervisor_start()) return check("FORCED RESTART", false);
    bool replacement = supervisor_healthy() && supervisor_process_id() != old_pid &&
        supervisor_thread_id() != old_tid && baseline_matches(baseline);
    return check("FORCED RESTART / NEW INCARNATION / BASELINE", replacement);
}

static bool fault_case(const SupervisorRecoveryBaseline *baseline) {
    u64 old_pid = supervisor_process_id();
    u64 old_tid = supervisor_thread_id();
    bool faulted = supervisor_test_fault();
    ProcessExitInfo info;
    bool exit = supervisor_last_exit_info(&info);
    bool failed = faulted && supervisor_state() == SUPERVISOR_STATE_FAILED && !supervisor_running() && exit &&
        info.reason == PROCESS_EXIT_FAULT && info.process_id == old_pid && info.thread_id == old_tid &&
        info.vector == 6ULL;
    if (!check("USER FAULT -> FAILED WITH RETAINED EXIT", failed)) return false;

    bool recovered = supervisor_recover() && supervisor_state() == SUPERVISOR_STATE_STOPPED &&
        scheduler_thread_count() == 1ULL;
    if (!check("FAILED INCARNATION FORCE-REAP", recovered)) return false;

    if (!supervisor_start()) return check("FAULT RESTART", false);
    bool replacement = supervisor_healthy() && supervisor_process_id() != old_pid &&
        supervisor_thread_id() != old_tid && baseline_matches(baseline);
    return check("FAULT RESTART / NEW INCARNATION / BASELINE", replacement);
}

void supervisor_recovery_test_run(void) {
    terminal_writeln("SUPERVISOR LIFECYCLE / RECOVERY TEST:");
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    bool preflight = kernel && main && main->process == kernel && main->state == THREAD_STATE_RUNNING &&
        main->on_run_queue && scheduler_thread_count() == 1ULL && scheduler_preemption_enabled() &&
        timer_initialized() && supervisor_healthy();
    check("RUNNING / IDLE BASELINE", preflight);
    if (!preflight) goto fail;

    SupervisorRecoveryBaseline baseline = baseline_take();
    bool pass = graceful_case(&baseline);
    if (pass) pass = forced_timeout_case(&baseline);
    if (pass) pass = fault_case(&baseline);
    if (pass) pass = supervisor_healthy() && baseline_matches(&baseline);
    check("FINAL HEALTH / RESOURCE BASELINE", pass);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("SUPERVISOR LIFECYCLE / RECOVERY TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
    return;

fail:
    {
        bool restored = restore_supervisor();
        check("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("SUPERVISOR LIFECYCLE / RECOVERY TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
