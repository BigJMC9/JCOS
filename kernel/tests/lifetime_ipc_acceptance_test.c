#include "lifetime_ipc_acceptance_test.h"

#include "address_space.h"
#include "capability.h"
#include "capability_test.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "ipc_timeout_test.h"
#include "ipc_wait_test.h"
#include "lib.h"
#include "pmm.h"
#include "pmm_test.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vmm_test.h"

#define LIFETIME_ACCEPTANCE_CYCLES 32U
#define LIFETIME_TIMEOUT_TICKS 2ULL
#define LIFETIME_TIMEOUT_SPIN_LIMIT 200000000ULL
#define LIFETIME_RFLAGS_IF (1ULL << 9)

#define LIFETIME_TAG_PREFILL 0x523250524546494CULL
#define LIFETIME_TAG_WAKE    0x523257414B454D53ULL
#define LIFETIME_TAG_WORK    0x5232574F524B4D53ULL
#define LIFETIME_TAG_LATE    0x52324C4154454D53ULL

typedef enum {
    LIFETIME_CASE_BLOCKED_RECEIVE_PEER = 0,
    LIFETIME_CASE_READY_RECEIVE_PEER,
    LIFETIME_CASE_LAST_CAP_RECEIVE_PEER,
    LIFETIME_CASE_BLOCKED_SEND_PEER,
    LIFETIME_CASE_COMMITTED_SEND_PEER,
    LIFETIME_CASE_RECEIVE_TIMEOUT,
    LIFETIME_CASE_SEND_TIMEOUT,
    LIFETIME_CASE_RECLAIM_RETRY
} LifetimeAcceptanceCase;

typedef struct {
    u64 frames;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 tables;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 supervisor_pid;
    u64 supervisor_tid;
    CapabilitySlot kernel_slots[CAPABILITY_TABLE_CAPACITY];
} LifetimeAcceptanceBaseline;

typedef struct {
    Process service;
    Process client;
    Endpoint endpoint;
    Thread worker;

    CapabilityHandle kernel_send;
    CapabilityHandle kernel_receive;
    CapabilityHandle client_handle;
    CapabilityHandle stale_handle;

    LifetimeAcceptanceCase test_case;
    volatile bool worker_started;
    volatile bool worker_returned;
    bool worker_result;
    IpcMessage worker_output;

    bool service_live;
    bool client_live;
    bool endpoint_live;
    bool worker_live;
    bool kernel_send_live;
    bool kernel_receive_live;
    bool client_cap_live;
    bool active;
    bool pmm_fault_owned;
    bool vmm_fault_owned;
} LifetimeAcceptanceFixture;

static LifetimeAcceptanceBaseline g_acceptance_baseline;
static LifetimeAcceptanceFixture g_acceptance;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void irq_restore(u64 flags) {
    if (flags & LIFETIME_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = irq_save();
    bool result = scheduler_yield();
    irq_restore(flags);
    return result;
}

static void fill_message(IpcMessage *message, u64 tag) {
    if (!message) return;
    k_memset(message, 0, sizeof(*message));
    message->word_count = IPC_MESSAGE_MAX_WORDS;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = tag ^ (u64)i;
}

static bool message_matches(const IpcMessage *message, u64 tag) {
    if (!message || message->word_count != IPC_MESSAGE_MAX_WORDS) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        if (message->words[i] != (tag ^ (u64)i)) return false;
    }
    return true;
}

static bool message_zero(const IpcMessage *message) {
    if (!message || message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message->words[i]) return false;
    return true;
}

static bool supervisor_ping_ok(void) {
    u64 cookie = 0x5232414343455054ULL;
    u64 reply = 0;
    return supervisor_process_id() == g_acceptance_baseline.supervisor_pid &&
        supervisor_thread_id() == g_acceptance_baseline.supervisor_tid &&
        supervisor_ping(cookie, &reply) && reply == cookie;
}

static bool kernel_caps_unchanged(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || capability_table_count(caps) != g_acceptance_baseline.kernel_caps) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *before = &g_acceptance_baseline.kernel_slots[i];
        const CapabilitySlot *after = &caps->slots[i];
        if (before->occupied != after->occupied) return false;
        if (!before->occupied) continue;
        if (before->object != after->object || before->object_id != after->object_id ||
            before->rights != after->rights || before->type != after->type ||
            before->generation != after->generation) return false;
    }
    return true;
}

