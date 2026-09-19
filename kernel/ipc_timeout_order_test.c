#include "ipc_timeout_order_test.h"

#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "ipc_timeout_test.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"

#define TIMEOUT_PAYLOAD_A 0x54494D454F555441ULL
#define TIMEOUT_PAYLOAD_B 0x54494D454F555442ULL
#define TIMEOUT_SHORT_TICKS 2ULL
#define TIMEOUT_LONG_TICKS 1000ULL
#define TIMEOUT_SPIN_LIMIT 200000000ULL

typedef enum {
    TIMEOUT_WORK_RECEIVE = 0,
    TIMEOUT_WORK_SEND
} TimeoutWork;

static struct {
    bool active;
    bool endpoint_created;
    bool worker_created;
    bool send_cap;
    bool receive_cap;
    TimeoutWork work;
    u64 timeout_ticks;
    volatile bool started;
    volatile bool returned;
    bool result;
    IpcMessage output;
    Process *kernel;
    Thread *main;
    Endpoint endpoint;
    Thread worker;
    CapabilityHandle send;
    CapabilityHandle receive;
    u64 frames;
    u64 kernel_threads;
    u64 supervisor_pid;
    u64 supervisor_tid;
    u32 thread_objects;
    u32 endpoint_objects;
    u32 tables;
    CapabilityTable caps_before;
} g_timeout;

static u64 timeout_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void timeout_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = timeout_irq_save();
    bool result = scheduler_yield();
    timeout_irq_restore(flags);
    return result;
}

static void report(const char *label, bool pass) {
    terminal_write("  ");
    terminal_write(label);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void fill(IpcMessage *message, u64 tag) {
    k_memset(message, 0, sizeof(*message));
    message->word_count = IPC_MESSAGE_MAX_WORDS;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = tag ^ (u64)i;
}

static bool matches(const IpcMessage *message, u64 tag) {
    if (!message || message->word_count != IPC_MESSAGE_MAX_WORDS) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        if (message->words[i] != (tag ^ (u64)i)) return false;
    }
    return true;
}

static bool output_zero(const IpcMessage *message) {
    if (!message || message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message->words[i]) return false;
    return true;
}

static bool ping(void) {
    u64 reply = 0;
    return supervisor_ping(0x54494D454F555450ULL, &reply) && reply == 0x54494D454F555450ULL;
}

static bool caps_unchanged(void) {
    CapabilityTable *now = process_capabilities(g_timeout.kernel);
    if (!now || now->count != g_timeout.caps_before.count) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *a = &g_timeout.caps_before.slots[i];
        const CapabilitySlot *b = &now->slots[i];
        if (a->occupied != b->occupied) return false;
        if (a->occupied && (a->object != b->object || a->object_id != b->object_id ||
            a->rights != b->rights || a->type != b->type || a->generation != b->generation)) return false;
    }
    return true;
}

static bool drop_caps(void) {
    CapabilityTable *caps = process_capabilities(g_timeout.kernel);
    if (!caps) return false;
    if (g_timeout.send_cap) {
        if (!capability_revoke(caps, g_timeout.send)) return false;
        g_timeout.send_cap = false;
    }
    if (g_timeout.receive_cap) {
        if (!capability_revoke(caps, g_timeout.receive)) return false;
        g_timeout.receive_cap = false;
    }
    return true;
}

static void timeout_worker(void *argument) {
    (void)argument;
    g_timeout.started = true;
    if (g_timeout.work == TIMEOUT_WORK_SEND) {
        IpcMessage message;
        fill(&message, TIMEOUT_PAYLOAD_B);
        g_timeout.result = ipc_send_blocking_for(g_timeout.kernel, g_timeout.send, &message, g_timeout.timeout_ticks);
    } else {
        k_memset(&g_timeout.output, 0xFF, sizeof(g_timeout.output));
        g_timeout.result = ipc_receive_blocking_for(g_timeout.kernel, g_timeout.receive,
            &g_timeout.output, g_timeout.timeout_ticks);
    }
    g_timeout.returned = true;
    scheduler_exit_current();
}

