#include "system_console_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "managed_service.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "supervisor.h"
#include "system_console.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"

#define SYSTEM_CONSOLE_TEST_TIMEOUT_SECONDS 2ULL
#define SYSTEM_CONSOLE_TEST_SUPERVISOR_COOKIE 0x5237434F4E52554EULL

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 tables;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    u64 supervisor_pid;
    u64 supervisor_tid;
    u64 portal_endpoint_id;
    u64 portal_thread_id;
} SystemConsoleBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(SYSTEM_CONSOLE_TEST_SUPERVISOR_COOKIE, &reply) &&
        reply == SYSTEM_CONSOLE_TEST_SUPERVISOR_COOKIE;
}

static void capture(SystemConsoleBaseline *baseline) {
    if (!baseline) return;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    baseline->free_pages = pmm_stats().free_pages;
    baseline->processes = process_object_count();
    baseline->spaces = address_space_object_count();
    baseline->threads = thread_object_count();
    baseline->endpoints = endpoint_object_count();
    baseline->tables = capability_table_object_count();
    baseline->exit_queues = process_exit_queue_object_count();
    baseline->kernel_caps = caps ? capability_table_count(caps) : 0U;
    baseline->kernel_threads = kernel ? process_thread_count(kernel) : 0ULL;
    baseline->scheduler_threads = scheduler_thread_count();
    baseline->supervisor_pid = supervisor_process_id();
    baseline->supervisor_tid = supervisor_thread_id();
    baseline->portal_endpoint_id = system_console_portal_endpoint_id();
    baseline->portal_thread_id = system_console_portal_thread_id();
}

static bool baseline_restored(const SystemConsoleBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    Thread *main = thread_current();
    return kernel && caps && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        capability_table_object_count() == baseline->tables &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        supervisor_process_id() == baseline->supervisor_pid &&
        supervisor_thread_id() == baseline->supervisor_tid &&
        system_console_portal_endpoint_id() == baseline->portal_endpoint_id &&
        system_console_portal_thread_id() == baseline->portal_thread_id &&
        scheduler_preemption_enabled() && supervisor_healthy() &&
        system_console_running() && system_console_idle();
}

static bool wait_console_failed(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * SYSTEM_CONSOLE_TEST_TIMEOUT_SECONDS;
    u64 started = timer_ticks();
    for (;;) {
        ManagedServiceState state = system_console_state();
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool restore_console(void) {
    if (system_console_running() && system_console_idle()) return true;
    if (!system_console_present()) return system_console_start();
    return system_console_restart();
}

void system_console_test_cleanup_run(void) {
    terminal_writeln("R7 PERSISTENT CONSOLE CLEANUP / RECOVERY:");
    report("BOOT CONSOLE RESTORED", restore_console() && supervisor_healthy());
}

void system_console_test_run(void) {
    terminal_writeln("R7 PERSISTENT CONSOLE / RESTART BOUNDARY TEST:");

    bool preflight = system_console_running() && system_console_idle() && supervisor_healthy() &&
        timer_initialized() && scheduler_preemption_enabled();
    report("BOOTSTRAP CONSOLE / SUPERVISOR HEALTH", preflight);
    if (!preflight) goto fail;

    SystemConsoleBaseline baseline;
    capture(&baseline);

    ManagedServiceConnection old_connection;
    bool connection = system_console_connection_snapshot(&old_connection);
    u64 old_incarnation = system_console_incarnation();
    u64 old_pid = system_console_process_id();
    u64 old_tid = system_console_thread_id();
    u64 writes_before = system_console_portal_write_count();
    bool initial_output = connection && old_incarnation && old_pid && old_tid &&
        system_console_write("R7A2\n") &&
        system_console_portal_write_count() == writes_before + 5ULL;
    report("POST-BOOT OUTPUT THROUGH PERSISTENT USER SERVICE", initial_output);
    if (!initial_output) goto fail;

    bool fault_sent = system_console_test_fault();
    bool failed = fault_sent && wait_console_failed();
    ProcessExitInfo exit_info;
    bool exit_retained = failed && system_console_last_exit_info(&exit_info) &&
        exit_info.reason == PROCESS_EXIT_FAULT && exit_info.process_id == old_pid &&
        exit_info.thread_id == old_tid && exit_info.vector == 6U;
    report("CONSOLE FAULT -> RETAINED FAILED INCARNATION", exit_retained);
    if (!exit_retained) goto fail;

    bool unrelated = supervisor_healthy() && supervisor_process_id() == baseline.supervisor_pid &&
        supervisor_thread_id() == baseline.supervisor_tid;
    report("UNRELATED SUPERVISOR SURVIVES CONSOLE CRASH", unrelated);
    if (!unrelated) goto fail;

    bool restarted = system_console_restart();
    u64 new_incarnation = system_console_incarnation();
    u64 new_pid = system_console_process_id();
    u64 new_tid = system_console_thread_id();
    bool replacement = restarted && system_console_running() && system_console_idle() &&
        new_incarnation && new_incarnation != old_incarnation &&
        new_pid && new_pid != old_pid && new_tid && new_tid != old_tid &&
        !system_console_connection_current(&old_connection) &&
        system_console_portal_endpoint_id() == baseline.portal_endpoint_id &&
        system_console_portal_thread_id() == baseline.portal_thread_id;
    report("NEW INCARNATION / OLD CONNECTION STALE / PORTAL PRESERVED", replacement);
    if (!replacement) goto fail;

    u64 writes_mid = system_console_portal_write_count();
    bool replacement_output = system_console_write("R7R\n") &&
        system_console_portal_write_count() == writes_mid + 4ULL;
    report("REPLACEMENT SERVICE RESUMES OUTPUT", replacement_output);
    if (!replacement_output) goto fail;

    bool final = baseline_restored(&baseline);
    report("PERSISTENT OBJECT / AUTHORITY BASELINE", final);
    if (final) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R7 PERSISTENT CONSOLE / RESTART BOUNDARY TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

fail:
    {
        bool restored = restore_console();
        report("FAILURE RECOVERY / BOOT CONSOLE RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R7 PERSISTENT CONSOLE / RESTART BOUNDARY TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}