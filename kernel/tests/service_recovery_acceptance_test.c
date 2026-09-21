#include "service_recovery_acceptance_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "lib.h"
#include "managed_service.h"
#include "pmm.h"
#include "pmm_test.h"
#include "process.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vmm_test.h"
#include "../../include/worker_service_protocol.h"

#define R6_SERVICE_ACCEPTANCE_CYCLES 20U
#define R6_SERVICE_WAIT_SECONDS 2ULL
#define R6_WORKER_PATH "/bin/workerservice.elf"
#define R6_SUPERVISOR_COOKIE_BASE 0x5236535550560000ULL
#define R6_WORKER_COOKIE_BASE 0x5236574F524B0000ULL

typedef enum {
    R6_SERVICE_CASE_GRACEFUL = 0,
    R6_SERVICE_CASE_FAULT,
    R6_SERVICE_CASE_OUTSTANDING,
    R6_SERVICE_CASE_HANG,
    R6_SERVICE_CASE_REAP_RETRY,
    R6_SERVICE_CASE_COUNT
} R6ServiceCase;

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
} R6ServiceBaseline;

static ManagedService g_r6_worker;
static bool g_r6_pmm_fault_owned;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static const char *case_name(R6ServiceCase test_case) {
    switch (test_case) {
        case R6_SERVICE_CASE_GRACEFUL: return "GRACEFUL STOP";
        case R6_SERVICE_CASE_FAULT: return "FAULT / RECOVER";
        case R6_SERVICE_CASE_OUTSTANDING: return "OUTSTANDING / NO REPLAY";
        case R6_SERVICE_CASE_HANG: return "HANG / FORCE";
        case R6_SERVICE_CASE_REAP_RETRY: return "REAP FAILURE / RETRY";
        default: return "UNKNOWN";
    }
}

static void print_cycle(u32 iteration, R6ServiceCase test_case, bool pass) {
    terminal_write("  CYCLE ");
    terminal_write_u64((u64)iteration + 1ULL);
    terminal_write(" [");
    terminal_write(case_name(test_case));
    terminal_write("]: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool supervisor_healthy(u32 salt) {
    u64 cookie = R6_SUPERVISOR_COOKIE_BASE ^ (u64)salt;
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(cookie, &reply) && reply == cookie;
}

static ManagedServiceSpec worker_spec(void) {
    ManagedServiceSpec spec;
    spec.path = R6_WORKER_PATH;
    spec.shutdown_message = WORKER_SERVICE_MESSAGE_SHUTDOWN;
    spec.shutdown_reply = WORKER_SERVICE_REPLY_STOPPED;
    return spec;
}

static void capture_baseline(R6ServiceBaseline *baseline) {
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
}

static bool baseline_restored(const R6ServiceBaseline *baseline, u32 salt) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    Thread *main = thread_current();
    return kernel && caps && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        managed_service_state(&g_r6_worker) == MANAGED_SERVICE_STOPPED &&
        !program_instance_needs_cleanup(&g_r6_worker.program) &&
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
        scheduler_preemption_enabled() && timer_initialized() &&
        !pmm_test_free_failure_armed() && !thread_creation_cleanup_pending() &&
        !address_space_creation_cleanup_pending() &&
        vmm_test_unlinked_table() == FRAME_INVALID && supervisor_healthy(salt);
}

static bool stale_capabilities(const ManagedServiceConnection *connection) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    void *object = 0;
    return caps && connection && connection->incarnation &&
        !capability_lookup(caps, connection->send_handle, CAPABILITY_TYPE_ENDPOINT, &object) &&
        !capability_lookup(caps, connection->receive_handle, CAPABILITY_TYPE_ENDPOINT, &object);
}

static bool same_exit(const ProcessExitInfo *a, const ProcessExitInfo *b) {
    return a && b && a->reason == b->reason && a->process_id == b->process_id &&
        a->thread_id == b->thread_id && a->vector == b->vector &&
        a->error_code == b->error_code && a->rip == b->rip && a->cr2 == b->cr2;
}

static bool worker_ping(const ManagedServiceConnection *connection, u64 cookie) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = WORKER_SERVICE_MESSAGE_PING;
    request.words[1] = cookie;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    return managed_service_call(&g_r6_worker, connection, &request, &reply) &&
        reply.word_count == 3U && reply.words[0] == WORKER_SERVICE_REPLY_PONG &&
        reply.words[1] == connection->incarnation && reply.words[2] == cookie;
}

static bool send_control(const ManagedServiceConnection *connection, u64 message) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 1U;
    request.words[0] = message;
    return managed_service_send_oneway(&g_r6_worker, connection, &request);
}

static bool send_outstanding(const ManagedServiceConnection *connection, u32 iteration) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = WORKER_SERVICE_MESSAGE_PING;
    request.words[1] = R6_WORKER_COOKIE_BASE ^ 0x4F555453ULL ^ (u64)iteration;
    return managed_service_send_oneway(&g_r6_worker, connection, &request);
}

