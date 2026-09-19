#include "ipc_wait_order_test.h"
#include "ipc_wait_test.h"
#include "task.h"
#include "scheduler.h"
#include "interrupts.h"
#include "terminal.h"
#include "supervisor.h"
#include "lib.h"
#include "pmm_test.h"

#define WAIT_PAYLOAD_A 0x1122334455667788ULL
#define WAIT_PAYLOAD_B 0x8877665544332211ULL

/* Durable ownership for the entire test, including a failed assertion/reap. */
static struct {
    bool active, endpoint_created, worker_created, send_cap, receive_cap;
    bool sender;
    u32 repeats;
    volatile bool started;
    volatile u32 returned;
    bool results[2];
    IpcMessage outputs[2];
    Process *kernel;
    Thread *main;
    Endpoint endpoint;
    Thread worker;
    CapabilityHandle send, receive;
    u64 frames, kernel_threads, supervisor_pid, supervisor_tid;
    u32 thread_objects, endpoint_objects, tables;
    CapabilityTable caps_before;
} g_wait;

static u64 save_irq(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void restore_irq(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }
static bool schedule_once(void) {
    u64 flags = save_irq();
    bool result = scheduler_yield();
    restore_irq(flags);
    return result;
}
static void report(const char *label, bool pass) {
    terminal_write("  "); terminal_write(label); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}
static void fill(IpcMessage *message, u64 tag) {
    k_memset(message, 0, sizeof(*message));
    message->word_count = IPC_MESSAGE_MAX_WORDS;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = tag ^ (u64)i;
}
static bool matches(const IpcMessage *message, u64 tag) {
    if (message->word_count != IPC_MESSAGE_MAX_WORDS) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        if (message->words[i] != (tag ^ (u64)i)) return false;
    }
    return true;
}
static bool empty_output(const IpcMessage *message) {
    if (message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message->words[i]) return false;
    return true;
}
static bool ping(void) {
    u64 reply = 0;
    return supervisor_ping(0x574149544F524445ULL, &reply) && reply == 0x574149544F524445ULL;
}
static bool caps_unchanged(void) {
    CapabilityTable *now = process_capabilities(g_wait.kernel);
    if (!now || now->count != g_wait.caps_before.count) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *a = &g_wait.caps_before.slots[i], *b = &now->slots[i];
        if (a->occupied != b->occupied) return false;
        if (a->occupied && (a->object != b->object || a->object_id != b->object_id ||
            a->rights != b->rights || a->type != b->type || a->generation != b->generation)) return false;
    }
    return true;
}
static bool drop_caps(void) {
    CapabilityTable *caps = process_capabilities(g_wait.kernel);
    if (g_wait.send_cap) {
        if (!capability_revoke(caps, g_wait.send)) return false;
        g_wait.send_cap = false;
    }
    if (g_wait.receive_cap) {
        if (!capability_revoke(caps, g_wait.receive)) return false;
        g_wait.receive_cap = false;
    }
    return true;
}
static void worker(void *argument) {
    (void)argument;
    g_wait.started = true;
    for (u32 i = 0; i < g_wait.repeats; ++i) {
        if (g_wait.sender) {
            IpcMessage message;
            fill(&message, WAIT_PAYLOAD_B);
            g_wait.results[i] = ipc_send_blocking(g_wait.kernel, g_wait.send, &message);
        } else {
            k_memset(&g_wait.outputs[i], 0xFF, sizeof(g_wait.outputs[i]));
            g_wait.results[i] = ipc_receive_blocking(g_wait.kernel, g_wait.receive, &g_wait.outputs[i]);
        }
        ++g_wait.returned;
    }
    scheduler_exit_current();
}
static bool start(bool sender, u32 repeats, bool publish) {
    if (g_wait.active || ipc_wait_test_observation().armed || thread_creation_cleanup_pending() ||
        pmm_test_free_failure_armed() || !repeats || repeats > 2U) return false;
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    if (!kernel || !main || main->process != kernel || main->state != THREAD_STATE_RUNNING ||
        thread_wait_active(main) || scheduler_preemption_enabled() || scheduler_thread_count() != 1ULL) return false;
    if (!ping()) return false;
    k_memset(&g_wait, 0, sizeof(g_wait));
    g_wait.active = true;
    g_wait.kernel = kernel;
    g_wait.main = main;
    g_wait.sender = sender;
    g_wait.repeats = repeats;
    g_wait.frames = pmm_stats().free_pages;
    g_wait.kernel_threads = process_thread_count(kernel);
    g_wait.thread_objects = thread_object_count();
    g_wait.endpoint_objects = endpoint_object_count();
    g_wait.tables = capability_table_object_count();
    g_wait.supervisor_pid = supervisor_process_id();
    g_wait.supervisor_tid = supervisor_thread_id();
    CapabilityTable *caps = process_capabilities(kernel);
    if (!caps) return false;
    k_memcpy(&g_wait.caps_before, caps, sizeof(*caps)); /* snapshot, never authority */
    if (!endpoint_create(&g_wait.endpoint)) return false;
    g_wait.endpoint_created = true;
    if (!capability_insert(caps, &g_wait.endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &g_wait.send)) return false;
    g_wait.send_cap = true;
    if (!capability_insert(caps, &g_wait.endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &g_wait.receive)) return false;
    g_wait.receive_cap = true;
    if (sender) {
        IpcMessage message;
        fill(&message, WAIT_PAYLOAD_A);
        if (!ipc_try_send(kernel, g_wait.send, &message)) return false;
    }
    if (!publish) return true;
    if (!thread_create(&g_wait.worker, kernel)) return false;
    g_wait.worker_created = true;
    return thread_prepare_kernel(&g_wait.worker, worker, 0) && scheduler_add(&g_wait.worker);
}
static bool cleanup(void) {
    if (!g_wait.active) return true;
    ipc_wait_test_reset();
    /* A failed local transition-gate assertion may retain main's reservation.
     * Never delete its objects or pretend ordinary cleanup resolved it. */
    if (thread_wait_active(g_wait.main)) return false;
    if (g_wait.worker_created) {
        if (g_wait.worker.state != THREAD_STATE_DEAD && !task_terminate_thread(&g_wait.worker)) return false;
        if (!thread_destroy(&g_wait.worker)) return false;
        g_wait.worker_created = false;
    }
    if (thread_creation_cleanup_pending() && !thread_reclaim_unpublished_stack()) return false;
    if (g_wait.endpoint_created) {
        Endpoint *ep = &g_wait.endpoint;
        if (ep->waiting_receiver || ep->waiting_sender || ep->waiting_receiver_id || ep->waiting_sender_id ||
            ep->waiting_sender_message_ready) return false;
        if (ep->message_ready) {
            IpcMessage discarded;
            if (!endpoint_try_receive(ep, &discarded)) return false;
        }
    }
    if (!drop_caps()) return false;
    if (g_wait.endpoint_created) {
        if (!endpoint_destroy(&g_wait.endpoint)) return false;
        g_wait.endpoint_created = false;
    }
    if (!caps_unchanged() || thread_current() != g_wait.main || scheduler_thread_count() != 1ULL ||
        process_thread_count(g_wait.kernel) != g_wait.kernel_threads || thread_object_count() != g_wait.thread_objects ||
        endpoint_object_count() != g_wait.endpoint_objects || capability_table_object_count() != g_wait.tables ||
        pmm_stats().free_pages != g_wait.frames || !ping() || supervisor_process_id() != g_wait.supervisor_pid ||
        supervisor_thread_id() != g_wait.supervisor_tid) return false;
    g_wait.active = false;
    return true;
}
static bool done_worker(bool result) {
    return thread_current() == g_wait.main && g_wait.worker.state == THREAD_STATE_DEAD &&
        !g_wait.worker.on_run_queue && !thread_wait_active(&g_wait.worker) &&
        g_wait.returned == g_wait.repeats && g_wait.results[g_wait.repeats - 1U] == result &&
        !g_wait.endpoint.waiting_receiver && !g_wait.endpoint.waiting_receiver_id &&
        !g_wait.endpoint.waiting_sender && !g_wait.endpoint.waiting_sender_id && scheduler_thread_count() == 1ULL;
}
static bool blocked(void) {
    ThreadWaitKind kind = g_wait.sender ? THREAD_WAIT_IPC_SEND : THREAD_WAIT_IPC_RECEIVE;
    return thread_current() == g_wait.main && g_wait.started && g_wait.worker.state == THREAD_STATE_BLOCKED &&
        !g_wait.worker.on_run_queue && g_wait.worker.wait_result == THREAD_WAIT_RESULT_PENDING &&
        thread_wait_matches_id(&g_wait.worker, kind, &g_wait.endpoint, g_wait.worker.wait_id) &&
        scheduler_thread_count() == 1ULL;
}
static bool deliver(void) {
    IpcMessage message;
    fill(&message, WAIT_PAYLOAD_B);
    return ipc_try_send(g_wait.kernel, g_wait.send, &message);
}
static bool promote(void) {
    IpcMessage message;
    return ipc_try_receive(g_wait.kernel, g_wait.receive, &message) && matches(&message, WAIT_PAYLOAD_A);
}