static bool start_case(TimeoutWork work, u64 timeout_ticks, bool prefill) {
    if (g_timeout.active || ipc_timeout_test_active_count() || !timer_initialized() ||
        scheduler_preemption_enabled() || scheduler_thread_count() != 1ULL) return false;
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    if (!kernel || !main || main->process != kernel || main->state != THREAD_STATE_RUNNING ||
        thread_wait_active(main) || !ping()) return false;

    k_memset(&g_timeout, 0, sizeof(g_timeout));
    g_timeout.active = true;
    g_timeout.work = work;
    g_timeout.timeout_ticks = timeout_ticks;
    g_timeout.kernel = kernel;
    g_timeout.main = main;
    g_timeout.frames = pmm_stats().free_pages;
    g_timeout.kernel_threads = process_thread_count(kernel);
    g_timeout.thread_objects = thread_object_count();
    g_timeout.endpoint_objects = endpoint_object_count();
    g_timeout.tables = capability_table_object_count();
    g_timeout.supervisor_pid = supervisor_process_id();
    g_timeout.supervisor_tid = supervisor_thread_id();

    CapabilityTable *caps = process_capabilities(kernel);
    if (!caps) return false;
    k_memcpy(&g_timeout.caps_before, caps, sizeof(*caps));
    if (!endpoint_create(&g_timeout.endpoint)) return false;
    g_timeout.endpoint_created = true;
    if (!capability_insert(caps, &g_timeout.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_timeout.send)) return false;
    g_timeout.send_cap = true;
    if (!capability_insert(caps, &g_timeout.endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_timeout.receive)) return false;
    g_timeout.receive_cap = true;

    if (prefill) {
        IpcMessage message;
        fill(&message, TIMEOUT_PAYLOAD_A);
        if (!ipc_try_send(kernel, g_timeout.send, &message)) return false;
    }

    if (!thread_create(&g_timeout.worker, kernel)) return false;
    g_timeout.worker_created = true;
    return thread_prepare_kernel(&g_timeout.worker, timeout_worker, 0) && scheduler_add(&g_timeout.worker);
}

static bool cleanup(void) {
    if (!g_timeout.active) return true;
    if (g_timeout.worker_created) {
        if (g_timeout.worker.state != THREAD_STATE_DEAD && !task_terminate_thread(&g_timeout.worker)) return false;
        if (!thread_destroy(&g_timeout.worker)) return false;
        g_timeout.worker_created = false;
    }
    if (ipc_timeout_test_active_count()) return false;
    if (g_timeout.endpoint_created) {
        if (g_timeout.endpoint.waiting_receiver || g_timeout.endpoint.waiting_receiver_id ||
            g_timeout.endpoint.waiting_sender || g_timeout.endpoint.waiting_sender_id ||
            g_timeout.endpoint.waiting_sender_message_ready) return false;
        if (g_timeout.endpoint.message_ready && !g_timeout.endpoint.closed) {
            IpcMessage discard;
            if (!endpoint_try_receive(&g_timeout.endpoint, &discard)) return false;
        }
    }
    if (!drop_caps()) return false;
    if (g_timeout.endpoint_created) {
        if (!endpoint_destroy(&g_timeout.endpoint)) return false;
        g_timeout.endpoint_created = false;
    }
    if (!caps_unchanged() || thread_current() != g_timeout.main || scheduler_thread_count() != 1ULL ||
        process_thread_count(g_timeout.kernel) != g_timeout.kernel_threads ||
        thread_object_count() != g_timeout.thread_objects || endpoint_object_count() != g_timeout.endpoint_objects ||
        capability_table_object_count() != g_timeout.tables || pmm_stats().free_pages != g_timeout.frames ||
        !ping() || supervisor_process_id() != g_timeout.supervisor_pid ||
        supervisor_thread_id() != g_timeout.supervisor_tid) return false;
    g_timeout.active = false;
    return true;
}

static bool blocked(ThreadWaitKind kind) {
    return g_timeout.started && !g_timeout.returned && g_timeout.worker.state == THREAD_STATE_BLOCKED &&
        !g_timeout.worker.on_run_queue && g_timeout.worker.wait_result == THREAD_WAIT_RESULT_PENDING &&
        thread_wait_matches_id(&g_timeout.worker, kind, &g_timeout.endpoint, g_timeout.worker.wait_id) &&
        ipc_timeout_test_active_count() == 1U && scheduler_thread_count() == 1ULL;
}

static bool wait_for_terminal(ThreadWaitResult result, u64 max_ticks) {
    u64 start = timer_ticks();
    for (u64 spins = 0; spins < TIMEOUT_SPIN_LIMIT; ++spins) {
        if (g_timeout.worker.wait_result == result) return true;
        if ((u64)(timer_ticks() - start) > max_ticks) break;
        __asm__ volatile ("pause");
    }
    return g_timeout.worker.wait_result == result;
}

static bool finish_case(const char *name, bool behavior) {
    bool released = cleanup();
    report(name, behavior && released);
    if (!released) terminal_writeln("TIMEOUT FIXTURE RETAINED. RUN timeoutcleanupretry; REBOOT IF RETRY FAILS.");
    return behavior && released;
}