static bool baseline_restored(void) {
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    IpcWaitTestObservation observation = ipc_wait_test_observation();
    return kernel && main && main->process == kernel && main->state == THREAD_STATE_RUNNING &&
        main->on_run_queue && scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled() &&
        pmm_stats().free_pages == g_acceptance_baseline.frames &&
        process_object_count() == g_acceptance_baseline.processes &&
        address_space_object_count() == g_acceptance_baseline.spaces &&
        thread_object_count() == g_acceptance_baseline.threads &&
        endpoint_object_count() == g_acceptance_baseline.endpoints &&
        capability_table_object_count() == g_acceptance_baseline.tables &&
        process_thread_count(kernel) == g_acceptance_baseline.kernel_threads &&
        kernel_caps_unchanged() && !thread_creation_cleanup_pending() &&
        !address_space_creation_cleanup_pending() && !pmm_test_free_failure_armed() &&
        !vmm_test_faults_armed() && !ipc_timeout_test_active_count() && !observation.armed && supervisor_ping_ok();
}

static bool prerequisites_clean(void) {
    IpcWaitTestObservation observation = ipc_wait_test_observation();
    return timer_initialized() && !g_acceptance.active && !thread_creation_cleanup_pending() &&
        !address_space_creation_cleanup_pending() && !pmm_test_free_failure_armed() &&
        !vmm_test_faults_armed() && !ipc_timeout_test_active_count() && !observation.armed;
}

static bool drop_kernel_caps(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;
    if (g_acceptance.kernel_send_live) {
        if (!capability_revoke(caps, g_acceptance.kernel_send)) return false;
        g_acceptance.kernel_send_live = false;
    }
    if (g_acceptance.kernel_receive_live) {
        if (!capability_revoke(caps, g_acceptance.kernel_receive)) return false;
        g_acceptance.kernel_receive_live = false;
    }
    return true;
}

static bool cleanup_fixture(void) {
    if (!g_acceptance.active) return true;

    if (g_acceptance.pmm_fault_owned && pmm_test_free_failure_armed()) {
        pmm_test_clear_free_failure();
        g_acceptance.pmm_fault_owned = false;
    }
    if (g_acceptance.vmm_fault_owned && vmm_test_faults_armed()) {
        vmm_test_clear_faults();
        g_acceptance.vmm_fault_owned = false;
    }

    if (g_acceptance.service_live) {
        if (!task_quiesce_process(&g_acceptance.service)) return false;
        if (!process_destroy(&g_acceptance.service)) return false;
        g_acceptance.service_live = false;
    }

    if (g_acceptance.client_live) {
        if (!task_quiesce_process(&g_acceptance.client)) return false;
        g_acceptance.worker_live = false;
        g_acceptance.client_cap_live = false;
        if (!process_destroy(&g_acceptance.client)) return false;
        g_acceptance.client_live = false;
    }

    if (!drop_kernel_caps()) return false;

    if (g_acceptance.endpoint_live) {
        if (endpoint_message_ready(&g_acceptance.endpoint) && !endpoint_closed(&g_acceptance.endpoint)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(&g_acceptance.endpoint, &discard)) return false;
        }
        if (!endpoint_destroy(&g_acceptance.endpoint)) return false;
        g_acceptance.endpoint_live = false;
    }

    if (!thread_reclaim_unpublished_stack() || !address_space_reclaim_unpublished() ||
        !vmm_reclaim_unlinked_table()) return false;

    g_acceptance.active = false;
    return baseline_restored();
}

void lifetime_ipc_acceptance_cleanup_run(void) {
    terminal_writeln("LIFETIME / IPC ACCEPTANCE CLEANUP RETRY:");
    if (!g_acceptance.active) {
        report("NOTHING RETAINED", true);
        return;
    }
    bool clean = cleanup_fixture();
    report("RETAINED FIXTURE RELEASED", clean);
    if (!clean) terminal_writeln("LIFETIME / IPC ACCEPTANCE FIXTURE RETAINED. REBOOT BEFORE FURTHER TESTS.");
    else terminal_writeln("ORIGINAL TEST WAS NOT RERUN.");
}