/* Exhaust all ordered pairs of the four supported terminal outcomes using
 * the actual transition gate. This is NOT exhaustive scheduling/SMP testing. */
static bool terminal_pairs(void) {
    if (!start(false, 1U, false)) return false;
    const ThreadWaitResult outcomes[4] = {
        THREAD_WAIT_RESULT_COMPLETED, THREAD_WAIT_RESULT_CANCELLED,
        THREAD_WAIT_RESULT_PEER_CLOSED, THREAD_WAIT_RESULT_TIMED_OUT
    };
    bool pass = true;
    u64 flags = save_irq();
    for (u32 a = 0; a < 4U && pass; ++a) {
        for (u32 b = 0; b < 4U && pass; ++b) {
            Thread *main = g_wait.main;
            if (!thread_wait_begin(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint)) { pass = false; break; }
            u64 id = main->wait_id;
            g_wait.endpoint.waiting_receiver = main;
            g_wait.endpoint.waiting_receiver_id = id;
            pass = !thread_wait_try_complete(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint, id, THREAD_WAIT_RESULT_PENDING) &&
                !thread_wait_try_complete(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint, id, THREAD_WAIT_RESULT_NONE) &&
                thread_wait_try_complete(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint, id, outcomes[a]) &&
                !thread_wait_try_complete(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint, id, outcomes[b]) &&
                main->wait_result == outcomes[a];
            if (main->wait_result == THREAD_WAIT_RESULT_PENDING) (void)thread_wait_cancel(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint);
            if (!thread_wait_end(main, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint)) pass = false;
            else {
                g_wait.endpoint.waiting_receiver = 0;
                g_wait.endpoint.waiting_receiver_id = 0;
            }
        }
    }
    restore_irq(flags);
    return pass && !thread_wait_active(g_wait.main);
}

