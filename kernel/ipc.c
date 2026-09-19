#include "ipc.h"
#include "ipc_wait_test.h"
#include "ipc_timeout_test.h"
#include "scheduler.h"
#include "thread.h"
#include "interrupts.h"
#include "arch.h"
#include "lib.h"
#include "timer.h"

#define IPC_RFLAGS_IF (1ULL << 9)

static u64 ipc_interrupt_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void ipc_interrupt_restore(u64 flags) { if (flags & IPC_RFLAGS_IF) interrupts_enable(); }

static bool ipc_message_valid(const IpcMessage *message) {
    return message && message->word_count && message->word_count <= IPC_MESSAGE_MAX_WORDS;
}

static bool ipc_endpoint_valid(const Endpoint *endpoint) {
    /* Validate canonical storage before dereferencing a supplied object pointer. */
    return endpoint_storage_in_use(endpoint) && endpoint->initialized && endpoint->id;
}

static bool ipc_result_valid(ThreadWaitResult result) {
    return result == THREAD_WAIT_RESULT_PENDING || result == THREAD_WAIT_RESULT_COMPLETED ||
        result == THREAD_WAIT_RESULT_CANCELLED || result == THREAD_WAIT_RESULT_PEER_CLOSED ||
        result == THREAD_WAIT_RESULT_TIMED_OUT;
}

#define IPC_TIMED_WAIT_CAPACITY (ENDPOINT_STORAGE_CAPACITY * 2U)
#define IPC_TIMEOUT_MAX_TICKS 0x7FFFFFFFFFFFFFFFULL

typedef struct {
    Thread *thread;
    Endpoint *endpoint;
    u64 thread_id;
    u64 endpoint_id;
    u64 wait_id;
    u64 deadline;
    ThreadWaitKind kind;
    bool occupied;
} IpcTimedWait;

/* Fixed bring-up table: at most one receiver and one sender wait per endpoint. */
static IpcTimedWait g_timed_waits[IPC_TIMED_WAIT_CAPACITY];

static bool ipc_deadline_reached(u64 now, u64 deadline) {
    return (s64)(now - deadline) >= 0;
}

static bool ipc_timeout_register(Thread *thread, Endpoint *endpoint, ThreadWaitKind kind, u64 id, u64 ticks) {
    if (!timer_initialized() || !ticks || ticks > IPC_TIMEOUT_MAX_TICKS ||
        !thread_wait_matches_id(thread, kind, endpoint, id)) return false;

    u32 empty = IPC_TIMED_WAIT_CAPACITY;
    for (u32 i = 0; i < IPC_TIMED_WAIT_CAPACITY; ++i) {
        IpcTimedWait *slot = &g_timed_waits[i];
        if (!slot->occupied) {
            if (empty == IPC_TIMED_WAIT_CAPACITY) empty = i;
            continue;
        }
        /* A Thread can own only one wait. Duplicate registration is an invariant violation. */
        if (slot->thread == thread ||
            (slot->endpoint == endpoint && slot->kind == kind && slot->wait_id == id)) return false;
    }
    if (empty == IPC_TIMED_WAIT_CAPACITY) return false;

    IpcTimedWait *slot = &g_timed_waits[empty];
    k_memset(slot, 0, sizeof(*slot));
    slot->thread = thread;
    slot->endpoint = endpoint;
    slot->thread_id = thread->id;
    slot->endpoint_id = endpoint->id;
    slot->wait_id = id;
    slot->deadline = timer_ticks() + ticks;
    slot->kind = kind;
    slot->occupied = true;
    return true;
}

static bool ipc_timeout_unregister(Thread *thread, Endpoint *endpoint, ThreadWaitKind kind, u64 id) {
    for (u32 i = 0; i < IPC_TIMED_WAIT_CAPACITY; ++i) {
        IpcTimedWait *slot = &g_timed_waits[i];
        if (!slot->occupied) continue;
        if (slot->thread == thread && slot->endpoint == endpoint && slot->kind == kind && slot->wait_id == id) {
            k_memset(slot, 0, sizeof(*slot));
            return true;
        }
    }
    return false;
}

u32 ipc_timeout_test_active_count(void) {
    u64 flags = ipc_interrupt_save();
    u32 count = 0;
    for (u32 i = 0; i < IPC_TIMED_WAIT_CAPACITY; ++i) if (g_timed_waits[i].occupied) ++count;
    ipc_interrupt_restore(flags);
    return count;
}

