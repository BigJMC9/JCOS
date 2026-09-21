#include "foreground_launch_policy_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "foreground_program.h"
#include "key_event.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "supervisor.h"
#include "system_console.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "../../include/console_service_protocol.h"

#define FOREGROUND_LAUNCH_SUPERVISOR_COOKIE 0x52374C41554E4348ULL

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
    u64 output_portal_endpoint;
    u64 output_portal_thread;
    u64 archive_request_endpoint;
    u64 archive_reply_endpoint;
    u64 archive_thread;
} LaunchPolicyBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(FOREGROUND_LAUNCH_SUPERVISOR_COOKIE, &reply) &&
        reply == FOREGROUND_LAUNCH_SUPERVISOR_COOKIE;
}

static void capture(LaunchPolicyBaseline *baseline) {
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
    baseline->output_portal_endpoint = system_console_portal_endpoint_id();
    baseline->output_portal_thread = system_console_portal_thread_id();
    baseline->archive_request_endpoint = system_console_archive_request_endpoint_id();
    baseline->archive_reply_endpoint = system_console_archive_reply_endpoint_id();
    baseline->archive_thread = system_console_archive_thread_id();
}

static bool baseline_matches(const LaunchPolicyBaseline *baseline) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    Thread *main = thread_current();
    return baseline && kernel && caps && main && main->process == kernel &&
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
        system_console_portal_endpoint_id() == baseline->output_portal_endpoint &&
        system_console_portal_thread_id() == baseline->output_portal_thread &&
        system_console_archive_request_endpoint_id() == baseline->archive_request_endpoint &&
        system_console_archive_reply_endpoint_id() == baseline->archive_reply_endpoint &&
        system_console_archive_thread_id() == baseline->archive_thread &&
        scheduler_preemption_enabled() && supervisor_healthy() &&
        foreground_program_idle() && system_console_running() && system_console_idle();
}

static KeyEvent key_event(KeyCode key, char character) {
    KeyEvent event;
    event.key = key;
    event.character = character;
    event.pressed = true;
    event.shift = false;
    event.ctrl = false;
    event.alt = false;
    return event;
}

static bool send_text(const char *text) {
    if (!text) return false;
    while (*text) {
        KeyEvent event = key_event(KEY_CHARACTER, *text++);
        u64 action = ~0ULL;
        u64 result = ~0ULL;
        if (!system_console_input_event(&event, &action, &result) ||
            action != JCOS_CONSOLE_SHELL_ACTION_NONE ||
            result != JCOS_CONSOLE_SHELL_RESULT_NONE) return false;
    }
    return true;
}

static bool submit(u64 expected_action, u64 expected_result) {
    KeyEvent enter = key_event(KEY_ENTER, 0);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&enter, &action, &result) &&
        action == expected_action && result == expected_result;
}