static bool wait_failed(void) {
    if (!timer_initialized()) return false;
    u64 timeout = (u64)timer_frequency() * R6_SERVICE_WAIT_SECONDS;
    if (!timeout) return false;
    u64 started = timer_ticks();
    for (;;) {
        ManagedServiceState state = managed_service_state(&g_r6_worker);
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if (state != MANAGED_SERVICE_RUNNING || (u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool exit_reason(ProcessExitReason reason, u64 pid, u64 tid, u64 vector) {
    ProcessExitInfo info;
    k_memset(&info, 0, sizeof(info));
    return managed_service_last_exit_info(&g_r6_worker, &info) && info.reason == reason &&
        info.process_id == pid && (!tid || info.thread_id == tid) &&
        (reason != PROCESS_EXIT_FAULT || info.vector == vector);
}

static bool cleanup_worker(void) {
    if (g_r6_pmm_fault_owned && pmm_test_free_failure_armed()) {
        pmm_test_clear_free_failure();
    }
    g_r6_pmm_fault_owned = false;

    ManagedServiceState state = managed_service_state(&g_r6_worker);
    if (state == MANAGED_SERVICE_STOPPED) return true;
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult result = managed_service_stop_bounded(&g_r6_worker);
        if (result == MANAGED_SERVICE_STOP_GRACEFUL || result == MANAGED_SERVICE_STOP_FORCED) return true;
    }
    return managed_service_recover(&g_r6_worker);
}

static bool cleanup_all(void) {
    if (!cleanup_worker()) return false;
    if (!supervisor_running()) {
        if (!supervisor_recover() || !supervisor_start()) return false;
    }
    return supervisor_healthy(0xC1EAU);
}

void service_recovery_acceptance_test_cleanup_run(void) {
    terminal_writeln("R6 SERVICE RECOVERY ACCEPTANCE CLEANUP RETRY:");
    report("WORKER RELEASED / SUPERVISOR AVAILABLE", cleanup_all());
}

static bool run_cycle(u32 iteration, R6ServiceCase test_case, const R6ServiceBaseline *baseline,
    const ManagedServiceSpec *spec, ManagedServiceConnection *previous, bool *have_previous,
    ManagedServiceConnection *first, bool *have_first) {
    if (!baseline || !spec || !previous || !have_previous || !first || !have_first) return false;
    if (!baseline_restored(baseline, iteration + 1U)) return false;

    if (!managed_service_start(&g_r6_worker, spec) || !managed_service_idle(&g_r6_worker)) return false;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!managed_service_connect_kernel(&g_r6_worker, &connection) || !connection.incarnation) return false;
    if (*have_previous && (connection.incarnation == previous->incarnation ||
            connection.send_handle == previous->send_handle ||
            connection.receive_handle == previous->receive_handle || !stale_capabilities(previous))) return false;
    if (!*have_first) {
        *first = connection;
        *have_first = true;
    }

    u64 pid = managed_service_process_id(&g_r6_worker);
    u64 tid = managed_service_thread_id(&g_r6_worker);
    u64 cookie = R6_WORKER_COOKIE_BASE ^ (u64)iteration;
    if (!pid || !tid || !worker_ping(&connection, cookie) || !supervisor_healthy(iteration + 0x100U)) return false;

    bool completed = false;
    switch (test_case) {
        case R6_SERVICE_CASE_GRACEFUL: {
            ManagedServiceStopResult result = managed_service_stop_bounded(&g_r6_worker);
            completed = result == MANAGED_SERVICE_STOP_GRACEFUL &&
                managed_service_last_stop_result(&g_r6_worker) == MANAGED_SERVICE_STOP_GRACEFUL &&
                exit_reason(PROCESS_EXIT_NORMAL, pid, tid, 0ULL);
            break;
        }
        case R6_SERVICE_CASE_FAULT: {
            completed = send_control(&connection, WORKER_SERVICE_MESSAGE_FAULT) && wait_failed() &&
                exit_reason(PROCESS_EXIT_FAULT, pid, tid, 6ULL) &&
                managed_service_recover(&g_r6_worker);
            break;
        }
        case R6_SERVICE_CASE_OUTSTANDING: {
            ManagedServiceStopResult result = send_outstanding(&connection, iteration) ?
                managed_service_stop_bounded(&g_r6_worker) : MANAGED_SERVICE_STOP_FAILED;
            completed = result == MANAGED_SERVICE_STOP_FORCED &&
                managed_service_last_stop_result(&g_r6_worker) == MANAGED_SERVICE_STOP_FORCED &&
                exit_reason(PROCESS_EXIT_TERMINATED, pid, 0ULL, 0ULL);
            break;
        }
        case R6_SERVICE_CASE_HANG: {
            u64 preempt_before = scheduler_preemption_count();
            u64 ticks_before = timer_ticks();
            bool dispatched = send_control(&connection, WORKER_SERVICE_MESSAGE_HANG) && scheduler_yield();
            u64 preempt_delta = scheduler_preemption_count() - preempt_before;
            u64 tick_delta = timer_ticks() - ticks_before;
            ManagedServiceStopResult result = dispatched ? managed_service_stop_bounded(&g_r6_worker) :
                MANAGED_SERVICE_STOP_FAILED;
            completed = dispatched && preempt_delta > 0ULL && tick_delta > 0ULL &&
                result == MANAGED_SERVICE_STOP_FORCED &&
                exit_reason(PROCESS_EXIT_TERMINATED, pid, 0ULL, 0ULL);
            break;
        }
        case R6_SERVICE_CASE_REAP_RETRY: {
            if (!send_control(&connection, WORKER_SERVICE_MESSAGE_FAULT) || !wait_failed()) break;
            ProcessExitInfo before;
            k_memset(&before, 0, sizeof(before));
            frame_t stack = phys_to_frame(g_r6_worker.program.thread.kernel_stack_physical);
            if (!managed_service_last_exit_info(&g_r6_worker, &before) ||
                before.reason != PROCESS_EXIT_FAULT || before.process_id != pid ||
                before.thread_id != tid || before.vector != 6ULL || stack == FRAME_INVALID ||
                !pmm_test_fail_free_range_once(stack, THREAD_KERNEL_STACK_PAGES)) break;
            g_r6_pmm_fault_owned = true;
            bool first_failed = !managed_service_recover(&g_r6_worker) && !pmm_test_free_failure_armed() &&
                managed_service_state(&g_r6_worker) != MANAGED_SERVICE_STOPPED;
            g_r6_pmm_fault_owned = false;
            ProcessExitInfo retained;
            k_memset(&retained, 0, sizeof(retained));
            bool preserved = first_failed && managed_service_last_exit_info(&g_r6_worker, &retained) &&
                same_exit(&before, &retained);
            bool retried = preserved && managed_service_recover(&g_r6_worker) &&
                managed_service_state(&g_r6_worker) == MANAGED_SERVICE_STOPPED;
            ProcessExitInfo after;
            k_memset(&after, 0, sizeof(after));
            completed = retried && managed_service_last_exit_info(&g_r6_worker, &after) &&
                same_exit(&before, &after);
            break;
        }
        default:
            break;
    }

    if (!completed || managed_service_state(&g_r6_worker) != MANAGED_SERVICE_STOPPED ||
        !stale_capabilities(&connection) || !supervisor_healthy(iteration + 0x200U) ||
        !baseline_restored(baseline, iteration + 0x300U)) return false;

    *previous = connection;
    *have_previous = true;
    return true;
}

void service_recovery_acceptance_test_run(void) {
    terminal_writeln("R6 SERVICE RECOVERY ACCEPTANCE TEST:");

    bool cleaned = cleanup_all();
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    bool preflight = cleaned && kernel && main && caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && scheduler_preemption_enabled() && timer_initialized() &&
        !pmm_test_free_failure_armed() && !thread_creation_cleanup_pending() &&
        !address_space_creation_cleanup_pending() && vmm_test_unlinked_table() == FRAME_INVALID &&
        supervisor_healthy(0U);
    report("R6 CLEAN BASELINE / SUPERVISOR", preflight);
    if (!preflight) goto fail;

    R6ServiceBaseline baseline;
    capture_baseline(&baseline);
    ManagedServiceSpec spec = worker_spec();
    u64 launches_before = program_launch_count();

    ManagedServiceConnection previous;
    ManagedServiceConnection first;
    k_memset(&previous, 0, sizeof(previous));
    k_memset(&first, 0, sizeof(first));
    bool have_previous = false;
    bool have_first = false;
    bool pass = true;

    terminal_write("  CYCLES: ");
    terminal_write_u64(R6_SERVICE_ACCEPTANCE_CYCLES);
    terminal_putchar('\n');

    for (u32 i = 0; i < R6_SERVICE_ACCEPTANCE_CYCLES; ++i) {
        R6ServiceCase test_case = (R6ServiceCase)(i % (u32)R6_SERVICE_CASE_COUNT);
        bool cycle = run_cycle(i, test_case, &baseline, &spec, &previous, &have_previous,
            &first, &have_first);
        print_cycle(i, test_case, cycle);
        if (!cycle) {
            pass = false;
            break;
        }
    }

    bool first_stale = have_first && stale_capabilities(&first) &&
        !managed_service_connection_current(&g_r6_worker, &first);
    bool launches = program_launch_count() == launches_before + R6_SERVICE_ACCEPTANCE_CYCLES;
    bool final_baseline = pass && baseline_restored(&baseline, 0xFFFFU);
    report("EARLIEST INCARNATION AUTHORITY STILL STALE", first_stale);
    report("COMMON LAUNCH COUNT", launches);
    report("FINAL FRAME / OBJECT / QUEUE / CAP BASELINE", final_baseline);

    if (pass && first_stale && launches && final_baseline) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R6 SERVICE RECOVERY ACCEPTANCE TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

fail:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R6 SERVICE RECOVERY ACCEPTANCE TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