/* Caller holds local IRQ exclusion. Endpoint links and operation IDs retain
 * ownership independently of capability slots, including READY-before-resume. */
static bool ipc_reservation_valid(const Endpoint *endpoint, ThreadWaitKind kind) {
    Thread *thread = kind == THREAD_WAIT_IPC_RECEIVE ? endpoint->waiting_receiver : endpoint->waiting_sender;
    u64 id = kind == THREAD_WAIT_IPC_RECEIVE ? endpoint->waiting_receiver_id : endpoint->waiting_sender_id;
    if (!thread) return id == 0;
    if (!thread_wait_matches_id(thread, kind, endpoint, id) || !ipc_result_valid(thread->wait_result)) return false;
    if (thread == thread_current() && thread->state == THREAD_STATE_RUNNING) {
        return thread->on_run_queue && thread->run_next;
    }
    if (!thread->interrupt_context_ready || !thread->interrupt_rsp) return false;
    if (thread->state == THREAD_STATE_BLOCKED) return !thread->on_run_queue && !thread->run_next;
    if (thread->state == THREAD_STATE_READY) return thread->on_run_queue && thread->run_next;
    return false;
}

static Endpoint *ipc_resolve_endpoint(Process *process, CapabilityHandle handle, CapabilityRights rights) {
    CapabilityTable *caps = process_capabilities(process);
    void *object = 0;
    if (!caps || !capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT, rights, &object)) return 0;
    return ipc_endpoint_valid((Endpoint *)object) ? (Endpoint *)object : 0;
}

/* A pre-park event needs no enqueue. A spurious wake may already have made the
 * thread READY. Both cases must avoid duplicate runnable insertion. */
static bool ipc_make_ready(Thread *thread) {
    if (thread->state == THREAD_STATE_BLOCKED) return scheduler_wake(thread);
    return (thread == thread_current() && thread->state == THREAD_STATE_RUNNING && thread->on_run_queue) ||
        (thread->state == THREAD_STATE_READY && thread->on_run_queue);
}

static void ipc_finish(Thread *thread, Endpoint *endpoint, ThreadWaitKind kind,
    u64 id, ThreadWaitResult result) {
    /* Callers have validated the reservation and performed any operation side
     * effects under the same exclusion. Failure here is a kernel invariant error. */
    if (!thread_wait_try_complete(thread, kind, endpoint, id, result)) cpu_halt_forever();
}

void ipc_timeout_poll(u64 now_ticks) {
    u64 flags = ipc_interrupt_save();
    for (u32 i = 0; i < IPC_TIMED_WAIT_CAPACITY; ++i) {
        IpcTimedWait *slot = &g_timed_waits[i];
        if (!slot->occupied) continue;

        Thread *thread = slot->thread;
        Endpoint *endpoint = slot->endpoint;
        if (!thread_storage_in_use(thread) || !ipc_endpoint_valid(endpoint) ||
            thread->id != slot->thread_id || endpoint->id != slot->endpoint_id ||
            !thread_wait_matches_id(thread, slot->kind, endpoint, slot->wait_id) ||
            !ipc_reservation_valid(endpoint, slot->kind)) cpu_halt_forever();

        /* Another event already won. Keep the registration until the exact
         * continuation consumes its terminal result or forced abort releases it. */
        if (thread->wait_result != THREAD_WAIT_RESULT_PENDING) continue;
        if (!ipc_deadline_reached(now_ticks, slot->deadline)) continue;

        if (slot->kind == THREAD_WAIT_IPC_SEND) {
            /* No staged message means SEND must already have committed. */
            if (!endpoint->waiting_sender_message_ready) cpu_halt_forever();
            ipc_finish(thread, endpoint, slot->kind, slot->wait_id, THREAD_WAIT_RESULT_TIMED_OUT);
            k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
            endpoint->waiting_sender_message_ready = false;
        } else if (slot->kind == THREAD_WAIT_IPC_RECEIVE) {
            ipc_finish(thread, endpoint, slot->kind, slot->wait_id, THREAD_WAIT_RESULT_TIMED_OUT);
        } else cpu_halt_forever();

        if (!ipc_make_ready(thread)) cpu_halt_forever();
    }
    ipc_interrupt_restore(flags);
}

/* Interrupts must already be disabled. A RECEIVE wake is a predicate hint, not
 * terminal success: its continuation must still consume the reserved message. */