static void worker_main(void *argument) {
    (void)argument;
    g_acceptance.worker_started = true;
    k_memset(&g_acceptance.worker_output, 0xFF, sizeof(g_acceptance.worker_output));

    if (g_acceptance.test_case == LIFETIME_CASE_BLOCKED_RECEIVE_PEER ||
        g_acceptance.test_case == LIFETIME_CASE_READY_RECEIVE_PEER ||
        g_acceptance.test_case == LIFETIME_CASE_LAST_CAP_RECEIVE_PEER ||
        g_acceptance.test_case == LIFETIME_CASE_RECLAIM_RETRY) {
        g_acceptance.worker_result = ipc_receive_blocking(&g_acceptance.client, g_acceptance.client_handle, &g_acceptance.worker_output);
    } else if (g_acceptance.test_case == LIFETIME_CASE_RECEIVE_TIMEOUT) {
        g_acceptance.worker_result = ipc_receive_blocking_for(&g_acceptance.client, g_acceptance.client_handle,
            &g_acceptance.worker_output, LIFETIME_TIMEOUT_TICKS);
    } else {
        IpcMessage message;
        fill_message(&message, LIFETIME_TAG_WORK ^ (u64)g_acceptance.test_case);
        if (g_acceptance.test_case == LIFETIME_CASE_SEND_TIMEOUT) {
            g_acceptance.worker_result = ipc_send_blocking_for(&g_acceptance.client, g_acceptance.client_handle,
                &message, LIFETIME_TIMEOUT_TICKS);
        } else {
            g_acceptance.worker_result = ipc_send_blocking(&g_acceptance.client, g_acceptance.client_handle, &message);
        }
    }

    g_acceptance.worker_returned = true;
    scheduler_exit_current();
}

static bool setup_cycle(LifetimeAcceptanceCase test_case) {
    if (g_acceptance.active) return false;
    k_memset(&g_acceptance, 0, sizeof(g_acceptance));
    g_acceptance.active = true;
    g_acceptance.test_case = test_case;

    if (!process_create(&g_acceptance.service)) return false;
    g_acceptance.service_live = true;
    if (!process_create(&g_acceptance.client)) return false;
    g_acceptance.client_live = true;
    if (!endpoint_create_owned(&g_acceptance.endpoint, &g_acceptance.service)) return false;
    g_acceptance.endpoint_live = true;

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    CapabilityTable *client_caps = process_capabilities(&g_acceptance.client);
    if (!kernel_caps || !client_caps) return false;

    if (!capability_insert(kernel_caps, &g_acceptance.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_acceptance.kernel_send)) return false;
    g_acceptance.kernel_send_live = true;
    if (!capability_insert(kernel_caps, &g_acceptance.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_acceptance.kernel_receive)) return false;
    g_acceptance.kernel_receive_live = true;

    CapabilityRights rights = (test_case == LIFETIME_CASE_BLOCKED_SEND_PEER ||
        test_case == LIFETIME_CASE_COMMITTED_SEND_PEER || test_case == LIFETIME_CASE_SEND_TIMEOUT)
        ? CAPABILITY_RIGHT_SEND : CAPABILITY_RIGHT_RECEIVE;
    if (!capability_insert(client_caps, &g_acceptance.endpoint, CAPABILITY_TYPE_ENDPOINT,
            rights, &g_acceptance.client_handle)) return false;
    g_acceptance.client_cap_live = true;

    if (test_case == LIFETIME_CASE_RECLAIM_RETRY) {
        g_acceptance.stale_handle = g_acceptance.client_handle;
        if (!capability_revoke(client_caps, g_acceptance.client_handle)) return false;
        g_acceptance.client_cap_live = false;
        void *object = 0;
        if (capability_lookup(client_caps, g_acceptance.stale_handle, CAPABILITY_TYPE_ENDPOINT, &object)) return false;
        if (!capability_insert(client_caps, &g_acceptance.endpoint, CAPABILITY_TYPE_ENDPOINT,
                CAPABILITY_RIGHT_RECEIVE, &g_acceptance.client_handle)) return false;
        g_acceptance.client_cap_live = true;
        if (g_acceptance.client_handle == g_acceptance.stale_handle) return false;
        IpcMessage immediate;
        fill_message(&immediate, LIFETIME_TAG_WAKE);
        if (!ipc_try_send(kernel, g_acceptance.kernel_send, &immediate)) return false;
    }

    if (test_case == LIFETIME_CASE_BLOCKED_SEND_PEER || test_case == LIFETIME_CASE_COMMITTED_SEND_PEER ||
        test_case == LIFETIME_CASE_SEND_TIMEOUT) {
        IpcMessage prefill;
        fill_message(&prefill, LIFETIME_TAG_PREFILL);
        if (!ipc_try_send(kernel, g_acceptance.kernel_send, &prefill)) return false;
    }

    if (!thread_create(&g_acceptance.worker, &g_acceptance.client)) return false;
    g_acceptance.worker_live = true;
    if (!thread_prepare_kernel(&g_acceptance.worker, worker_main, 0) || !scheduler_add(&g_acceptance.worker)) return false;
    return true;
}