static bool request_run(const char *path) {
    return send_text("run ") && send_text(path) &&
        submit(JCOS_CONSOLE_SHELL_ACTION_RUN_FOREGROUND, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        !system_console_idle();
}

static bool request_missing(void) {
    return send_text("run /bin/definitely-missing.elf") &&
        submit(JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_UNKNOWN) &&
        system_console_idle();
}

static bool cleanup(void) {
    bool ok = foreground_program_cleanup();
    if (system_console_running() && system_console_idle()) {
        (void)system_console_shell_deactivate();
    } else if (system_console_present() && !system_console_app_session_id()) {
        ok = system_console_restart() && ok;
    }
    return ok && foreground_program_idle() && system_console_running() && system_console_idle();
}

void foreground_launch_policy_test_cleanup_run(void) {
    terminal_writeln("R7 USERSPACE LAUNCH POLICY CLEANUP / RECOVERY:");
    report("FOREGROUND PROGRAM / CONSOLE BASELINE RESTORED", cleanup() && supervisor_healthy());
}

void foreground_launch_policy_test_run(void) {
    terminal_writeln("R7 USERSPACE PATH RESOLUTION / FOREGROUND LAUNCH TEST:");
    if (!cleanup()) {
        report("NO RETAINED FOREGROUND STATE", false);
        return;
    }

    LaunchPolicyBaseline baseline;
    capture(&baseline);
    u64 launch_endpoint_before = system_console_launch_endpoint_id();
    u64 app_endpoint_before = system_console_app_endpoint_id();
    u64 app_input_before = system_console_app_input_endpoint_id();
    u64 console_incarnation_before = system_console_incarnation();
    u64 launches_before = program_launch_count();
    u64 runs_before = foreground_program_run_count();

    bool preflight = timer_initialized() && scheduler_preemption_enabled() &&
        launch_endpoint_before && app_endpoint_before && app_input_before &&
        system_console_idle() && foreground_program_idle() && supervisor_healthy();
    report("RING3 FILE / LAUNCH MECHANISM BASELINE", preflight);
    if (!preflight) goto fail;

    bool activated = system_console_shell_activate() && system_console_idle();
    report("RING3 SHELL ACTIVATED", activated);
    if (!activated) goto fail;

    bool selected = request_run("/bin/hello.elf");
    report("PATH RESOLVED IN RING3 / OPAQUE EXTENT REQUESTED", selected);
    if (!selected) goto fail;

    bool first = foreground_program_run_pending() && foreground_program_idle() &&
        system_console_idle() && foreground_program_run_count() == runs_before + 1ULL &&
        program_launch_count() == launches_before + 1ULL;
    ProcessExitInfo first_exit;
    bool first_exit_ok = first && foreground_program_last_exit(&first_exit) &&
        first_exit.reason == PROCESS_EXIT_NORMAL;
    report("GENERIC EXTENT LAUNCH / FOREGROUND APP EXIT", first && first_exit_ok);
    if (!first || !first_exit_ok) goto fail;

    bool shell_returned = system_console_shell_activate() && system_console_idle() && request_missing() &&
        foreground_program_run_count() == runs_before + 1ULL &&
        program_launch_count() == launches_before + 1ULL;
    report("MISSING PATH REJECTED BY RING3 WITHOUT LAUNCH", shell_returned);
    if (!shell_returned) goto fail;

    bool deactivated = system_console_shell_deactivate() && system_console_idle();
    report("SHELL DEACTIVATED / EXACT FIRST BASELINE", deactivated && baseline_matches(&baseline));
    if (!deactivated || !baseline_matches(&baseline)) goto fail;

    bool restarted = system_console_restart() && system_console_idle() &&
        system_console_incarnation() != console_incarnation_before &&
        system_console_launch_endpoint_id() && system_console_launch_endpoint_id() != launch_endpoint_before &&
        system_console_app_endpoint_id() && system_console_app_endpoint_id() != app_endpoint_before &&
        system_console_app_input_endpoint_id() && system_console_app_input_endpoint_id() != app_input_before &&
        system_console_portal_endpoint_id() == baseline.output_portal_endpoint &&
        system_console_portal_thread_id() == baseline.output_portal_thread &&
        system_console_archive_request_endpoint_id() == baseline.archive_request_endpoint &&
        system_console_archive_reply_endpoint_id() == baseline.archive_reply_endpoint &&
        system_console_archive_thread_id() == baseline.archive_thread && supervisor_healthy();
    report("POLICY SERVICE / LAUNCH CHANNEL REINCARNATED", restarted);
    if (!restarted) goto fail;

    bool second = system_console_shell_activate() && system_console_idle() &&
        request_run("/bin/hello.elf") && foreground_program_run_pending() &&
        foreground_program_run_count() == runs_before + 2ULL &&
        program_launch_count() == launches_before + 3ULL &&
        system_console_shell_activate() && system_console_idle() &&
        system_console_shell_deactivate() && system_console_idle();
    report("REPLACEMENT POLICY RESOLVES / LAUNCHES AGAIN", second);
    if (!second) goto fail;

    bool final = baseline_matches(&baseline);
    report("FINAL RESOURCE / PRIVILEGED PORTAL BASELINE", final);
    if (!final) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("R7 USERSPACE PATH RESOLUTION / FOREGROUND LAUNCH TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup());
    terminal_set_color(terminal_error_color());
    terminal_writeln("R7 USERSPACE PATH RESOLUTION / FOREGROUND LAUNCH TEST: FAILED");
    terminal_set_color(terminal_default_color());
}