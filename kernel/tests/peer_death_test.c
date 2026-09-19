#include "peer_death_test.h"
#include "test_output.h"

#include "address_space.h"
#include "arch.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"

#define PEER_TEST_RFLAGS_IF (1ULL << 9)

typedef enum {
    PEER_CASE_BLOCKED_RECEIVE = 0,
    PEER_CASE_READY_RECEIVE,
    PEER_CASE_BLOCKED_SEND,
    PEER_CASE_COMMITTED_SEND
} PeerCase;

typedef struct {
    Process service;
    Process client;
    Process replacement;
    Endpoint endpoint;
    Endpoint auxiliary;
    Thread client_thread;

    CapabilityHandle kernel_send;
    CapabilityHandle kernel_receive;
    CapabilityHandle client_handle;

    PeerCase test_case;
    IpcMessage returned_message;
    bool client_returned;
    bool client_result;

    bool service_live;
    bool client_live;
    bool replacement_live;
    bool endpoint_live;
    bool auxiliary_live;
    bool kernel_send_live;
    bool kernel_receive_live;
    bool client_thread_created;
    bool active;
} PeerFixture;

typedef struct {
    u64 frames;
    u64 threads;
    u64 processes;
    u64 spaces;
    u64 endpoints;
    u64 tables;
    u64 supervisor_pid;
    u64 supervisor_tid;
    u32 kernel_caps;
} PeerBaseline;

static PeerFixture g_peer;
static PeerBaseline g_baseline;