static bool blocked_wait(ThreadWaitKind kind) {
    return g_acceptance.worker_started && !g_acceptance.worker_returned &&
        g_acceptance.worker.state == THREAD_STATE_BLOCKED && !g_acceptance.worker.on_run_queue &&
        g_acceptance.worker.wait_result == THREAD_WAIT_RESULT_PENDING &&
        thread_wait_matches_id(&g_acceptance.worker, kind, &g_acceptance.endpoint, g_acceptance.worker.wait_id);
}

static bool wait_terminal(ThreadWaitResult result, u64 max_ticks) {
    u64 start = timer_ticks();
    for (u64 spins = 0; spins < LIFETIME_TIMEOUT_SPIN_LIMIT; ++spins) {
        if (g_acceptance.worker.wait_result == result) return true;
        if ((u64)(timer_ticks() - start) > max_ticks) break;
        __asm__ volatile ("pause");
    }
    return g_acceptance.worker.wait_result == result;
}

static bool quiesce_service(void) {
    if (!g_acceptance.service_live) return false;
    if (!task_quiesce_process(&g_acceptance.service)) return false;
    if (!endpoint_closed(&g_acceptance.endpoint)) return false;
    if (!process_destroy(&g_acceptance.service)) return false;
    g_acceptance.service_live = false;
    return true;
}

static bool resume_worker(bool expected_result, bool expect_zero_output) {
    if (!schedule_once() || !g_acceptance.worker_returned || g_acceptance.worker_result != expected_result ||
        g_acceptance.worker.state != THREAD_STATE_DEAD || g_acceptance.worker.on_run_queue ||
        thread_wait_active(&g_acceptance.worker)) return false;
    if (expect_zero_output && !message_zero(&g_acceptance.worker_output)) return false;
    return true;
}

