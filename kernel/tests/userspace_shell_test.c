#include "userspace_shell_test.h"

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
#include "userspace_shell.h"
#include "../../include/console_service_protocol.h"

#define USERSPACE_SHELL_SUPERVISOR_COOKIE 0x5237555348454C4CULL

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
    u64 console_pid;
    u64 console_tid;
    u64 console_incarnation;
    u64 portal_endpoint_id;
    u64 portal_thread_id;
} UserspaceShellBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(USERSPACE_SHELL_SUPERVISOR_COOKIE, &reply) &&
        reply == USERSPACE_SHELL_SUPERVISOR_COOKIE;
}

static void capture(UserspaceShellBaseline *baseline) {
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
    baseline->console_pid = system_console_process_id();
    baseline->console_tid = system_console_thread_id();
    baseline->console_incarnation = system_console_incarnation();
    baseline->portal_endpoint_id = system_console_portal_endpoint_id();
    baseline->portal_thread_id = system_console_portal_thread_id();
}

static bool baseline_restored(const UserspaceShellBaseline *baseline) {
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
        system_console_process_id() == baseline->console_pid &&
        system_console_thread_id() == baseline->console_tid &&
        system_console_incarnation() == baseline->console_incarnation &&
        system_console_portal_endpoint_id() == baseline->portal_endpoint_id &&
        system_console_portal_thread_id() == baseline->portal_thread_id &&
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

static bool send_key(KeyCode key, char character, bool shift, bool ctrl, bool alt,
    u64 expected_action, u64 expected_result) {
    KeyEvent event = key_event(key, character);
    event.shift = shift;
    event.ctrl = ctrl;
    event.alt = alt;
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&event, &action, &result) &&
        action == expected_action && result == expected_result;
}

static bool submit_line(u64 expected_action, u64 expected_result) {
    KeyEvent enter = key_event(KEY_ENTER, 0);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&enter, &action, &result) &&
        action == expected_action && result == expected_result;
}

static bool deactivate_shell(void) {
    return system_console_running() && system_console_idle() && system_console_shell_deactivate();
}

void userspace_shell_test_cleanup_run(void) {
    terminal_writeln("R7 USERSPACE SHELL CLEANUP:");
    bool console = system_console_running() && system_console_idle();
    bool deactivated = console && system_console_shell_deactivate();
    report("SHELL SESSION DEACTIVATED", deactivated);
}

