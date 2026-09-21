#include "boot_archive_policy_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "key_event.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "supervisor.h"
#include "system_console.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "../../include/console_service_protocol.h"

#define BOOT_ARCHIVE_TEST_SUPERVISOR_COOKIE 0x523746494C45534DULL

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
    u64 archive_size;
} BootArchiveBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(BOOT_ARCHIVE_TEST_SUPERVISOR_COOKIE, &reply) &&
        reply == BOOT_ARCHIVE_TEST_SUPERVISOR_COOKIE;
}

static void capture(BootArchiveBaseline *baseline) {
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
    baseline->archive_size = system_console_archive_size();
}

static bool counts_restored(const BootArchiveBaseline *baseline) {
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
        system_console_archive_size() == baseline->archive_size &&
        scheduler_preemption_enabled() && supervisor_healthy() &&
        system_console_running() && system_console_idle();
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

static bool send_character(char c) {
    KeyEvent event = key_event(KEY_CHARACTER, c);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&event, &action, &result) &&
        action == JCOS_CONSOLE_SHELL_ACTION_NONE &&
        result == JCOS_CONSOLE_SHELL_RESULT_NONE;
}

static bool send_text(const char *text) {
    if (!text) return false;
    while (*text) {
        if (!send_character(*text++)) return false;
    }
    return true;
}

static bool submit_ok(void) {
    KeyEvent enter = key_event(KEY_ENTER, 0);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&enter, &action, &result) &&
        action == JCOS_CONSOLE_SHELL_ACTION_NONE &&
        result == JCOS_CONSOLE_SHELL_RESULT_OK && system_console_idle();
}

static bool run_command(const char *command) {
    return send_text(command) && submit_ok();
}

static bool recover_console(void) {
    if (system_console_running() && system_console_idle()) {
        (void)system_console_shell_deactivate();
        return system_console_running() && system_console_idle();
    }
    return system_console_present() && system_console_restart();
}

void boot_archive_policy_test_cleanup_run(void) {
    terminal_writeln("R7 USERSPACE FILE POLICY CLEANUP / RECOVERY:");
    report("PERSISTENT CONSOLE / RAW ARCHIVE PORTAL RESTORED",
        recover_console() && supervisor_healthy());
}

void boot_archive_policy_test_run(void) {
    terminal_writeln("R7 USERSPACE FILE NAMESPACE / RAW BOOT ARCHIVE TEST:");

    bool preflight = timer_initialized() && scheduler_preemption_enabled() &&
        system_console_running() && system_console_idle() && supervisor_healthy() &&
        system_console_archive_size() > 1024ULL &&
        system_console_archive_request_endpoint_id() &&
        system_console_archive_reply_endpoint_id() && system_console_archive_thread_id();
    report("RAW ARCHIVE MECHANISM / SERVICE BASELINE", preflight);
    if (!preflight) goto fail;

    BootArchiveBaseline baseline;
    capture(&baseline);
    u64 old_console_incarnation = system_console_incarnation();
    u64 old_console_pid = system_console_process_id();
    u64 old_console_tid = system_console_thread_id();
    u64 reads_before = system_console_archive_read_count();

    bool activated = system_console_shell_activate() && system_console_idle();
    report("RING3 SHELL ACTIVATED", activated);
    if (!activated) goto fail;

    bool fscheck = run_command("fscheck") &&
        system_console_archive_read_count() > reads_before;
    report("RING3 TAR PARSER / KNOWN FILE CONTENT", fscheck);
    if (!fscheck) goto fail_shell;

    u64 reads_after_check = system_console_archive_read_count();
    bool stat_ok = run_command("stat /etc/r7c1.txt") &&
        system_console_archive_read_count() > reads_after_check;
    report("USERSPACE PATH LOOKUP / STAT", stat_ok);
    if (!stat_ok) goto fail_shell;

    u64 reads_after_stat = system_console_archive_read_count();
    bool cat_ok = run_command("cat /etc/r7c1.txt") &&
        system_console_archive_read_count() > reads_after_stat;
    report("USERSPACE FILE READ / CAT", cat_ok);
    if (!cat_ok) goto fail_shell;

    bool deactivated = system_console_shell_deactivate() && system_console_idle();
    report("SHELL SESSION DEACTIVATED", deactivated);
    if (!deactivated) goto fail;

    u64 reads_before_restart = system_console_archive_read_count();
    bool restarted = system_console_restart();
    bool replacement = restarted && system_console_running() && system_console_idle() &&
        system_console_incarnation() && system_console_incarnation() != old_console_incarnation &&
        system_console_process_id() && system_console_process_id() != old_console_pid &&
        system_console_thread_id() && system_console_thread_id() != old_console_tid &&
        system_console_portal_endpoint_id() == baseline.output_portal_endpoint &&
        system_console_portal_thread_id() == baseline.output_portal_thread &&
        system_console_archive_request_endpoint_id() == baseline.archive_request_endpoint &&
        system_console_archive_reply_endpoint_id() == baseline.archive_reply_endpoint &&
        system_console_archive_thread_id() == baseline.archive_thread &&
        system_console_archive_size() == baseline.archive_size &&
        system_console_archive_read_count() >= reads_before_restart && supervisor_healthy();
    report("POLICY SERVICE REPLACED / RAW ARCHIVE PORTAL PRESERVED", replacement);
    if (!replacement) goto fail;

    u64 reads_second = system_console_archive_read_count();
    bool second = system_console_shell_activate() && system_console_idle() &&
        run_command("fscheck") && system_console_archive_read_count() > reads_second &&
        system_console_shell_deactivate() && system_console_idle();
    report("REPLACEMENT SERVICE REBUILDS USERSPACE NAMESPACE", second);
    if (!second) goto fail_shell;

    bool final = counts_restored(&baseline);
    report("EXACT RESOURCE / PORTAL IDENTITY BASELINE", final);
    if (final) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R7 USERSPACE FILE NAMESPACE / RAW BOOT ARCHIVE TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }
    goto fail;

fail_shell:
    (void)system_console_shell_deactivate();
fail:
    {
        bool recovered = recover_console();
        report("FAILURE RECOVERY / CONSOLE IDLE", recovered && supervisor_healthy());
        terminal_set_color(terminal_error_color());
        terminal_writeln("R7 USERSPACE FILE NAMESPACE / RAW BOOT ARCHIVE TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}