static bool before_park(bool sender, bool close) {
    if (!start(sender, 1U, true)) return false;
    IpcMessage message;
    fill(&message, WAIT_PAYLOAD_B);
    IpcWaitTestAction action = close ? IPC_WAIT_TEST_CLOSE : (sender ? IPC_WAIT_TEST_RECEIVE : IPC_WAIT_TEST_SEND);
    if (!ipc_wait_test_arm_before_park(&g_wait.worker, &g_wait.endpoint, g_wait.kernel,
            sender ? g_wait.receive : g_wait.send, action, &message)) return false;
    u64 parks = ipc_wait_test_park_attempts();
    if (!schedule_once()) return false;
    IpcWaitTestObservation obs = ipc_wait_test_observation();
    ThreadWaitResult expected = close ? THREAD_WAIT_RESULT_PEER_CLOSED :
        (sender ? THREAD_WAIT_RESULT_COMPLETED : THREAD_WAIT_RESULT_PENDING);
    return obs.fired && !obs.armed && obs.succeeded && obs.wait_id && obs.state == THREAD_STATE_RUNNING &&
        obs.result == expected && ipc_wait_test_park_attempts() == parks && done_worker(!close) &&
        (sender || (close ? empty_output(&g_wait.outputs[0]) : matches(&g_wait.outputs[0], WAIT_PAYLOAD_B)));
}

