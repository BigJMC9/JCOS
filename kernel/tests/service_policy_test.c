#include "service_policy_test.h"

#include "address_space.h"
#include "background_service.h"
#include "capability.h"
#include "endpoint.h"
#include "key_event.h"
#include "lib.h"
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
#include "../../include/ordinary_service_protocol.h"
#include "../../include/service_broker_protocol.h"

#define SERVICE_POLICY_SUPERVISOR_COOKIE 0x523753455256504FULL
#define SERVICE_POLICY_WORKER_COOKIE     0x523753455256574BULL

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
} ServicePolicyBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(SERVICE_POLICY_SUPERVISOR_COOKIE, &reply) &&
        reply == SERVICE_POLICY_SUPERVISOR_COOKIE;
}

static void capture(ServicePolicyBaseline *baseline) {
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

static bool baseline_matches(const ServicePolicyBaseline *baseline) {
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
        !background_service_present() && system_console_running() && system_console_idle();
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
    while (text && *text) {
        KeyEvent event = key_event(KEY_CHARACTER, *text++);
        u64 action = ~0ULL;
        u64 result = ~0ULL;
        if (!system_console_input_event(&event, &action, &result) ||
            action != JCOS_CONSOLE_SHELL_ACTION_NONE || result != JCOS_CONSOLE_SHELL_RESULT_NONE) return false;
    }
    return text != 0;
}

static bool submit(u64 expected_action, u64 expected_result) {
    KeyEvent enter = key_event(KEY_ENTER, 0);
    u64 action = ~0ULL;
    u64 result = ~0ULL;
    return system_console_input_event(&enter, &action, &result) &&
        action == expected_action && result == expected_result;
}

static bool request_service(const char *command) {
    return send_text(command) &&
        submit(JCOS_CONSOLE_SHELL_ACTION_SERVICE_CONTROL, JCOS_CONSOLE_SHELL_RESULT_OK) &&
        !system_console_idle();
}

static bool handle_service(BackgroundServiceResult *out) {
    return out && background_service_handle_pending(out) &&
        system_console_service_result(out->result, out->state, out->incarnation) &&
        system_console_idle();
}

static bool cleanup(void) {
    bool ok = background_service_stop();
    if (system_console_running() && system_console_idle()) {
        (void)system_console_shell_deactivate();
    } else if (system_console_present() && !system_console_app_session_id()) {
        ok = system_console_restart() && ok;
    }
    return ok && !background_service_present() && system_console_running() && system_console_idle();
}

void service_policy_test_cleanup_run(void) {
    terminal_writeln("R7 USERSPACE SERVICE POLICY CLEANUP / RECOVERY:");
    report("BACKGROUND SLOT / CONSOLE BASELINE RESTORED", cleanup() && supervisor_healthy());
}

void service_policy_test_run(void) {
    terminal_writeln("R7 USERSPACE SERVICE NAMING / EXECUTABLE REPLACEMENT TEST:");
    if (!cleanup()) {
        report("NO RETAINED BACKGROUND SERVICE", false);
        return;
    }

    ServicePolicyBaseline baseline;
    capture(&baseline);
    u64 broker_before = system_console_service_endpoint_id();
    u64 console_incarnation_before = system_console_incarnation();

    bool preflight = timer_initialized() && scheduler_preemption_enabled() && broker_before &&
        system_console_idle() && !background_service_present() && supervisor_healthy();
    report("RING3 POLICY / GENERIC SERVICE BROKER BASELINE", preflight);
    if (!preflight) goto fail;

    bool activated = system_console_shell_activate() && system_console_idle();
    report("RING3 SHELL ACTIVATED", activated);
    if (!activated) goto fail;

    bool unknown = send_text("service start definitely-not-a-service") &&
        submit(JCOS_CONSOLE_SHELL_ACTION_NONE, JCOS_CONSOLE_SHELL_RESULT_UNKNOWN) &&
        system_console_idle() && !background_service_present();
    report("UNKNOWN SERVICE NAME REJECTED ENTIRELY IN RING3", unknown);
    if (!unknown) goto fail;

    BackgroundServiceResult result;
    bool start_requested = request_service("service start worker");
    bool started = start_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_RUNNING && result.incarnation &&
        background_service_running() && background_service_incarnation() == result.incarnation;
    u64 first_incarnation = result.incarnation;
    u64 first_pid = background_service_process_id();
    u64 first_tid = background_service_thread_id();
    u64 ping_reply = 0ULL;
    u64 image_id = 0ULL;
    bool first_ping = started && first_pid && first_tid &&
        background_service_ping(SERVICE_POLICY_WORKER_COOKIE, &ping_reply) &&
        ping_reply == SERVICE_POLICY_WORKER_COOKIE &&
        background_service_image_id(&image_id) &&
        image_id == JCOS_ORDINARY_SERVICE_IMAGE_PRIMARY;
    report("SERVICE NAME/PATH RESOLVED IN RING3 / PRIMARY ELF STARTED", started && first_ping);
    if (!started || !first_ping) goto fail;

    bool protocol_contract = background_service_protocol_contract() &&
        background_service_ping(SERVICE_POLICY_WORKER_COOKIE, &ping_reply) &&
        ping_reply == SERVICE_POLICY_WORKER_COOKIE;
    report("VERSION / UNKNOWN-OP ERRORS ARE EXPLICIT AND NONFATAL", protocol_contract);
    if (!protocol_contract) goto fail;

    bool status_requested = request_service("service status worker");
    bool status = status_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_RUNNING &&
        result.incarnation == first_incarnation && background_service_running();
    report("RING3 STATUS POLICY / LIVE SERVICE INCARNATION", status);
    if (!status) goto fail;

    bool fault_requested = request_service("service fault worker");
    ProcessExitInfo fault_exit;
    k_memset(&fault_exit, 0, sizeof(fault_exit));
    bool faulted = fault_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_FAILED &&
        result.incarnation == first_incarnation && background_service_present() &&
        !background_service_running() &&
        background_service_last_exit_info(&fault_exit) &&
        fault_exit.reason == PROCESS_EXIT_FAULT && fault_exit.vector == 6ULL &&
        fault_exit.process_id == first_pid && fault_exit.thread_id == first_tid &&
        supervisor_healthy();
    report("SERVICE FAULT / VECTOR 6 CONTAINED / POLICY REMAINS USABLE", faulted);
    if (!faulted) goto fail;

    bool replace_requested = request_service("service replace worker");
    bool replaced = replace_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_RUNNING && result.incarnation &&
        result.incarnation != first_incarnation && background_service_running();
    u64 replacement_incarnation = result.incarnation;
    ping_reply = 0ULL;
    image_id = 0ULL;
    bool replacement_identity = replaced &&
        background_service_ping(SERVICE_POLICY_WORKER_COOKIE + 1ULL, &ping_reply) &&
        ping_reply == SERVICE_POLICY_WORKER_COOKIE + 1ULL &&
        background_service_image_id(&image_id) &&
        image_id == JCOS_ORDINARY_SERVICE_IMAGE_REPLACEMENT && supervisor_healthy();
    report("RING3 REPLACEMENT POLICY / DISTINCT ELF / FRESH INCARNATION",
        replaced && replacement_identity);
    if (!replaced || !replacement_identity) goto fail;

    bool restart_requested = request_service("service restart worker");
    bool restarted = restart_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_RUNNING && result.incarnation &&
        result.incarnation != replacement_incarnation && background_service_running();
    u64 second_incarnation = result.incarnation;
    image_id = 0ULL;
    bool replacement_retained = restarted &&
        background_service_image_id(&image_id) &&
        image_id == JCOS_ORDINARY_SERVICE_IMAGE_REPLACEMENT;
    report("NORMAL RESTART RETAINS RING3-SELECTED REPLACEMENT", replacement_retained);
    if (!replacement_retained) goto fail;

    bool stop_requested = request_service("service stop worker");
    bool stopped = stop_requested && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK &&
        result.state == JCOS_SERVICE_BROKER_STATE_STOPPED && !result.incarnation &&
        !background_service_present();
    report("RING3 STOP POLICY / FULL REAP", stopped);
    if (!stopped) goto fail;

    bool deactivated = system_console_shell_deactivate() && system_console_idle();
    report("FIRST POLICY CYCLE / EXACT RESOURCE BASELINE", deactivated && baseline_matches(&baseline));
    if (!deactivated || !baseline_matches(&baseline)) goto fail;

    bool policy_restarted = system_console_restart() && system_console_idle() &&
        system_console_incarnation() != console_incarnation_before &&
        system_console_service_endpoint_id() && system_console_service_endpoint_id() != broker_before &&
        system_console_portal_endpoint_id() == baseline.output_portal_endpoint &&
        system_console_portal_thread_id() == baseline.output_portal_thread &&
        system_console_archive_request_endpoint_id() == baseline.archive_request_endpoint &&
        system_console_archive_reply_endpoint_id() == baseline.archive_reply_endpoint &&
        system_console_archive_thread_id() == baseline.archive_thread;
    report("POLICY SERVICE / SERVICE-BROKER AUTHORITY REINCARNATED", policy_restarted);
    if (!policy_restarted) goto fail;

    u64 restored_image_id = 0ULL;
    bool second_cycle = system_console_shell_activate() && system_console_idle() &&
        request_service("service start worker") && handle_service(&result) &&
        result.result == JCOS_SERVICE_BROKER_RESULT_OK && result.state == JCOS_SERVICE_BROKER_STATE_RUNNING &&
        result.incarnation && result.incarnation != second_incarnation &&
        background_service_image_id(&restored_image_id) &&
        restored_image_id == JCOS_ORDINARY_SERVICE_IMAGE_PRIMARY &&
        request_service("service stop worker") && handle_service(&result) &&
        result.state == JCOS_SERVICE_BROKER_STATE_STOPPED && !background_service_present() &&
        system_console_shell_deactivate() && system_console_idle();
    report("REPLACEMENT POLICY RESOLVES / MANAGES SERVICE AGAIN", second_cycle);
    if (!second_cycle) goto fail;

    bool final = baseline_matches(&baseline);
    report("FINAL RESOURCE / PRIVILEGED MECHANISM BASELINE", final);
    if (!final) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("R7 USERSPACE SERVICE NAMING / EXECUTABLE REPLACEMENT TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup());
    terminal_set_color(terminal_error_color());
    terminal_writeln("R7 USERSPACE SERVICE NAMING / EXECUTABLE REPLACEMENT TEST: FAILED");
    terminal_set_color(terminal_default_color());
}