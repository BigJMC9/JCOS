#include "managed_service_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "ipc.h"
#include "lib.h"
#include "managed_service.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "../../include/worker_service_protocol.h"

#define WORKER_PATH "/bin/workerservice.elf"
#define WORKER_COOKIE_1 0x574F524B45523031ULL
#define WORKER_COOKIE_2 0x574F524B45523032ULL
#define WORKER_COOKIE_3 0x574F524B45523033ULL
#define SUPERVISOR_COOKIE 0x574F524B53555056ULL
#define WAIT_SECONDS 2ULL

static ManagedService g_worker;

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
} ServiceBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void capture(ServiceBaseline *b) {
    if (!b) return;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    b->free_pages = pmm_stats().free_pages;
    b->processes = process_object_count();
    b->spaces = address_space_object_count();
    b->threads = thread_object_count();
    b->endpoints = endpoint_object_count();
    b->tables = capability_table_object_count();
    b->exit_queues = process_exit_queue_object_count();
    b->kernel_caps = caps ? capability_table_count(caps) : 0U;
    b->kernel_threads = kernel ? process_thread_count(kernel) : 0ULL;
    b->scheduler_threads = scheduler_thread_count();
    b->supervisor_pid = supervisor_process_id();
    b->supervisor_tid = supervisor_thread_id();
}

static bool baseline(const ServiceBaseline *b) {
    if (!b) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps && pmm_stats().free_pages == b->free_pages &&
        process_object_count() == b->processes && address_space_object_count() == b->spaces &&
        thread_object_count() == b->threads && endpoint_object_count() == b->endpoints &&
        capability_table_object_count() == b->tables && process_exit_queue_object_count() == b->exit_queues &&
        capability_table_count(caps) == b->kernel_caps && process_thread_count(kernel) == b->kernel_threads &&
        scheduler_thread_count() == b->scheduler_threads && supervisor_process_id() == b->supervisor_pid &&
        supervisor_thread_id() == b->supervisor_tid;
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(SUPERVISOR_COOKIE, &reply) && reply == SUPERVISOR_COOKIE;
}

static ManagedServiceSpec worker_spec(void) {
    ManagedServiceSpec spec;
    spec.path = WORKER_PATH;
    spec.shutdown_message = WORKER_SERVICE_MESSAGE_SHUTDOWN;
    spec.shutdown_reply = WORKER_SERVICE_REPLY_STOPPED;
    return spec;
}

static bool cleanup_worker(void) {
    ManagedServiceState state = managed_service_state(&g_worker);
    if (state == MANAGED_SERVICE_STOPPED) return true;
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult result = managed_service_stop_bounded(&g_worker);
        return result == MANAGED_SERVICE_STOP_GRACEFUL || result == MANAGED_SERVICE_STOP_FORCED;
    }
    return managed_service_recover(&g_worker);
}

static bool cleanup_all(void) {
    if (!cleanup_worker()) return false;
    if (!supervisor_running()) {
        if (!supervisor_recover() || !supervisor_start()) return false;
    }
    return supervisor_healthy();
}

void managed_service_test_cleanup_run(void) {
    terminal_writeln("MANAGED SERVICE CLEANUP RETRY:");
    report("WORKER RELEASED / SUPERVISOR AVAILABLE", cleanup_all());
}

static bool worker_ping(const ManagedServiceConnection *connection, u64 cookie) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = WORKER_SERVICE_MESSAGE_PING;
    request.words[1] = cookie;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    return managed_service_call(&g_worker, connection, &request, &reply) &&
        reply.word_count == 3U && reply.words[0] == WORKER_SERVICE_REPLY_PONG &&
        reply.words[1] == connection->incarnation && reply.words[2] == cookie;
}

static bool send_control(const ManagedServiceConnection *connection, u64 message) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 1U;
    request.words[0] = message;
    return managed_service_send_oneway(&g_worker, connection, &request);
}