static bool receive_timeout(void) {
    if (!start_case(TIMEOUT_WORK_RECEIVE, TIMEOUT_SHORT_TICKS, false) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_RECEIVE)) return false;
    if (!wait_for_terminal(THREAD_WAIT_RESULT_TIMED_OUT, TIMEOUT_SHORT_TICKS + 4ULL)) return false;
    bool ready = g_timeout.worker.state == THREAD_STATE_READY && g_timeout.worker.on_run_queue &&
        scheduler_thread_count() == 2ULL;
    IpcMessage late;
    fill(&late, TIMEOUT_PAYLOAD_B);
    bool late_rejected = !ipc_try_send(g_timeout.kernel, g_timeout.send, &late) &&
        g_timeout.worker.wait_result == THREAD_WAIT_RESULT_TIMED_OUT;
    if (!ready || !late_rejected || !schedule_once()) return false;
    bool returned = g_timeout.returned && !g_timeout.result && output_zero(&g_timeout.output) &&
        g_timeout.worker.state == THREAD_STATE_DEAD && !thread_wait_active(&g_timeout.worker) &&
        ipc_timeout_test_active_count() == 0U;
    bool late_after_release = ipc_try_send(g_timeout.kernel, g_timeout.send, &late);
    IpcMessage got;
    bool drained = late_after_release && ipc_try_receive(g_timeout.kernel, g_timeout.receive, &got) &&
        matches(&got, TIMEOUT_PAYLOAD_B);
    return returned && drained;
}

static bool ready_receive_timeout(void) {
    if (!start_case(TIMEOUT_WORK_RECEIVE, TIMEOUT_SHORT_TICKS, false) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_RECEIVE)) return false;
    IpcMessage message;
    fill(&message, TIMEOUT_PAYLOAD_B);
    if (!ipc_try_send(g_timeout.kernel, g_timeout.send, &message) ||
        g_timeout.worker.state != THREAD_STATE_READY ||
        g_timeout.worker.wait_result != THREAD_WAIT_RESULT_PENDING) return false;
    if (!wait_for_terminal(THREAD_WAIT_RESULT_TIMED_OUT, TIMEOUT_SHORT_TICKS + 4ULL)) return false;
    IpcMessage reserved;
    bool reserved_until_resume = !ipc_try_receive(g_timeout.kernel, g_timeout.receive, &reserved) &&
        scheduler_thread_count() == 2ULL;
    if (!reserved_until_resume || !schedule_once()) return false;
    bool returned = g_timeout.returned && !g_timeout.result && output_zero(&g_timeout.output) &&
        !thread_wait_active(&g_timeout.worker) && ipc_timeout_test_active_count() == 0U;
    return returned && ipc_try_receive(g_timeout.kernel, g_timeout.receive, &reserved) &&
        matches(&reserved, TIMEOUT_PAYLOAD_B);
}

static bool receive_completion_first(void) {
    if (!start_case(TIMEOUT_WORK_RECEIVE, TIMEOUT_LONG_TICKS, false) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_RECEIVE)) return false;
    IpcMessage message;
    fill(&message, TIMEOUT_PAYLOAD_B);
    if (!ipc_try_send(g_timeout.kernel, g_timeout.send, &message) || !schedule_once()) return false;
    bool completed = g_timeout.returned && g_timeout.result && matches(&g_timeout.output, TIMEOUT_PAYLOAD_B) &&
        g_timeout.worker.state == THREAD_STATE_DEAD && ipc_timeout_test_active_count() == 0U;
    ipc_timeout_poll(timer_ticks() + TIMEOUT_LONG_TICKS + 1ULL);
    return completed && ipc_timeout_test_active_count() == 0U;
}

static bool send_timeout(void) {
    if (!start_case(TIMEOUT_WORK_SEND, TIMEOUT_SHORT_TICKS, true) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_SEND)) return false;
    if (!wait_for_terminal(THREAD_WAIT_RESULT_TIMED_OUT, TIMEOUT_SHORT_TICKS + 4ULL)) return false;
    if (g_timeout.endpoint.waiting_sender_message_ready || g_timeout.worker.state != THREAD_STATE_READY ||
        scheduler_thread_count() != 2ULL) return false;
    IpcMessage original;
    bool original_only = ipc_try_receive(g_timeout.kernel, g_timeout.receive, &original) &&
        matches(&original, TIMEOUT_PAYLOAD_A) && !ipc_try_receive(g_timeout.kernel, g_timeout.receive, &original);
    if (!original_only || !schedule_once()) return false;
    return g_timeout.returned && !g_timeout.result && g_timeout.worker.state == THREAD_STATE_DEAD &&
        !thread_wait_active(&g_timeout.worker) && ipc_timeout_test_active_count() == 0U;
}