static bool normal_cycle(LifetimeAcceptanceCase test_case) {
    if (!setup_cycle(test_case)) return false;
    Process *kernel = process_kernel();
    CapabilityTable *client_caps = process_capabilities(&g_acceptance.client);
    if (!kernel || !client_caps || !schedule_once()) return false;

    if (test_case == LIFETIME_CASE_RECLAIM_RETRY) {
        if (!g_acceptance.worker_returned || !g_acceptance.worker_result ||
            !message_matches(&g_acceptance.worker_output, LIFETIME_TAG_WAKE) ||
            g_acceptance.worker.state != THREAD_STATE_DEAD) return false;
        if (!quiesce_service()) return false;

        frame_t first = phys_to_frame(g_acceptance.worker.kernel_stack_physical);
        if (first == FRAME_INVALID || !pmm_test_fail_free_range_once(first, THREAD_KERNEL_STACK_PAGES)) return false;
        g_acceptance.pmm_fault_owned = true;
        if (task_quiesce_process(&g_acceptance.client)) return false;
        if (pmm_test_free_failure_armed() || process_thread_count(&g_acceptance.client) != 1ULL ||
            !thread_storage_in_use(&g_acceptance.worker)) return false;
        g_acceptance.pmm_fault_owned = false;
        if (!task_quiesce_process(&g_acceptance.client)) return false;
        g_acceptance.worker_live = false;
        g_acceptance.client_cap_live = false;

        if (!vmm_test_fail_once(&g_acceptance.client.owned_address_space.page_map, VMM_TEST_FREE, 0U)) return false;
        g_acceptance.vmm_fault_owned = true;
        if (process_destroy(&g_acceptance.client)) return false;
        if (vmm_test_faults_armed() || !process_address_space(&g_acceptance.client)) return false;
        g_acceptance.vmm_fault_owned = false;
        if (!process_destroy(&g_acceptance.client)) return false;
        g_acceptance.client_live = false;

        if (!drop_kernel_caps()) return false;
        if (!endpoint_destroy(&g_acceptance.endpoint)) return false;
        g_acceptance.endpoint_live = false;
        g_acceptance.active = false;
        return baseline_restored();
    }

    ThreadWaitKind kind = (test_case == LIFETIME_CASE_BLOCKED_SEND_PEER ||
        test_case == LIFETIME_CASE_COMMITTED_SEND_PEER || test_case == LIFETIME_CASE_SEND_TIMEOUT)
        ? THREAD_WAIT_IPC_SEND : THREAD_WAIT_IPC_RECEIVE;
    if (!blocked_wait(kind)) return false;

    if (test_case == LIFETIME_CASE_READY_RECEIVE_PEER) {
        IpcMessage wake;
        fill_message(&wake, LIFETIME_TAG_WAKE);
        if (!ipc_try_send(kernel, g_acceptance.kernel_send, &wake) ||
            g_acceptance.worker.state != THREAD_STATE_READY ||
            g_acceptance.worker.wait_result != THREAD_WAIT_RESULT_PENDING) return false;
    }

    if (test_case == LIFETIME_CASE_LAST_CAP_RECEIVE_PEER) {
        if (!capability_revoke(client_caps, g_acceptance.client_handle)) return false;
        g_acceptance.client_cap_live = false;
        if (!drop_kernel_caps()) return false;
        void *object = 0;
        if (capability_lookup(client_caps, g_acceptance.client_handle, CAPABILITY_TYPE_ENDPOINT, &object) ||
            g_acceptance.endpoint.capability_refs || endpoint_destroy(&g_acceptance.endpoint)) return false;
    }

    if (test_case == LIFETIME_CASE_COMMITTED_SEND_PEER) {
        IpcMessage old;
        k_memset(&old, 0, sizeof(old));
        if (!ipc_try_receive(kernel, g_acceptance.kernel_receive, &old) || !message_matches(&old, LIFETIME_TAG_PREFILL) ||
            g_acceptance.worker.state != THREAD_STATE_READY ||
            g_acceptance.worker.wait_result != THREAD_WAIT_RESULT_COMPLETED) return false;
    }

    if (test_case == LIFETIME_CASE_RECEIVE_TIMEOUT || test_case == LIFETIME_CASE_SEND_TIMEOUT) {
        if (!wait_terminal(THREAD_WAIT_RESULT_TIMED_OUT, LIFETIME_TIMEOUT_TICKS + 4ULL) ||
            g_acceptance.worker.state != THREAD_STATE_READY || !g_acceptance.worker.on_run_queue) return false;

        if (test_case == LIFETIME_CASE_RECEIVE_TIMEOUT) {
            IpcMessage late;
            fill_message(&late, LIFETIME_TAG_LATE);
            if (ipc_try_send(kernel, g_acceptance.kernel_send, &late)) return false;
            if (!resume_worker(false, true)) return false;
            if (!ipc_try_send(kernel, g_acceptance.kernel_send, &late)) return false;
            IpcMessage recovered;
            k_memset(&recovered, 0, sizeof(recovered));
            if (!ipc_try_receive(kernel, g_acceptance.kernel_receive, &recovered) ||
                !message_matches(&recovered, LIFETIME_TAG_LATE)) return false;
        } else {
            if (!resume_worker(false, false)) return false;
            IpcMessage original;
            k_memset(&original, 0, sizeof(original));
            if (!ipc_try_receive(kernel, g_acceptance.kernel_receive, &original) ||
                !message_matches(&original, LIFETIME_TAG_PREFILL)) return false;
            IpcMessage extra;
            k_memset(&extra, 0, sizeof(extra));
            if (ipc_try_receive(kernel, g_acceptance.kernel_receive, &extra)) return false;
        }
    } else {
        bool expected_success = test_case == LIFETIME_CASE_COMMITTED_SEND_PEER;
        if (!quiesce_service()) return false;
        ThreadWaitResult expected = expected_success ? THREAD_WAIT_RESULT_COMPLETED : THREAD_WAIT_RESULT_PEER_CLOSED;
        if (g_acceptance.worker.wait_result != expected || g_acceptance.worker.state != THREAD_STATE_READY) return false;
        if (!resume_worker(expected_success,
                test_case == LIFETIME_CASE_BLOCKED_RECEIVE_PEER ||
                test_case == LIFETIME_CASE_READY_RECEIVE_PEER ||
                test_case == LIFETIME_CASE_LAST_CAP_RECEIVE_PEER)) return false;
    }

    bool clean = cleanup_fixture();
    return clean;
}

