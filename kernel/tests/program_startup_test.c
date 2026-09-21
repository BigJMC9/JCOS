#include "program_startup_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "ipc.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"
#include "../../include/launch_probe_protocol.h"
#include "../../include/program_startup.h"

#define PROGRAM_STARTUP_TEST_PATH "/bin/launchprobe.elf"
#define PROGRAM_STARTUP_TEST_COOKIE 0x5354415254555056ULL
#define PROGRAM_STARTUP_TEST_PING 0x5355504C41554E43ULL
#define PROGRAM_STARTUP_TIMEOUT_SECONDS 2ULL

static ProgramInstance g_probe;
static Endpoint g_probe_endpoint;
static CapabilityHandle g_kernel_receive;
static CapabilityHandle g_probe_send_grant;
static bool g_probe_endpoint_created;
static bool g_kernel_receive_cap;
static bool g_probe_send_grant_cap;

typedef struct {
    u64 free_pages;
    u32 process_count;
    u32 address_space_count;
    u32 thread_count;
    u32 endpoint_count;
    u32 exit_queue_count;
    u32 kernel_cap_count;
    u64 kernel_thread_count;
    u64 scheduler_count;
    u64 supervisor_pid;
    u64 supervisor_tid;
} StartupBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void capture_baseline(StartupBaseline *baseline) {
    if (!baseline) return;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    baseline->free_pages = pmm_stats().free_pages;
    baseline->process_count = process_object_count();
    baseline->address_space_count = address_space_object_count();
    baseline->thread_count = thread_object_count();
    baseline->endpoint_count = endpoint_object_count();
    baseline->exit_queue_count = process_exit_queue_object_count();
    baseline->kernel_cap_count = caps ? capability_table_count(caps) : 0U;
    baseline->kernel_thread_count = kernel ? process_thread_count(kernel) : 0ULL;
    baseline->scheduler_count = scheduler_thread_count();
    baseline->supervisor_pid = supervisor_process_id();
    baseline->supervisor_tid = supervisor_thread_id();
}

static bool baseline_counts_match(const StartupBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->process_count &&
        address_space_object_count() == baseline->address_space_count &&
        thread_object_count() == baseline->thread_count &&
        endpoint_object_count() == baseline->endpoint_count &&
        process_exit_queue_object_count() == baseline->exit_queue_count &&
        capability_table_count(caps) == baseline->kernel_cap_count &&
        process_thread_count(kernel) == baseline->kernel_thread_count &&
        scheduler_thread_count() == baseline->scheduler_count;
}

static bool baseline_matches(const StartupBaseline *baseline) {
    return baseline_counts_match(baseline) &&
        supervisor_process_id() == baseline->supervisor_pid &&
        supervisor_thread_id() == baseline->supervisor_tid;
}

static bool supervisor_healthy(void) {
    u64 reply = 0;
    return supervisor_running() && supervisor_stack_guarded() &&
        supervisor_ping(PROGRAM_STARTUP_TEST_PING, &reply) &&
        reply == PROGRAM_STARTUP_TEST_PING;
}

static bool cleanup_probe(void) {
    if (program_instance_needs_cleanup(&g_probe) && !program_terminate(&g_probe)) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    if (g_kernel_receive_cap) {
        if (!capability_revoke(caps, g_kernel_receive)) return false;
        g_kernel_receive_cap = false;
        g_kernel_receive = CAPABILITY_INVALID_HANDLE;
    }
    if (g_probe_send_grant_cap) {
        if (!capability_revoke(caps, g_probe_send_grant)) return false;
        g_probe_send_grant_cap = false;
        g_probe_send_grant = CAPABILITY_INVALID_HANDLE;
    }

    if (g_probe_endpoint_created) {
        if (endpoint_message_ready(&g_probe_endpoint)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(&g_probe_endpoint, &discard)) return false;
        }
        if (endpoint_receiver_waiting(&g_probe_endpoint) || endpoint_sender_waiting(&g_probe_endpoint)) return false;
        if (!endpoint_destroy(&g_probe_endpoint)) return false;
        g_probe_endpoint_created = false;
    }
    return true;
}

static bool cleanup_all(void) {
    if (!cleanup_probe()) return false;
    if (!supervisor_running()) {
        if (supervisor_state() != SUPERVISOR_STATE_STOPPED && !supervisor_recover()) return false;
        if (scheduler_thread_count() != 1ULL || !supervisor_start()) return false;
    }
    return supervisor_healthy();
}

void program_startup_test_cleanup_run(void) {
    terminal_writeln("PROGRAM STARTUP CLEANUP RETRY:");
    report("PROBE RELEASED / SUPERVISOR AVAILABLE", cleanup_all());
}

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * PROGRAM_STARTUP_TIMEOUT_SECONDS : 0;
}

