#include "console_client_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "lib.h"
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
#include "vfs.h"
#include "../../include/console_client_protocol.h"
#include "../../include/hello_app_abi.h"

#define CONSOLE_CLIENT_TIMEOUT_SECONDS 2ULL

static ProgramInstance g_app;
static ProcessExitQueue g_exit_queue;
static SystemConsoleAppClientGrant g_client_grant;
static bool g_exit_queue_created;
static u64 g_session_id;

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
    u64 portal_endpoint;
    u64 portal_thread;
    u64 app_endpoint;
} ConsoleClientBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * CONSOLE_CLIENT_TIMEOUT_SECONDS : 0ULL;
}

static bool supervisor_ok(void) {
    u64 cookie = 0x52374231434C4945ULL;
    u64 reply = 0ULL;
    return supervisor_ping(cookie, &reply) && reply == cookie;
}

static void capture_baseline(ConsoleClientBaseline *baseline) {
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
    baseline->console_pid = system_console_process_id();
    baseline->console_tid = system_console_thread_id();
    baseline->console_incarnation = system_console_incarnation();
    baseline->portal_endpoint = system_console_portal_endpoint_id();
    baseline->portal_thread = system_console_portal_thread_id();
    baseline->app_endpoint = system_console_app_endpoint_id();
}

static bool baseline_matches(const ConsoleClientBaseline *baseline, bool exact_console_identity) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps || !system_console_idle() || !supervisor_ok()) return false;

    bool objects = pmm_stats().free_pages == baseline->free_pages &&
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
        system_console_portal_endpoint_id() == baseline->portal_endpoint &&
        system_console_portal_thread_id() == baseline->portal_thread;

    if (!objects) return false;
    if (!exact_console_identity) return true;
    return system_console_process_id() == baseline->console_pid &&
        system_console_thread_id() == baseline->console_tid &&
        system_console_incarnation() == baseline->console_incarnation &&
        system_console_app_endpoint_id() == baseline->app_endpoint;
}

static bool wait_app_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (g_app.thread.state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;

        if (g_app.thread.state == THREAD_STATE_READY) {
            if (!g_app.thread.on_run_queue || !g_app.thread.interrupt_context_ready ||
                !g_app.thread.interrupt_rsp || thread_wait_active(&g_app.thread)) return false;
        } else if (g_app.thread.state == THREAD_STATE_BLOCKED) {
            /* A console client can legitimately block while a previous one-slot
             * endpoint message is being consumed. Blocking SEND is therefore
             * expected back-pressure, not a failed application. */
            if (g_app.thread.on_run_queue || !g_app.thread.interrupt_context_ready ||
                !g_app.thread.interrupt_rsp || !thread_wait_active(&g_app.thread)) return false;
        } else {
            return false;
        }

        if (!scheduler_yield()) return false;
    }
    return !g_app.thread.on_run_queue && !g_app.thread.interrupt_context_ready && !g_app.thread.interrupt_rsp &&
        !thread_wait_active(&g_app.thread);
}

static bool drain_exit(ProcessExitInfo *out) {
    if (!out || !g_exit_queue_created || process_exit_queue_pending_count(&g_exit_queue) != 1U) return false;
    k_memset(out, 0, sizeof(*out));
    return process_exit_queue_try_receive(&g_exit_queue, out) &&
        process_exit_queue_pending_count(&g_exit_queue) == 0U &&
        process_exit_queue_watch_count(&g_exit_queue) == 0U;
}

static bool close_exit_queue(void) {
    if (!g_exit_queue_created) return true;
    if (process_exit_queue_pending_count(&g_exit_queue)) {
        ProcessExitInfo discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!process_exit_queue_try_receive(&g_exit_queue, &discard)) return false;
    }
    if (process_exit_queue_watch_count(&g_exit_queue)) return false;
    if (!g_exit_queue.closed && !process_exit_queue_close(&g_exit_queue)) return false;
    if (!process_exit_queue_destroy(&g_exit_queue)) return false;
    g_exit_queue_created = false;
    return true;
}

static bool cleanup_app(void) {
    bool ok = true;
    if (g_client_grant.active && !system_console_app_client_grant_end(&g_client_grant)) ok = false;
    if (program_instance_needs_cleanup(&g_app) && !program_terminate(&g_app)) ok = false;
    if (!close_exit_queue()) ok = false;
    if (g_session_id) {
        u64 session = g_session_id;
        g_session_id = 0ULL;
        if (system_console_app_session_id() == session &&
            !system_console_app_session_abort(session)) ok = false;
    }
    return ok;
}

void console_client_test_cleanup_run(void) {
    terminal_writeln("R7 APPLICATION CONSOLE CLEANUP RETRY:");
    report("APPLICATION / SESSION RELEASED", cleanup_app());
}

static bool child_authority_exact(void) {
    if (!g_app.process_created || !g_app.published ||
        g_app.startup_handles[0] == CAPABILITY_INVALID_HANDLE) return false;
    CapabilityTable *caps = process_capabilities(&g_app.process);
    if (!caps || capability_table_count(caps) != 1U) return false;
    void *object = 0;
    bool send = capability_lookup_rights(caps, g_app.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &object);
    if (!send || !object) return false;
    object = 0;
    return !capability_lookup_rights(caps, g_app.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_RECEIVE, &object);
}