static const char *case_name(LifetimeAcceptanceCase test_case) {
    switch (test_case) {
        case LIFETIME_CASE_BLOCKED_RECEIVE_PEER: return "BLOCKED RX / PEER DEATH";
        case LIFETIME_CASE_READY_RECEIVE_PEER: return "READY RX / PEER DEATH";
        case LIFETIME_CASE_LAST_CAP_RECEIVE_PEER: return "LAST CAP / WAIT RESERVATION";
        case LIFETIME_CASE_BLOCKED_SEND_PEER: return "BLOCKED TX / PEER DEATH";
        case LIFETIME_CASE_COMMITTED_SEND_PEER: return "COMMITTED TX / PEER DEATH";
        case LIFETIME_CASE_RECEIVE_TIMEOUT: return "RX TIMEOUT / LATE REPLY";
        case LIFETIME_CASE_SEND_TIMEOUT: return "TX TIMEOUT / STAGING";
        case LIFETIME_CASE_RECLAIM_RETRY: return "REAP + ROOT RETRY / STALE CAP";
        default: return "UNKNOWN";
    }
}

void lifetime_ipc_acceptance_test_run(void) {
    terminal_writeln("LIFETIME / IPC ACCEPTANCE TEST:");
    if (!prerequisites_clean()) {
        report("PRECONDITIONS / NO RETAINED TEST STATE", false);
        terminal_writeln("  RUN THE RELEVANT CLEANUP RETRY OR REBOOT BEFORE THIS GATE.");
        return;
    }

    Process *kernel = process_kernel();
    Thread *main = thread_current();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    bool quiet = kernel && main && kernel_caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();
    report("MAIN THREAD / SINGLE-CPU BASELINE", quiet);
    if (!quiet) return;

    k_memset(&g_acceptance_baseline, 0, sizeof(g_acceptance_baseline));
    g_acceptance_baseline.frames = pmm_stats().free_pages;
    g_acceptance_baseline.processes = process_object_count();
    g_acceptance_baseline.spaces = address_space_object_count();
    g_acceptance_baseline.threads = thread_object_count();
    g_acceptance_baseline.endpoints = endpoint_object_count();
    g_acceptance_baseline.tables = capability_table_object_count();
    g_acceptance_baseline.kernel_caps = capability_table_count(kernel_caps);
    g_acceptance_baseline.kernel_threads = process_thread_count(kernel);
    g_acceptance_baseline.supervisor_pid = supervisor_process_id();
    g_acceptance_baseline.supervisor_tid = supervisor_thread_id();
    k_memcpy(g_acceptance_baseline.kernel_slots, kernel_caps->slots, sizeof(g_acceptance_baseline.kernel_slots));

    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(g_acceptance_baseline.frames);
    terminal_putchar('\n');

    bool supervisor_before = supervisor_ping_ok();
    report("SUPERVISOR BEFORE", supervisor_before);
    bool generation_policy = capability_test_generation_boundary();
    report("GENERATION EXHAUSTION POLICY", generation_policy);
    if (!supervisor_before || !generation_policy) return;

    bool pass = true;
    for (u32 i = 0; i < LIFETIME_ACCEPTANCE_CYCLES; ++i) {
        LifetimeAcceptanceCase test_case = (LifetimeAcceptanceCase)(i % 8U);
        bool cycle = normal_cycle(test_case);
        terminal_write("  CYCLE ");
        terminal_write_u64((u64)i + 1ULL);
        terminal_write(" [");
        terminal_write(case_name(test_case));
        terminal_write("]: ");
        terminal_writeln(cycle ? "PASS" : "FAILED");
        if (!cycle) {
            pass = false;
            break;
        }
    }

    bool final_baseline = !g_acceptance.active && baseline_restored();
    report("FINAL FRAME / OBJECT / AUTHORITY BASELINES", final_baseline);
    report("TIMED-WAIT TABLE EMPTY", ipc_timeout_test_active_count() == 0U);
    report("SUPERVISOR AFTER / IDENTITY", supervisor_ping_ok());
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(pmm_stats().free_pages);
    terminal_putchar('\n');

    bool final_pass = pass && final_baseline && ipc_timeout_test_active_count() == 0U && supervisor_ping_ok();
    terminal_set_color(final_pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("LIFETIME / IPC ACCEPTANCE TEST: ");
    terminal_writeln(final_pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());

    if (g_acceptance.active) {
        terminal_writeln("LIFETIME / IPC ACCEPTANCE FIXTURE RETAINED. RUN r2finalcleanupretry; REBOOT IF RETRY FAILS.");
    }
}