static bool spurious(bool sender) {
    if (!start(sender, 1U, true) || !schedule_once() || !blocked()) return false;
    u64 id = g_wait.worker.wait_id;
    IpcMessage competing;
    fill(&competing, WAIT_PAYLOAD_B);
    bool contender = sender ? ipc_send_blocking(g_wait.kernel, g_wait.send, &competing) :
        ipc_receive_blocking(g_wait.kernel, g_wait.receive, &competing);
    if (contender || g_wait.worker.wait_id != id || !blocked()) return false;
    if (!scheduler_wake(&g_wait.worker) || scheduler_wake(&g_wait.worker) || scheduler_thread_count() != 2ULL) return false;
    if (!schedule_once() || !blocked() || g_wait.returned || g_wait.worker.wait_id != id) return false;
    if (!(sender ? promote() : deliver())) return false;
    if (scheduler_wake(&g_wait.worker) || scheduler_thread_count() != 2ULL) return false;
    return schedule_once() && done_worker(true) && (sender || matches(&g_wait.outputs[0], WAIT_PAYLOAD_B));
}

static bool no_caps(bool sender, bool make_ready) {
    if (!start(sender, 1U, true) || !schedule_once() || !blocked()) return false;
    u64 id = g_wait.worker.wait_id;
    if (make_ready && !(sender ? promote() : deliver())) return false;
    if (!drop_caps() || g_wait.endpoint.capability_refs || endpoint_destroy(&g_wait.endpoint) ||
        endpoint_create(&g_wait.endpoint)) return false;
    if (!ipc_endpoint_close(&g_wait.endpoint) || ipc_endpoint_close(&g_wait.endpoint) ||
        scheduler_wake(&g_wait.worker) || scheduler_thread_count() != 2ULL) return false;
    ThreadWaitResult expected = sender && make_ready ? THREAD_WAIT_RESULT_COMPLETED : THREAD_WAIT_RESULT_PEER_CLOSED;
    ThreadWaitKind kind = sender ? THREAD_WAIT_IPC_SEND : THREAD_WAIT_IPC_RECEIVE;
    if (g_wait.worker.wait_result != expected || g_wait.worker.wait_id != id ||
        thread_wait_try_complete(&g_wait.worker, kind, &g_wait.endpoint, id, THREAD_WAIT_RESULT_CANCELLED) ||
        thread_wait_try_complete(&g_wait.worker, kind, &g_wait.endpoint, id, THREAD_WAIT_RESULT_COMPLETED) ||
        endpoint_destroy(&g_wait.endpoint)) return false;
    if (!schedule_once() || !done_worker(sender && make_ready)) return false;
    return sender || empty_output(&g_wait.outputs[0]);
}

static bool kill_ready(bool sender) {
    if (!start(sender, 1U, true) || !schedule_once() || !blocked() || !(sender ? promote() : deliver())) return false;
    if (!task_terminate_thread(&g_wait.worker) || task_terminate_thread(&g_wait.worker) ||
        g_wait.worker.state != THREAD_STATE_DEAD || thread_wait_active(&g_wait.worker) || g_wait.returned ||
        g_wait.endpoint.waiting_receiver || g_wait.endpoint.waiting_receiver_id ||
        g_wait.endpoint.waiting_sender || g_wait.endpoint.waiting_sender_id || scheduler_thread_count() != 1ULL) return false;
    IpcMessage received;
    return ipc_try_receive(g_wait.kernel, g_wait.receive, &received) && matches(&received, WAIT_PAYLOAD_B) &&
        !ipc_try_receive(g_wait.kernel, g_wait.receive, &received);
}