static bool launch_hello(u64 *out_source_authority, u64 *out_portal_delta) {
    if (!out_source_authority || !out_portal_delta || !system_console_idle()) return false;
    *out_source_authority = CAPABILITY_INVALID_HANDLE;
    *out_portal_delta = 0ULL;

    VfsNode *file = vfs_resolve(vfs_root(), JCOS_HELLO_APP_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) return false;
    if (!system_console_app_session_begin(&g_session_id) || !g_session_id) return false;
    if (!system_console_app_client_grant_begin(&g_client_grant) ||
        g_client_grant.session_id != g_session_id) return false;

    *out_source_authority = g_client_grant.authority_handle;
    if (!process_exit_queue_create(&g_exit_queue)) return false;
    g_exit_queue_created = true;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_exit_queue;
    spec.startup_grant_count = 1U;
    k_memcpy(&spec.startup_grants[0], &g_client_grant.grant, sizeof(spec.startup_grants[0]));
    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION;
    spec.startup_arguments[1] = g_session_id;

    u64 portal_before = system_console_portal_write_count();
    if (!program_launch(&g_app, &spec) || !child_authority_exact()) return false;
    if (!system_console_app_client_grant_end(&g_client_grant)) return false;

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    void *stale = 0;
    if (!kernel_caps || capability_lookup(kernel_caps, *out_source_authority,
            CAPABILITY_TYPE_ENDPOINT, &stale)) return false;

    /* END is accepted into the one-slot application endpoint before the
     * sender necessarily exits. First wait for the console policy and byte
     * portal to settle, then require the client process to terminate. This
     * prevents test output from racing the final application payload. */
    if (!system_console_app_session_wait_end(g_session_id) || !wait_app_dead()) return false;
    g_session_id = 0ULL;

    ProcessExitInfo exit_info;
    if (!drain_exit(&exit_info) || exit_info.reason != PROCESS_EXIT_NORMAL ||
        exit_info.process_id != g_app.process.id || exit_info.thread_id != g_app.thread.id) return false;

    if (!program_reap(&g_app) || program_instance_needs_cleanup(&g_app) || !close_exit_queue()) return false;

    u64 expected = (u64)k_strlen(JCOS_HELLO_APP_TEXT) + 1ULL;
    u64 portal_after = system_console_portal_write_count();
    if (portal_after < portal_before || portal_after - portal_before != expected || !system_console_idle()) return false;
    *out_portal_delta = portal_after - portal_before;
    return true;
}

void console_client_test_run(void) {
    terminal_writeln("R7 APPLICATION CONSOLE / STANDALONE UTILITY TEST:");
    if (!cleanup_app()) {
        report("NO RETAINED APPLICATION STATE", false);
        return;
    }

    ConsoleClientBaseline baseline;
    capture_baseline(&baseline);
    bool preflight = system_console_idle() && baseline.console_pid && baseline.console_tid &&
        baseline.console_incarnation && baseline.portal_endpoint && baseline.portal_thread &&
        baseline.app_endpoint && supervisor_ok();
    report("PERSISTENT CONSOLE / SUPERVISOR BASELINE", preflight);
    if (!preflight) return;

    u64 launches_before = program_launch_count();
    u64 first_authority = 0ULL;
    u64 first_delta = 0ULL;
    bool first = launch_hello(&first_authority, &first_delta);
    report("INDEPENDENT RING3 APP / SEND-ONLY OUTPUT", first);
    if (!first) goto done;

    bool first_baseline = baseline_matches(&baseline, true) && first_delta == (u64)k_strlen(JCOS_HELLO_APP_TEXT) + 1ULL;
    report("FIRST APP EXIT / EXACT BASELINE", first_baseline);
    if (!first_baseline) goto done;

    u64 old_incarnation = system_console_incarnation();
    u64 old_app_endpoint = system_console_app_endpoint_id();
    u64 portal_endpoint = system_console_portal_endpoint_id();
    u64 portal_thread = system_console_portal_thread_id();
    bool restarted = system_console_restart() && system_console_idle() &&
        system_console_incarnation() && system_console_incarnation() != old_incarnation &&
        system_console_app_endpoint_id() && system_console_app_endpoint_id() != old_app_endpoint &&
        system_console_portal_endpoint_id() == portal_endpoint &&
        system_console_portal_thread_id() == portal_thread;
    report("SERVICE REPLACEMENT / CLIENT ENDPOINT REINCARNATED", restarted);
    if (!restarted) goto done;

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    void *stale = 0;
    bool stale_authority = kernel_caps && !capability_lookup(kernel_caps, first_authority,
        CAPABILITY_TYPE_ENDPOINT, &stale);
    report("OLD LAUNCH AUTHORITY REMAINS STALE", stale_authority);
    if (!stale_authority) goto done;

    ConsoleClientBaseline restarted_baseline;
    capture_baseline(&restarted_baseline);
    u64 second_authority = 0ULL;
    u64 second_delta = 0ULL;
    bool second = launch_hello(&second_authority, &second_delta) &&
        second_authority != first_authority && second_delta == first_delta;
    report("FRESH SESSION / SECOND APP AFTER RESTART", second);
    if (!second) goto done;

    bool final_baseline = baseline_matches(&restarted_baseline, true) &&
        program_launch_count() == launches_before + 3ULL && supervisor_ok();
    report("FINAL RESOURCE / SERVICE BASELINE", final_baseline);
    if (!final_baseline) goto done;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("R7 APPLICATION CONSOLE / STANDALONE UTILITY TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

done: {
        bool clean = cleanup_app();
        report("FAILURE CLEANUP", clean);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R7 APPLICATION CONSOLE / STANDALONE UTILITY TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}