static bool send_completion_first(void) {
    if (!start_case(TIMEOUT_WORK_SEND, TIMEOUT_LONG_TICKS, true) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_SEND)) return false;
    IpcMessage original;
    if (!ipc_try_receive(g_timeout.kernel, g_timeout.receive, &original) ||
        !matches(&original, TIMEOUT_PAYLOAD_A) ||
        g_timeout.worker.wait_result != THREAD_WAIT_RESULT_COMPLETED ||
        g_timeout.worker.state != THREAD_STATE_READY) return false;
    ipc_timeout_poll(timer_ticks() + TIMEOUT_LONG_TICKS + 1ULL);
    if (g_timeout.worker.wait_result != THREAD_WAIT_RESULT_COMPLETED || !schedule_once()) return false;
    IpcMessage promoted;
    return g_timeout.returned && g_timeout.result && ipc_timeout_test_active_count() == 0U &&
        ipc_try_receive(g_timeout.kernel, g_timeout.receive, &promoted) && matches(&promoted, TIMEOUT_PAYLOAD_B);
}

static bool close_first(void) {
    if (!start_case(TIMEOUT_WORK_RECEIVE, TIMEOUT_LONG_TICKS, false) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_RECEIVE)) return false;
    if (!ipc_endpoint_close(&g_timeout.endpoint) ||
        g_timeout.worker.wait_result != THREAD_WAIT_RESULT_PEER_CLOSED) return false;
    ipc_timeout_poll(timer_ticks() + TIMEOUT_LONG_TICKS + 1ULL);
    if (g_timeout.worker.wait_result != THREAD_WAIT_RESULT_PEER_CLOSED || !schedule_once()) return false;
    return g_timeout.returned && !g_timeout.result && output_zero(&g_timeout.output) &&
        ipc_timeout_test_active_count() == 0U;
}

static bool timeout_first_close(void) {
    if (!start_case(TIMEOUT_WORK_RECEIVE, TIMEOUT_SHORT_TICKS, false) || !schedule_once() ||
        !blocked(THREAD_WAIT_IPC_RECEIVE)) return false;
    if (!wait_for_terminal(THREAD_WAIT_RESULT_TIMED_OUT, TIMEOUT_SHORT_TICKS + 4ULL)) return false;
    if (!ipc_endpoint_close(&g_timeout.endpoint) ||
        g_timeout.worker.wait_result != THREAD_WAIT_RESULT_TIMED_OUT || !schedule_once()) return false;
    return g_timeout.returned && !g_timeout.result && output_zero(&g_timeout.output) &&
        ipc_timeout_test_active_count() == 0U;
}

void ipc_timeout_order_cleanup_run(void) {
    terminal_writeln("IPC TIMEOUT CLEANUP RETRY:");
    if (!g_timeout.active) {
        report("NOTHING RETAINED", true);
        return;
    }
    report("RETAINED FIXTURE RELEASED", cleanup());
    terminal_writeln("ORIGINAL TEST WAS NOT RERUN.");
}

void ipc_timeout_order_test_run(void) {
    terminal_writeln("IPC TIMEOUT / DEADLINE TEST:");
    if (g_timeout.active || ipc_timeout_test_active_count()) {
        report("NO RETAINED FIXTURE / DEADLINE", false);
        return;
    }
    bool timer_ok = timer_initialized() && timer_frequency() != 0U;
    report("PIT MONOTONIC SOURCE", timer_ok);
    if (!timer_ok) return;

    u64 before = pmm_stats().free_pages;
    bool pass = finish_case("BLOCKED RECEIVE TIMEOUT / LATE REPLY", receive_timeout());
    if (pass) pass = finish_case("READY RECEIVE TIMEOUT / MESSAGE RETAINED", ready_receive_timeout());
    if (pass) pass = finish_case("RECEIVE COMPLETION WINS / LATE TIMEOUT", receive_completion_first());
    if (pass) pass = finish_case("UNCOMMITTED SEND TIMEOUT / STAGING DISCARDED", send_timeout());
    if (pass) pass = finish_case("COMMITTED SEND WINS / LATE TIMEOUT", send_completion_first());
    if (pass) pass = finish_case("PEER CLOSE WINS / LATE TIMEOUT", close_first());
    if (pass) pass = finish_case("TIMEOUT WINS / LATE CLOSE", timeout_first_close());

    u64 after = pmm_stats().free_pages;
    terminal_write("  FREE BEFORE: "); terminal_write_u64(before); terminal_putchar('\n');
    terminal_write("  FREE AFTER: "); terminal_write_u64(after); terminal_putchar('\n');
    bool baselines = pass && before == after && !g_timeout.active && ipc_timeout_test_active_count() == 0U;
    report("FRAME / DEADLINE / OWNERSHIP BASELINES", baselines);
    terminal_set_color(baselines ? terminal_accent_color() : terminal_error_color());
    terminal_write("IPC TIMEOUT / DEADLINE TEST: ");
    terminal_writeln(baselines ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