static bool stale_identity(void) {
    if (!start(false, 2U, true) || !schedule_once() || !blocked()) return false;
    u64 old_id = g_wait.worker.wait_id;
    if (!deliver() || !schedule_once() || !blocked() || g_wait.returned != 1U || !g_wait.results[0]) return false;
    u64 new_id = g_wait.worker.wait_id;
    if (!new_id || new_id == old_id || g_wait.endpoint.waiting_receiver_id != new_id ||
        thread_wait_try_complete(&g_wait.worker, THREAD_WAIT_IPC_RECEIVE, &g_wait.endpoint, old_id, THREAD_WAIT_RESULT_COMPLETED) ||
        g_wait.worker.wait_result != THREAD_WAIT_RESULT_PENDING) return false;
    /* A stale endpoint-side event token is also rejected before closure mutates. */
    g_wait.endpoint.waiting_receiver_id = old_id;
    bool rejected = !ipc_endpoint_close(&g_wait.endpoint) && !g_wait.endpoint.closed &&
        g_wait.worker.wait_result == THREAD_WAIT_RESULT_PENDING && scheduler_thread_count() == 1ULL;
    g_wait.endpoint.waiting_receiver_id = new_id;
    return rejected && deliver() && schedule_once() && done_worker(true) &&
        matches(&g_wait.outputs[0], WAIT_PAYLOAD_B) && matches(&g_wait.outputs[1], WAIT_PAYLOAD_B);
}

static bool finish_case(const char *name, bool behavior) {
    bool released = cleanup();
    report(name, behavior && released);
    if (!released) {
        terminal_writeln("WAIT FIXTURE RETAINED. RUN waitcleanupretry; REBOOT IF RETRY FAILS.");
    }
    return behavior && released;
}

void ipc_wait_order_cleanup_run(void) {
    terminal_writeln("IPC WAIT CLEANUP RETRY:");
    report("RETAINED FIXTURE RELEASED", cleanup());
    terminal_writeln("ORIGINAL TEST WAS NOT RERUN.");
}

void ipc_wait_order_test_run(void) {
    terminal_writeln("BOUNDED IPC WAIT ORDER TEST:");
    if (g_wait.active || ipc_wait_test_observation().armed) {
        report("NO RETAINED FIXTURE / HOOK", false);
        return;
    }
    u64 before = pmm_stats().free_pages;
    bool pass = finish_case("TERMINAL RESULT PAIRS (16)", terminal_pairs());
    if (pass) pass = finish_case("RECEIVE BEFORE PARK / NO LOST WAKE", before_park(false, false));
    if (pass) pass = finish_case("CLOSE RECEIVE BEFORE PARK", before_park(false, true));
    if (pass) pass = finish_case("SEND COMMIT BEFORE PARK", before_park(true, false));
    if (pass) pass = finish_case("CLOSE SEND BEFORE PARK", before_park(true, true));
    if (pass) pass = finish_case("SPURIOUS RECEIVE WAKE / SAME WAIT", spurious(false));
    if (pass) pass = finish_case("SPURIOUS SEND WAKE / SAME WAIT", spurious(true));
    if (pass) pass = finish_case("LAST CAP / BLOCKED RECEIVE / CLOSE", no_caps(false, false));
    if (pass) pass = finish_case("LAST CAP / READY RECEIVE / CLOSE", no_caps(false, true));
    if (pass) pass = finish_case("LAST CAP / UNCOMMITTED SEND / CLOSE", no_caps(true, false));
    if (pass) pass = finish_case("LAST CAP / COMMITTED SEND / CLOSE", no_caps(true, true));
    if (pass) pass = finish_case("KILL READY RECEIVE / MESSAGE RETAINED", kill_ready(false));
    if (pass) pass = finish_case("KILL COMMITTED SEND / MESSAGE RETAINED", kill_ready(true));
    if (pass) pass = finish_case("OLD WAIT ID CANNOT COMPLETE NEXT WAIT", stale_identity());
    /* R4.1A deliberately changed sole-runnable blocking IPC from immediate
     * park rollback to idle-and-recheck. The timed form is covered by
     * scheduler-idle; an untimed wait with no producer would correctly wait. */
    u64 after = pmm_stats().free_pages;
    terminal_write("  FREE BEFORE: "); terminal_write_u64(before); terminal_putchar('\n');
    terminal_write("  FREE AFTER: "); terminal_write_u64(after); terminal_putchar('\n');
    report("FRAME / OWNERSHIP BASELINES", before == after && !g_wait.active);
    terminal_set_color(pass && before == after ? terminal_accent_color() : terminal_error_color());
    terminal_write("BOUNDED IPC WAIT ORDER TEST: "); terminal_writeln(pass && before == after ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