static bool wait_failed(void) {
    if (!timer_initialized()) return false;
    u64 timeout = (u64)timer_frequency() * WAIT_SECONDS;
    if (!timeout) return false;
    u64 started = timer_ticks();
    for (;;) {
        ManagedServiceState state = managed_service_state(&g_worker);
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if (state != MANAGED_SERVICE_RUNNING || (u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool stale_capabilities(const ManagedServiceConnection *old_connection) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    void *object = 0;
    return caps && old_connection &&
        !capability_lookup(caps, old_connection->send_handle, CAPABILITY_TYPE_ENDPOINT, &object) &&
        !capability_lookup(caps, old_connection->receive_handle, CAPABILITY_TYPE_ENDPOINT, &object);
}

void managed_service_test_run(void) {
    terminal_writeln("SECOND C SERVICE / INCARNATION / RECONNECT TEST:");

    bool cleaned = cleanup_all();
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    bool preflight = cleaned && kernel && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue && scheduler_thread_count() == 1ULL &&
        scheduler_preemption_enabled() && timer_initialized() && supervisor_healthy();
    report("SUPERVISOR / PREEMPTION BASELINE", preflight);
    if (!preflight) goto fail;

    ServiceBaseline base;
    capture(&base);
    ManagedServiceSpec spec = worker_spec();

    bool started = managed_service_start(&g_worker, &spec);
    ManagedServiceConnection first;
    bool connected = started && managed_service_idle(&g_worker) &&
        managed_service_connect_kernel(&g_worker, &first) && first.incarnation != 0ULL;
    bool first_ping = connected && worker_ping(&first, WORKER_COOKIE_1);
    report("SECOND C SERVICE / COMMON LAUNCH / PING", first_ping);
    if (!first_ping) goto fail;

    u64 first_pid = managed_service_process_id(&g_worker);
    u64 first_tid = managed_service_thread_id(&g_worker);
    u64 first_incarnation = first.incarnation;
    bool unrelated_live = first_pid && first_tid && supervisor_healthy();
    report("UNRELATED SUPERVISOR REMAINS HEALTHY", unrelated_live);
    if (!unrelated_live) goto fail;

    bool fault_sent = send_control(&first, WORKER_SERVICE_MESSAGE_FAULT);
    bool failed = fault_sent && wait_failed();
    ProcessExitInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    bool fault_record = failed && managed_service_last_exit_info(&g_worker, &fault) &&
        fault.reason == PROCESS_EXIT_FAULT && fault.process_id == first_pid && fault.thread_id == first_tid &&
        fault.vector == 6ULL && supervisor_healthy();
    report("WORKER FAULT CONTAINED / SUPERVISOR UNAFFECTED", fault_record);
    if (!fault_record) goto fail;

    bool recovered = managed_service_recover(&g_worker) && managed_service_state(&g_worker) == MANAGED_SERVICE_STOPPED;
    bool restarted = recovered && managed_service_start(&g_worker, &spec);
    ManagedServiceConnection second;
    bool reconnected = restarted && managed_service_connect_kernel(&g_worker, &second) &&
        second.incarnation && second.incarnation != first_incarnation &&
        second.send_handle != first.send_handle && second.receive_handle != first.receive_handle &&
        stale_capabilities(&first) && worker_ping(&second, WORKER_COOKIE_2);
    report("NEW INCARNATION / OLD AUTHORITY STALE / EXPLICIT RECONNECT", reconnected);
    if (!reconnected) goto fail;

    /* Deliver a request to the blocked receiver, but terminate the incarnation
     * before its suspended userspace continuation can process/reply. This is an
     * intentionally ambiguous outstanding request: the manager does not replay
     * it against the replacement. */
    IpcMessage outstanding;
    k_memset(&outstanding, 0, sizeof(outstanding));
    outstanding.word_count = 2U;
    outstanding.words[0] = WORKER_SERVICE_MESSAGE_PING;
    outstanding.words[1] = 0x4F55545354414E44ULL;
    bool accepted = managed_service_send_oneway(&g_worker, &second, &outstanding);
    ManagedServiceStopResult forced = accepted ? managed_service_stop_bounded(&g_worker) : MANAGED_SERVICE_STOP_FAILED;
    ProcessExitInfo terminated;
    k_memset(&terminated, 0, sizeof(terminated));
    bool outstanding_failed = forced == MANAGED_SERVICE_STOP_FORCED &&
        managed_service_last_exit_info(&g_worker, &terminated) &&
        terminated.reason == PROCESS_EXIT_TERMINATED && stale_capabilities(&second);
    report("OUTSTANDING REQUEST CANCELLED / NOT REPLAYED", outstanding_failed);
    if (!outstanding_failed) goto fail;

    bool third_started = managed_service_start(&g_worker, &spec);
    ManagedServiceConnection third_connection;
    bool third_ok = third_started && managed_service_connect_kernel(&g_worker, &third_connection) &&
        third_connection.incarnation != second.incarnation && worker_ping(&third_connection, WORKER_COOKIE_3);
    report("POST-CANCEL RECONNECT / FRESH REQUEST", third_ok);
    if (!third_ok) goto fail;

    bool hang_sent = send_control(&third_connection, WORKER_SERVICE_MESSAGE_HANG);
    bool ran_hang = hang_sent && scheduler_yield();
    ManagedServiceStopResult hang_stop = ran_hang ? managed_service_stop_bounded(&g_worker) : MANAGED_SERVICE_STOP_FAILED;
    bool hang_contained = hang_stop == MANAGED_SERVICE_STOP_FORCED && supervisor_healthy();
    report("SPINNING SERVICE FORCED DOWN / SUPERVISOR PROGRESSES", hang_contained);
    if (!hang_contained) goto fail;

    bool final_started = managed_service_start(&g_worker, &spec);
    ManagedServiceConnection final_connection;
    bool final_live = final_started && managed_service_connect_kernel(&g_worker, &final_connection) &&
        worker_ping(&final_connection, WORKER_COOKIE_1);
    ManagedServiceStopResult graceful = final_live ? managed_service_stop_bounded(&g_worker) : MANAGED_SERVICE_STOP_FAILED;
    bool stopped = graceful == MANAGED_SERVICE_STOP_GRACEFUL &&
        managed_service_state(&g_worker) == MANAGED_SERVICE_STOPPED;
    report("FINAL INCARNATION GRACEFUL STOP / REAP", stopped);

    bool final = stopped && supervisor_healthy() && baseline(&base);
    report("FINAL HEALTH / RESOURCE BASELINE", final);
    if (final) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("SECOND C SERVICE / INCARNATION / RECONNECT TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

fail:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("SECOND C SERVICE / INCARNATION / RECONNECT TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