static bool ipc_send_locked(Endpoint *endpoint, const IpcMessage *message) {
    if (!ipc_endpoint_valid(endpoint) || !ipc_message_valid(message) || endpoint->closed ||
        endpoint_message_ready(endpoint) || !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_RECEIVE)) return false;
    Thread *receiver = endpoint->waiting_receiver;
    if (receiver && receiver->wait_result != THREAD_WAIT_RESULT_PENDING) return false;
    if (!endpoint_try_send(endpoint, message)) return false;
    if (receiver && !ipc_make_ready(receiver)) {
        IpcMessage discarded;
        if (!endpoint_try_receive(endpoint, &discarded)) cpu_halt_forever();
        return false;
    }
    return true;
}

static bool ipc_receive_locked(Endpoint *endpoint, Thread *current, IpcMessage *out_message) {
    if (!ipc_endpoint_valid(endpoint) || !out_message || endpoint->closed ||
        !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_RECEIVE) ||
        !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_SEND)) return false;
    Thread *receiver = endpoint->waiting_receiver;
    if (receiver && (receiver != current || receiver->wait_result != THREAD_WAIT_RESULT_PENDING)) return false;
    if (!endpoint_message_ready(endpoint)) return false;

    Thread *sender = endpoint->waiting_sender;
    bool staged = endpoint->waiting_sender_message_ready;
    if (staged && (!sender || sender->wait_result != THREAD_WAIT_RESULT_PENDING)) return false;
    if (sender && sender->wait_result == THREAD_WAIT_RESULT_PENDING && !staged) return false;

    IpcMessage received;
    k_memset(&received, 0, sizeof(received));
    if (!endpoint_try_receive(endpoint, &received)) return false;
    if (staged) {
        IpcMessage pending = endpoint->waiting_sender_message;
        if (!endpoint_try_send(endpoint, &pending)) {
            if (!endpoint_try_send(endpoint, &received)) cpu_halt_forever();
            return false;
        }
        if (!ipc_make_ready(sender)) {
            IpcMessage discarded;
            if (!endpoint_try_receive(endpoint, &discarded) || !endpoint_try_send(endpoint, &received)) cpu_halt_forever();
            return false;
        }
        /* Acceptance into the mailbox commits SEND. Peer consumption is not
         * promised; later closure may discard the mailbox but cannot undo this. */
        ipc_finish(sender, endpoint, THREAD_WAIT_IPC_SEND, endpoint->waiting_sender_id, THREAD_WAIT_RESULT_COMPLETED);
        k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
        endpoint->waiting_sender_message_ready = false;
    }
    *out_message = received;
    return true;
}

bool ipc_try_send(Process *process, CapabilityHandle handle, const IpcMessage *message) {
    if (!process || !ipc_message_valid(message)) return false;
    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_SEND);
    bool sent = endpoint && ipc_send_locked(endpoint, message);
    ipc_interrupt_restore(flags);
    return sent;
}

bool ipc_try_receive(Process *process, CapabilityHandle handle, IpcMessage *out_message) {
    if (!out_message) return false;
    k_memset(out_message, 0, sizeof(*out_message));
    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_RECEIVE);
    bool received = endpoint && ipc_receive_locked(endpoint, thread_current(), out_message);
    ipc_interrupt_restore(flags);
    return received;
}

static struct {
    Thread *thread;
    u64 thread_id;
    Endpoint *endpoint;
    u64 endpoint_id;
    Process *peer;
    CapabilityHandle handle;
    IpcWaitTestAction action;
    IpcMessage message;
    IpcWaitTestObservation observation;
} g_before_park;
static u64 g_park_attempts;

bool ipc_wait_test_arm_before_park(Thread *thread, Endpoint *endpoint, Process *peer,
    CapabilityHandle handle, IpcWaitTestAction action, const IpcMessage *message) {
    u64 flags = ipc_interrupt_save();
    bool armed = false;
    if (g_before_park.observation.armed || !thread_storage_in_use(thread) ||
        thread == thread_current() || thread->state != THREAD_STATE_READY || thread_wait_active(thread) ||
        !ipc_endpoint_valid(endpoint) || endpoint->closed) goto done;
    if (action != IPC_WAIT_TEST_SEND && action != IPC_WAIT_TEST_RECEIVE && action != IPC_WAIT_TEST_CLOSE) goto done;
    if (action == IPC_WAIT_TEST_SEND && !ipc_message_valid(message)) goto done;
    if (action != IPC_WAIT_TEST_CLOSE && !process_capabilities(peer)) goto done;
    k_memset(&g_before_park, 0, sizeof(g_before_park));
    g_before_park.thread = thread;
    g_before_park.thread_id = thread->id;
    g_before_park.endpoint = endpoint;
    g_before_park.endpoint_id = endpoint->id;
    g_before_park.peer = peer;
    g_before_park.handle = handle;
    g_before_park.action = action;
    if (message) g_before_park.message = *message;
    g_before_park.observation.armed = true;
    armed = true;
done:
    ipc_interrupt_restore(flags);
    return armed;
}