static bool wait_probe_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (g_probe.thread.state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (g_probe.thread.state != THREAD_STATE_READY || !g_probe.thread.on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
    return !g_probe.thread.on_run_queue && !g_probe.thread.interrupt_context_ready &&
        !g_probe.thread.interrupt_rsp;
}

static bool create_probe_fixture(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;
    if (!endpoint_create(&g_probe_endpoint)) return false;
    g_probe_endpoint_created = true;
    if (!capability_insert(caps, &g_probe_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_kernel_receive)) return false;
    g_kernel_receive_cap = true;
    if (!capability_insert(caps, &g_probe_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER, &g_probe_send_grant)) return false;
    g_probe_send_grant_cap = true;
    return true;
}

void program_startup_test_run(void) {
    terminal_writeln("VERSIONED PROGRAM STARTUP / COMMON LAUNCHER TEST:");

    if (!cleanup_all()) {
        report("NO RETAINED FIXTURE / SUPERVISOR", false);
        return;
    }

    Process *kernel = process_kernel();
    Thread *main = thread_current();
    VfsNode *file = vfs_resolve(vfs_root(), PROGRAM_STARTUP_TEST_PATH);
    bool abi = sizeof(JcosProgramStartup) == 128U && JCOS_PROGRAM_STARTUP_VERSION == 1U &&
        JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES == 8U && JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS == 4U;
    bool preflight = abi && kernel && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled() &&
        file && file->type == VFS_FILE && file->data && file->size && supervisor_healthy();
    report("V1 ABI / PROGRAM FILE / SUPERVISOR BASELINE", preflight);
    if (!preflight) return;

    StartupBaseline before_restart;
    capture_baseline(&before_restart);
    u64 old_pid = before_restart.supervisor_pid;
    u64 old_tid = before_restart.supervisor_tid;
    u64 launches_before = program_launch_count();

    bool stopped = supervisor_stop_bounded() == SUPERVISOR_STOP_GRACEFUL &&
        !supervisor_running();
    bool restarted = stopped && supervisor_start();
    u64 new_pid = supervisor_process_id();
    u64 new_tid = supervisor_thread_id();
    bool common_launcher = restarted && program_launch_count() == launches_before + 1ULL &&
        new_pid && new_tid && new_pid != old_pid && new_tid != old_tid && supervisor_healthy();
    report("SUPERVISOR RESTART THROUGH COMMON LAUNCHER", common_launcher);

    bool restart_baseline = common_launcher && baseline_counts_match(&before_restart);
    report("SUPERVISOR RESOURCE BASELINE", restart_baseline);
    if (!restart_baseline) goto done;

    StartupBaseline stable;
    capture_baseline(&stable);

    if (!create_probe_fixture()) goto done;
    StartupBaseline fixture;
    capture_baseline(&fixture);

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.startup_grant_count = 1U;
    spec.startup_grants[0].authority_table = process_capabilities(kernel);
    spec.startup_grants[0].authority_handle = g_probe_send_grant;
    spec.startup_grants[0].object = &g_probe_endpoint;
    spec.startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[0].rights = CAPABILITY_RIGHT_SEND;
    spec.startup_argument_count = 1U;
    spec.startup_arguments[0] = PROGRAM_STARTUP_TEST_COOKIE;

    launches_before = program_launch_count();
    bool launched = program_launch(&g_probe, &spec);
    CapabilityTable *child_caps = launched ? process_capabilities(&g_probe.process) : 0;
    void *granted = 0;
    bool explicit_grant = launched && program_launch_count() == launches_before + 1ULL &&
        child_caps && capability_table_count(child_caps) == 1U &&
        capability_lookup_rights(child_caps, g_probe.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &granted) && granted == &g_probe_endpoint;
    report("INDEPENDENT C APP / EXPLICIT GRANT", explicit_grant);
    if (!explicit_grant) goto done;

    bool exited = wait_probe_dead();
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    bool received = exited && ipc_try_receive(kernel, g_kernel_receive, &message);
    bool startup_seen = received && message.word_count == 4U &&
        message.words[0] == JCOS_LAUNCH_PROBE_MESSAGE_READY &&
        message.words[1] == PROGRAM_STARTUP_TEST_COOKIE &&
        message.words[2] == JCOS_PROGRAM_STARTUP_VERSION && message.words[3] == 1ULL;
    report("STARTUP VERSION / ARGUMENT / CAPABILITY OBSERVED", startup_seen);

    bool reaped = startup_seen && program_reap(&g_probe) &&
        !program_instance_needs_cleanup(&g_probe) && baseline_matches(&fixture);
    report("PROBE NORMAL EXIT / REAP", reaped);
    if (!reaped) goto done;

    bool released = cleanup_probe();
    bool final_baseline = released && baseline_matches(&stable) && supervisor_healthy();
    report("PROBE CLEANUP / GLOBAL BASELINE", final_baseline);

    if (final_baseline) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("VERSIONED PROGRAM STARTUP / COMMON LAUNCHER TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

done:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("VERSIONED PROGRAM STARTUP / COMMON LAUNCHER TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