static bool check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static u64 peer_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void peer_irq_restore(u64 flags) {
    if (flags & PEER_TEST_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = peer_irq_save();
    bool result = scheduler_yield();
    peer_irq_restore(flags);
    return result;
}

static void fill_message(IpcMessage *message, u64 tag) {
    k_memset(message, 0, sizeof(*message));
    message->word_count = 4U;
    message->words[0] = 0x5045455244454154ULL;
    message->words[1] = tag;
    message->words[2] = tag ^ 0x1122334455667788ULL;
    message->words[3] = 0x4A434F5350324454ULL;
}

static bool message_matches(const IpcMessage *message, u64 tag) {
    return message && message->word_count == 4U &&
        message->words[0] == 0x5045455244454154ULL &&
        message->words[1] == tag &&
        message->words[2] == (tag ^ 0x1122334455667788ULL) &&
        message->words[3] == 0x4A434F5350324454ULL;
}

static bool message_zero(const IpcMessage *message) {
    if (!message || message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        if (message->words[i]) return false;
    }
    return true;
}

static bool supervisor_ok(void) {
    u64 cookie = 0x504545524C495645ULL;
    u64 reply = 0;
    return supervisor_process_id() == g_baseline.supervisor_pid &&
        supervisor_thread_id() == g_baseline.supervisor_tid &&
        supervisor_ping(cookie, &reply) && reply == cookie;
}

static bool persistent_baseline(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return caps && pmm_stats().free_pages == g_baseline.frames &&
        thread_object_count() == g_baseline.threads &&
        process_object_count() == g_baseline.processes &&
        address_space_object_count() == g_baseline.spaces &&
        endpoint_object_count() == g_baseline.endpoints &&
        capability_table_object_count() == g_baseline.tables &&
        capability_table_count(caps) == g_baseline.kernel_caps &&
        scheduler_thread_count() == 1ULL && supervisor_ok();
}

static void peer_worker(void *argument) {
    PeerFixture *fixture = (PeerFixture *)argument;
    if (!fixture || fixture != &g_peer) cpu_halt_forever();

    if (fixture->test_case == PEER_CASE_BLOCKED_RECEIVE ||
        fixture->test_case == PEER_CASE_READY_RECEIVE) {
        fixture->client_result = ipc_receive_blocking(
            &fixture->client,
            fixture->client_handle,
            &fixture->returned_message
        );
    } else {
        IpcMessage message;
        fill_message(&message, 0x53454E4400000000ULL | (u64)fixture->test_case);
        fixture->client_result = ipc_send_blocking(
            &fixture->client,
            fixture->client_handle,
            &message
        );
    }

    fixture->client_returned = true;
    scheduler_exit_current();
}

static bool cleanup_fixture(void) {
    if (!g_peer.active) return true;

    if (g_peer.service_live) {
        if (!task_quiesce_process(&g_peer.service)) return false;
        if (!process_destroy(&g_peer.service)) return false;
        g_peer.service_live = false;
    }

    if (g_peer.client_live) {
        if (!task_quiesce_process(&g_peer.client)) return false;
        g_peer.client_thread_created = false;
        if (!process_destroy(&g_peer.client)) return false;
        g_peer.client_live = false;
    }

    if (g_peer.replacement_live) {
        if (!task_quiesce_process(&g_peer.replacement)) return false;
        if (!process_destroy(&g_peer.replacement)) return false;
        g_peer.replacement_live = false;
    }

    CapabilityTable *kernel_caps = process_capabilities(process_kernel());
    if (!kernel_caps) return false;

    if (g_peer.kernel_send_live) {
        if (!capability_revoke(kernel_caps, g_peer.kernel_send)) return false;
        g_peer.kernel_send_live = false;
    }
    if (g_peer.kernel_receive_live) {
        if (!capability_revoke(kernel_caps, g_peer.kernel_receive)) return false;
        g_peer.kernel_receive_live = false;
    }

    if (g_peer.endpoint_live) {
        if (!endpoint_destroy(&g_peer.endpoint)) return false;
        g_peer.endpoint_live = false;
    }
    if (g_peer.auxiliary_live) {
        if (!endpoint_destroy(&g_peer.auxiliary)) return false;
        g_peer.auxiliary_live = false;
    }

    if (!thread_reclaim_unpublished_stack() || !address_space_reclaim_unpublished()) return false;

    g_peer.active = false;
    return persistent_baseline();
}

void peer_death_cleanup_run(void) {
    terminal_writeln("PEER DEATH CLEANUP RETRY:");
    if (!g_peer.active) {
        check("NOTHING RETAINED", true);
        return;
    }
    bool clean = cleanup_fixture();
    check("RETAINED FIXTURE RELEASED", clean);
    if (!clean) terminal_writeln("PEER FIXTURE RETAINED. REBOOT BEFORE FURTHER TESTS.");
    else terminal_writeln("ORIGINAL TEST WAS NOT RERUN.");
}

static bool setup_case(PeerCase test_case) {
    k_memset(&g_peer, 0, sizeof(g_peer));
    g_peer.active = true;
    g_peer.test_case = test_case;

    if (!process_create(&g_peer.service)) return false;
    g_peer.service_live = true;
    u64 service_id = g_peer.service.id;

    if (!process_create(&g_peer.client)) return false;
    g_peer.client_live = true;

    if (!endpoint_create_owned(&g_peer.endpoint, &g_peer.service)) return false;
    g_peer.endpoint_live = true;
    if (!endpoint_create_owned(&g_peer.auxiliary, &g_peer.service)) return false;
    g_peer.auxiliary_live = true;
    if (endpoint_owner_process_id(&g_peer.endpoint) != service_id ||
        endpoint_owner_process_id(&g_peer.auxiliary) != service_id) return false;

    CapabilityTable *kernel_caps = process_capabilities(process_kernel());
    CapabilityTable *client_caps = process_capabilities(&g_peer.client);
    if (!kernel_caps || !client_caps) return false;

    if (!capability_insert(kernel_caps, &g_peer.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_peer.kernel_send)) return false;
    g_peer.kernel_send_live = true;

    if (!capability_insert(kernel_caps, &g_peer.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_peer.kernel_receive)) return false;
    g_peer.kernel_receive_live = true;

    CapabilityRights client_right = (test_case == PEER_CASE_BLOCKED_RECEIVE ||
        test_case == PEER_CASE_READY_RECEIVE) ? CAPABILITY_RIGHT_RECEIVE : CAPABILITY_RIGHT_SEND;
    if (!capability_insert(client_caps, &g_peer.endpoint, CAPABILITY_TYPE_ENDPOINT,
            client_right, &g_peer.client_handle)) return false;

    if (!thread_create(&g_peer.client_thread, &g_peer.client)) return false;
    g_peer.client_thread_created = true;
    if (!thread_prepare_kernel(&g_peer.client_thread, peer_worker, &g_peer)) return false;
    if (!scheduler_add(&g_peer.client_thread)) return false;

    if (test_case == PEER_CASE_BLOCKED_SEND || test_case == PEER_CASE_COMMITTED_SEND) {
        IpcMessage prefill;
        fill_message(&prefill, 0x50524546494C4C31ULL);
        if (!ipc_try_send(process_kernel(), g_peer.kernel_send, &prefill)) return false;
    }

    if (!schedule_once()) return false;
    if (thread_current()->process != process_kernel() || g_peer.client_returned ||
        g_peer.client_thread.state != THREAD_STATE_BLOCKED || g_peer.client_thread.on_run_queue ||
        !thread_wait_active(&g_peer.client_thread)) return false;

    return true;
}

static bool run_case(PeerCase test_case, const char *label) {
    terminal_write(" "); terminal_write(label); terminal_writeln(":");
    if (!setup_case(test_case)) return false;

    u64 owner_id = endpoint_owner_process_id(&g_peer.endpoint);
    if (!owner_id || owner_id != g_peer.service.id) return false;

    if (!check("OPEN OWNER DESTROY REJECTED", !process_destroy(&g_peer.service))) return false;

    if (test_case == PEER_CASE_READY_RECEIVE) {
        IpcMessage wake;
        fill_message(&wake, 0x524541445957414BULL);
        if (!ipc_try_send(process_kernel(), g_peer.kernel_send, &wake) ||
            g_peer.client_thread.state != THREAD_STATE_READY || !g_peer.client_thread.on_run_queue ||
            g_peer.client_thread.wait_result != THREAD_WAIT_RESULT_PENDING) return false;
    }

    if (test_case == PEER_CASE_COMMITTED_SEND) {
        IpcMessage old;
        k_memset(&old, 0, sizeof(old));
        if (!ipc_try_receive(process_kernel(), g_peer.kernel_receive, &old) ||
            !message_matches(&old, 0x50524546494C4C31ULL) ||
            g_peer.client_thread.state != THREAD_STATE_READY || !g_peer.client_thread.on_run_queue ||
            g_peer.client_thread.wait_result != THREAD_WAIT_RESULT_COMPLETED) return false;
    }

    bool expected_success = test_case == PEER_CASE_COMMITTED_SEND;
    if (!check("PEER QUIESCE", task_quiesce_process(&g_peer.service))) return false;
    if (!check("ALL OWNED ENDPOINTS CLOSED", endpoint_closed(&g_peer.endpoint) &&
        endpoint_closed(&g_peer.auxiliary) &&
        endpoint_owner_process_id(&g_peer.endpoint) == owner_id &&
        endpoint_owner_process_id(&g_peer.auxiliary) == owner_id)) return false;

    ThreadWaitResult expected_result = expected_success ?
        THREAD_WAIT_RESULT_COMPLETED : THREAD_WAIT_RESULT_PEER_CLOSED;
    if (!check("TERMINAL OUTCOME", g_peer.client_thread.wait_result == expected_result &&
        g_peer.client_thread.state == THREAD_STATE_READY && g_peer.client_thread.on_run_queue)) return false;

    CapabilityTable *client_caps = process_capabilities(&g_peer.client);
    void *object = 0;
    if (!check("EXISTING CLIENT AUTHORITY RETAINED", client_caps &&
        capability_lookup(client_caps, g_peer.client_handle, CAPABILITY_TYPE_ENDPOINT, &object) &&
        object == &g_peer.endpoint)) return false;

    CapabilityHandle rejected = CAPABILITY_INVALID_HANDLE;
    if (!check("NO NEW AUTHORITY TO CLOSED PEER", !capability_insert(
            process_capabilities(process_kernel()), &g_peer.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &rejected) && rejected == CAPABILITY_INVALID_HANDLE)) return false;

    IpcMessage probe;
    fill_message(&probe, 0x434C4F5345444950ULL);
    IpcMessage out;
    k_memset(&out, 0, sizeof(out));
    bool closed_ipc_rejected = (test_case == PEER_CASE_BLOCKED_RECEIVE ||
        test_case == PEER_CASE_READY_RECEIVE)
        ? !ipc_try_receive(&g_peer.client, g_peer.client_handle, &out)
        : !ipc_try_send(&g_peer.client, g_peer.client_handle, &probe);
    if (!check("CLOSED IPC REJECTED", closed_ipc_rejected)) return false;

    if (!check("OWNER PROCESS DESTROY", process_destroy(&g_peer.service))) return false;
    g_peer.service_live = false;

    if (!process_create(&g_peer.replacement)) return false;
    g_peer.replacement_live = true;
    if (!check("OWNER INCARNATION NOT REUSED", g_peer.replacement.id != owner_id &&
        endpoint_owner_process_id(&g_peer.endpoint) == owner_id)) return false;
    if (!process_destroy(&g_peer.replacement)) return false;
    g_peer.replacement_live = false;

    if (!schedule_once()) return false;
    if (!check("CLIENT RESUMED ONCE", g_peer.client_returned &&
        g_peer.client_thread.state == THREAD_STATE_DEAD && !g_peer.client_thread.on_run_queue &&
        !thread_wait_active(&g_peer.client_thread))) return false;
    if (!check("CLIENT RESULT", g_peer.client_result == expected_success)) return false;
    if ((test_case == PEER_CASE_BLOCKED_RECEIVE || test_case == PEER_CASE_READY_RECEIVE) &&
        !check("RECEIVE OUTPUT CLEARED", message_zero(&g_peer.returned_message))) return false;
    if (!check("WAIT RESERVATION RELEASED", !endpoint_receiver_waiting(&g_peer.endpoint) &&
        !endpoint_sender_waiting(&g_peer.endpoint))) return false;

    bool clean = cleanup_fixture();
    return check("CASE CLEANUP / BASELINES", clean);
}

void peer_death_test_run(void) {
    terminal_writeln("PEER DEATH PROPAGATION TEST:");
    if (g_peer.active) {
        terminal_writeln("  RETAINED FIXTURE PRESENT: FAILED");
        terminal_writeln("  RUN peerdeathcleanupretry OR REBOOT.");
        return;
    }

    Thread *current = thread_current();
    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    bool quiet = current && kernel && kernel_caps && current->process == kernel &&
        current->state == THREAD_STATE_RUNNING && current->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();
    if (!check("MAIN THREAD", quiet)) return;

    g_baseline.frames = pmm_stats().free_pages;
    g_baseline.threads = thread_object_count();
    g_baseline.processes = process_object_count();
    g_baseline.spaces = address_space_object_count();
    g_baseline.endpoints = endpoint_object_count();
    g_baseline.tables = capability_table_object_count();
    g_baseline.kernel_caps = capability_table_count(kernel_caps);
    g_baseline.supervisor_pid = supervisor_process_id();
    g_baseline.supervisor_tid = supervisor_thread_id();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(g_baseline.frames); terminal_putchar('\n');
    if (!check("SUPERVISOR BEFORE", supervisor_ok())) return;

    bool passed = run_case(PEER_CASE_BLOCKED_RECEIVE, "BLOCKED RECEIVE") &&
        run_case(PEER_CASE_READY_RECEIVE, "READY RECEIVE") &&
        run_case(PEER_CASE_BLOCKED_SEND, "UNCOMMITTED SEND") &&
        run_case(PEER_CASE_COMMITTED_SEND, "COMMITTED SEND");

    bool baseline = !g_peer.active && persistent_baseline();
    check("PERSISTENT BASELINES", baseline);
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    test_output_final("PEER DEATH PROPAGATION TEST", passed && baseline);

    if (g_peer.active) {
        terminal_writeln("PEER FIXTURE RETAINED. RUN peerdeathcleanupretry; REBOOT IF RETRY FAILS.");
    }
}