void ipc_wait_test_reset(void) {
    u64 flags = ipc_interrupt_save();
    k_memset(&g_before_park, 0, sizeof(g_before_park));
    ipc_interrupt_restore(flags);
}

IpcWaitTestObservation ipc_wait_test_observation(void) {
    u64 flags = ipc_interrupt_save();
    IpcWaitTestObservation result = g_before_park.observation;
    ipc_interrupt_restore(flags);
    return result;
}

u64 ipc_wait_test_park_attempts(void) { return g_park_attempts; }

static void ipc_before_park(Thread *thread, Endpoint *endpoint) {
    if (!g_before_park.observation.armed || g_before_park.thread != thread ||
        g_before_park.endpoint != endpoint) return;
    g_before_park.observation.armed = false;
    g_before_park.observation.fired = true;
    if (g_before_park.thread_id != thread->id || g_before_park.endpoint_id != endpoint->id) return;
    g_before_park.observation.wait_id = thread->wait_id;
    bool result = false;
    IpcMessage discarded;
    switch (g_before_park.action) {
        case IPC_WAIT_TEST_SEND:
            result = ipc_try_send(g_before_park.peer, g_before_park.handle, &g_before_park.message);
            break;
        case IPC_WAIT_TEST_RECEIVE:
            result = ipc_try_receive(g_before_park.peer, g_before_park.handle, &discarded);
            break;
        case IPC_WAIT_TEST_CLOSE:
            result = ipc_endpoint_close(endpoint);
            break;
        default:
            break;
    }
    g_before_park.observation.succeeded = result;
    g_before_park.observation.result = thread->wait_result;
    g_before_park.observation.state = thread->state;
    /* Do not retain borrowed pointers after consuming the one-shot hook. */
    g_before_park.thread = 0;
    g_before_park.endpoint = 0;
    g_before_park.peer = 0;
}

static void ipc_end_reservation(Thread *current, Endpoint *endpoint, ThreadWaitKind kind, u64 id,
    bool timeout_registered) {
    if (!thread_wait_matches_id(current, kind, endpoint, id) || !ipc_reservation_valid(endpoint, kind)) cpu_halt_forever();
    if (timeout_registered && !ipc_timeout_unregister(current, endpoint, kind, id)) cpu_halt_forever();
    /* No caller dereferences endpoint after returning from this release helper.
     * Local exclusion covers both sides; capability count may already be zero. */
    if (!thread_wait_end(current, kind, endpoint)) cpu_halt_forever();
    if (kind == THREAD_WAIT_IPC_RECEIVE) {
        endpoint->waiting_receiver = 0;
        endpoint->waiting_receiver_id = 0;
    } else {
        endpoint->waiting_sender = 0;
        endpoint->waiting_sender_id = 0;
        k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
        endpoint->waiting_sender_message_ready = false;
    }
}