void userspace_shell_test_run(void) {
    terminal_writeln("R7 USERSPACE SHELL / INPUT HANDOFF TEST:");

    bool preflight = timer_initialized() && scheduler_preemption_enabled() &&
        system_console_running() && system_console_idle() && supervisor_healthy();
    report("PERSISTENT CONSOLE / SUPERVISOR HEALTH", preflight);
    if (!preflight) return;

    u64 boot_handoffs = userspace_shell_boot_handoff_count();
    u64 sessions = userspace_shell_session_count();
    bool boot_default = boot_handoffs > 0ULL && sessions >= boot_handoffs;
    report("BOOT DEFAULT RING3 SHELL HANDOFF OBSERVED", boot_default);
    if (!boot_default) return;

    UserspaceShellBaseline baseline;
    capture(&baseline);

    u64 writes_before = system_console_portal_write_count();
    bool activated = system_console_shell_activate() && system_console_idle() &&
        system_console_portal_write_count() > writes_before;
    report("RING3 SHELL ACTIVATION / PROMPT", activated);
    if (!activated) goto fail;

    u64 echo_before = system_console_portal_write_count();
    bool echo = send_text("echo R7A3") &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_portal_write_count() > echo_before && system_console_idle();
    report("RING3 COMMAND PARSER / ECHO", echo);
    if (!echo) goto fail;

    bool history_seed = send_text("echo HISTORY") &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_OK) && system_console_idle();
    report("RING3 HISTORY SEED", history_seed);
    if (!history_seed) goto fail;

    bool scrollback_available = terminal_scrollback_can_page_up();
    bool page_up = send_key(KEY_PAGE_UP, 0, false, false, false,
        JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        system_console_idle() &&
        (scrollback_available ? terminal_scrollback_active() : !terminal_scrollback_active());
    bool page_down = page_up && send_key(KEY_PAGE_DOWN, 0, false, false, false,
        JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        system_console_idle() && !terminal_scrollback_active();
    report("RING3 PAGE-UP / PAGE-DOWN SCROLLBACK", page_up && page_down);
    if (!page_up || !page_down) goto fail;

    bool line_up = send_key(KEY_PAGE_UP, 0, false, true, false,
        JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        system_console_idle() &&
        (scrollback_available ? terminal_scrollback_active() : !terminal_scrollback_active());
    bool line_down = line_up && send_key(KEY_PAGE_DOWN, 0, false, true, false,
        JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        system_console_idle() && !terminal_scrollback_active();
    report("RING3 CTRL+PAGE-UP / PAGE-DOWN LINE SCROLLBACK", line_up && line_down);
    if (!line_up || !line_down) goto fail;

    bool history = send_key(KEY_UP, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_DOWN, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_text("monitor") &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_idle();
    report("RING3 UP/DOWN HISTORY / DRAFT RESTORE", history);
    if (!history) goto fail;

    bool reactivate = system_console_shell_activate() && system_console_idle();
    report("SHELL SESSION REACTIVATION", reactivate);
    if (!reactivate) goto fail;

    bool left_backspace = send_text("monitxr") &&
        send_key(KEY_LEFT, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_BACKSPACE, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_character('o') &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_idle();
    report("RING3 LEFT / MID-LINE BACKSPACE EDIT", left_backspace);
    if (!left_backspace) goto fail;

    reactivate = system_console_shell_activate() && system_console_idle();
    if (!reactivate) goto fail;
    bool home_right_delete_end = send_text("mmonitor") &&
        send_key(KEY_HOME, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_RIGHT, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_DELETE, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_END, 0, false, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_idle();
    report("RING3 HOME/RIGHT/DELETE/END EDIT", home_right_delete_end);
    if (!home_right_delete_end) goto fail;

    reactivate = system_console_shell_activate() && system_console_idle();
    if (!reactivate) goto fail;
    bool clipboard = send_text("monitor") &&
        send_key(KEY_HOME, 0, true, false, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_CHARACTER, 'C', true, true, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_CHARACTER, 'X', true, true, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        send_key(KEY_CHARACTER, 'V', true, true, false,
            JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_NONE) &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_idle();
    report("RING3 SHIFT-SELECTION / CTRL+SHIFT C-X-V", clipboard);
    if (!clipboard) goto fail;

    reactivate = system_console_shell_activate() && system_console_idle();
    if (!reactivate) goto fail;
    bool unknown_service_reset = send_text("service start nope") &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_UNKNOWN) &&
        system_console_idle() && send_text("monitor") &&
        submit_line(JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        system_console_idle();
    report("SERVICE POLICY LOCAL ERROR CLEARS SUBMITTED LINE", unknown_service_reset);
    if (!unknown_service_reset) goto fail;

    reactivate = system_console_shell_activate() && system_console_idle();
    if (!reactivate) goto fail;

    KeyEvent escape = key_event(KEY_ESCAPE, 0);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    bool escape_action = system_console_input_event(&escape, &action, &result) &&
        action == JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR &&
        result == JCOS_CONSOLE_SHELL_RESULT_OK && system_console_idle();
    report("ESCAPE EVENT HAS EMERGENCY-RETURN SEMANTICS", escape_action);
    if (!escape_action) goto fail;

    bool unrelated = supervisor_healthy() && supervisor_process_id() == baseline.supervisor_pid &&
        supervisor_thread_id() == baseline.supervisor_tid;
    report("UNRELATED SUPERVISOR REMAINS USABLE", unrelated);
    if (!unrelated) goto fail;

    bool final = baseline_restored(&baseline);
    report("PERSISTENT RESOURCE / IDENTITY BASELINE", final);
    if (final) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R7 USERSPACE SHELL / INPUT HANDOFF TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

fail:
    {
        bool recovered = system_console_running() && system_console_idle();
        if (!recovered && system_console_present()) recovered = system_console_restart();
        if (recovered) (void)deactivate_shell();
        report("FAILURE RECOVERY / CONSOLE IDLE", recovered && system_console_running() && system_console_idle());
        terminal_set_color(terminal_error_color());
        terminal_writeln("R7 USERSPACE SHELL / INPUT HANDOFF TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}