static bool ipc_receive_blocking_common(Process *process, CapabilityHandle handle, IpcMessage *out_message,
    bool timed, u64 timeout_ticks) {
    if (!out_message || (timed && (!timeout_ticks || timeout_ticks > IPC_TIMEOUT_MAX_TICKS || !timer_initialized()))) return false;
    k_memset(out_message, 0, sizeof(*out_message));
    Thread *current = thread_current();
    if (!process || !current || !current->id || current->process != process) return false;
    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_RECEIVE);
    if (!endpoint || endpoint->closed) { ipc_interrupt_restore(flags); return false; }
    if (endpoint_message_ready(endpoint)) {
        bool received = ipc_receive_locked(endpoint, current, out_message);
        ipc_interrupt_restore(flags);
        return received;
    }
    if (endpoint->waiting_receiver || endpoint->waiting_receiver_id ||
        !thread_wait_begin(current, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }
    u64 id = current->wait_id;
    endpoint->waiting_receiver = current;
    endpoint->waiting_receiver_id = id;
    if (timed && !ipc_timeout_register(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id, timeout_ticks)) {
        ipc_finish(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id, THREAD_WAIT_RESULT_CANCELLED);
        ipc_end_reservation(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id, false);
        ipc_interrupt_restore(flags);
        return false;
    }
    ipc_before_park(current, endpoint);
    for (;;) {
        if (!thread_wait_matches_id(current, THREAD_WAIT_IPC_RECEIVE, endpoint, id) ||
            !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_RECEIVE)) cpu_halt_forever();
        if (current->wait_result != THREAD_WAIT_RESULT_PENDING) break;
        if (endpoint_message_ready(endpoint)) {
            bool received = ipc_receive_locked(endpoint, current, out_message);
            ipc_finish(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id,
                received ? THREAD_WAIT_RESULT_COMPLETED : THREAD_WAIT_RESULT_CANCELLED);
            break;
        }
        ++g_park_attempts;
        if (!scheduler_block_current()) {
            ipc_finish(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id, THREAD_WAIT_RESULT_CANCELLED);
            break;
        }
    }
    bool received = current->wait_result == THREAD_WAIT_RESULT_COMPLETED;
    ipc_end_reservation(current, endpoint, THREAD_WAIT_IPC_RECEIVE, id, timed);
    ipc_interrupt_restore(flags);
    return received;
}

bool ipc_receive_blocking(Process *process, CapabilityHandle handle, IpcMessage *out_message) {
    return ipc_receive_blocking_common(process, handle, out_message, false, 0);
}

bool ipc_receive_blocking_for(Process *process, CapabilityHandle handle, IpcMessage *out_message, u64 timeout_ticks) {
    return ipc_receive_blocking_common(process, handle, out_message, true, timeout_ticks);
}

static bool ipc_send_blocking_common(Process *process, CapabilityHandle handle, const IpcMessage *message,
    bool timed, u64 timeout_ticks) {
    if (!process || !ipc_message_valid(message) ||
        (timed && (!timeout_ticks || timeout_ticks > IPC_TIMEOUT_MAX_TICKS || !timer_initialized()))) return false;
    Thread *current = thread_current();
    if (!current || !current->id || current->process != process) return false;
    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_SEND);
    if (!endpoint || endpoint->closed) { ipc_interrupt_restore(flags); return false; }
    if (!endpoint_message_ready(endpoint)) {
        bool sent = ipc_send_locked(endpoint, message);
        ipc_interrupt_restore(flags);
        return sent;
    }
    if (endpoint->waiting_sender || endpoint->waiting_sender_id || endpoint->waiting_sender_message_ready ||
        !thread_wait_begin(current, THREAD_WAIT_IPC_SEND, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }
    u64 id = current->wait_id;
    endpoint->waiting_sender = current;
    endpoint->waiting_sender_id = id;
    endpoint->waiting_sender_message = *message;
    endpoint->waiting_sender_message_ready = true;
    if (timed && !ipc_timeout_register(current, endpoint, THREAD_WAIT_IPC_SEND, id, timeout_ticks)) {
        ipc_finish(current, endpoint, THREAD_WAIT_IPC_SEND, id, THREAD_WAIT_RESULT_CANCELLED);
        ipc_end_reservation(current, endpoint, THREAD_WAIT_IPC_SEND, id, false);
        ipc_interrupt_restore(flags);
        return false;
    }
    ipc_before_park(current, endpoint);
    for (;;) {
        if (!thread_wait_matches_id(current, THREAD_WAIT_IPC_SEND, endpoint, id) ||
            !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_SEND)) cpu_halt_forever();
        if (current->wait_result != THREAD_WAIT_RESULT_PENDING) break;
        if (!endpoint->waiting_sender_message_ready) cpu_halt_forever();
        ++g_park_attempts;
        if (!scheduler_block_current()) {
            ipc_finish(current, endpoint, THREAD_WAIT_IPC_SEND, id, THREAD_WAIT_RESULT_CANCELLED);
            break;
        }
    }
    bool sent = current->wait_result == THREAD_WAIT_RESULT_COMPLETED;
    if (sent && endpoint->waiting_sender_message_ready) cpu_halt_forever();
    ipc_end_reservation(current, endpoint, THREAD_WAIT_IPC_SEND, id, timed);
    ipc_interrupt_restore(flags);
    return sent;
}

bool ipc_send_blocking(Process *process, CapabilityHandle handle, const IpcMessage *message) {
    return ipc_send_blocking_common(process, handle, message, false, 0);
}

bool ipc_send_blocking_for(Process *process, CapabilityHandle handle, const IpcMessage *message, u64 timeout_ticks) {
    return ipc_send_blocking_common(process, handle, message, true, timeout_ticks);
}

bool ipc_abort_thread_wait(Thread *thread) {
    u64 flags = ipc_interrupt_save();
    bool aborted = false;
    if (thread == thread_current() || !thread_wait_active(thread)) goto done;
    if (thread->state != THREAD_STATE_BLOCKED && thread->state != THREAD_STATE_READY) goto done;
    ThreadWaitKind kind = thread->wait_kind;
    Endpoint *endpoint = (Endpoint *)thread->wait_object;
    u64 wait_id = thread->wait_id;
    if (!ipc_endpoint_valid(endpoint) || !ipc_reservation_valid(endpoint, kind)) goto done;
    if (kind == THREAD_WAIT_IPC_RECEIVE) {
        if (endpoint->waiting_receiver != thread || !thread_wait_abort(thread, kind, endpoint)) goto done;
        (void)ipc_timeout_unregister(thread, endpoint, kind, wait_id);
        /* An unconsumed message stays available if this endpoint is still open. */
        endpoint->waiting_receiver = 0;
        endpoint->waiting_receiver_id = 0;
    } else if (kind == THREAD_WAIT_IPC_SEND) {
        if (endpoint->waiting_sender != thread || !thread_wait_abort(thread, kind, endpoint)) goto done;
        (void)ipc_timeout_unregister(thread, endpoint, kind, wait_id);
        endpoint->waiting_sender = 0;
        endpoint->waiting_sender_id = 0;
        /* Discard only staging; a committed mailbox message is independent. */
        k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
        endpoint->waiting_sender_message_ready = false;
    } else goto done;
    aborted = true;
done:
    ipc_interrupt_restore(flags);
    return aborted;
}

bool ipc_endpoint_close(Endpoint *endpoint) {
    u64 flags = ipc_interrupt_save();
    bool closed = false;
    if (!ipc_endpoint_valid(endpoint) || endpoint->closed ||
        !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_RECEIVE) ||
        !ipc_reservation_valid(endpoint, THREAD_WAIT_IPC_SEND)) goto done;
    Thread *receiver = endpoint->waiting_receiver;
    Thread *sender = endpoint->waiting_sender;
    bool staged = endpoint->waiting_sender_message_ready;
    if (staged && (!sender || sender->wait_result != THREAD_WAIT_RESULT_PENDING)) goto done;
    if (sender && sender->wait_result == THREAD_WAIT_RESULT_PENDING && !staged) goto done;

    endpoint->closed = true;
    if (receiver && receiver->wait_result == THREAD_WAIT_RESULT_PENDING) {
        ipc_finish(receiver, endpoint, THREAD_WAIT_IPC_RECEIVE, endpoint->waiting_receiver_id, THREAD_WAIT_RESULT_PEER_CLOSED);
    }
    if (sender && sender->wait_result == THREAD_WAIT_RESULT_PENDING) {
        ipc_finish(sender, endpoint, THREAD_WAIT_IPC_SEND, endpoint->waiting_sender_id, THREAD_WAIT_RESULT_PEER_CLOSED);
    }
    /* Never overwrite COMPLETED with PEER_CLOSED. Receipt by the mailbox is
     * acceptance, not guaranteed peer consumption or durability. */
    k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
    endpoint->waiting_sender_message_ready = false;
    k_memset(&endpoint->message, 0, sizeof(endpoint->message));
    endpoint->message_ready = false;
    if (receiver && !ipc_make_ready(receiver)) cpu_halt_forever();
    if (sender && !ipc_make_ready(sender)) cpu_halt_forever();
    /* Reverse links/IDs still pin the object until continuation end or abort. */
    closed = true;
done:
    ipc_interrupt_restore(flags);
    return closed;
}


bool ipc_close_owned_endpoints(u64 owner_process_id) {
    if (!owner_process_id) return false;

    u64 flags = ipc_interrupt_save();
    bool result = true;
    Endpoint *endpoint = endpoint_owned_first(owner_process_id);

    while (endpoint) {
        Endpoint *next = endpoint_owned_next(owner_process_id, endpoint);
        if (!endpoint_closed(endpoint) && !ipc_endpoint_close(endpoint)) {
            result = false;
            break;
        }
        endpoint = next;
    }

    ipc_interrupt_restore(flags);
    return result;
}
