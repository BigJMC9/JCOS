#include "ipc.h"
#include "scheduler.h"
#include "thread.h"
#include "interrupts.h"
#include "arch.h"
#include "lib.h"

#define IPC_RFLAGS_IF (1ULL << 9)

static u64 ipc_interrupt_save(void) {
    u64 flags = 0;

    __asm__ volatile (
        "pushfq\n\t"
        "popq %0"
        : "=r"(flags)
        :
        : "memory"
    );

    interrupts_disable();
    return flags;
}

static void ipc_interrupt_restore(u64 flags) {
    if (flags & IPC_RFLAGS_IF) interrupts_enable();
}

static bool ipc_message_valid(const IpcMessage *message) {
    if (!message) return false;
    if (!message->word_count) return false;
    if (message->word_count > IPC_MESSAGE_MAX_WORDS) return false;
    return true;
}

static bool ipc_thread_blocked_waiting(const Thread *thread, ThreadWaitKind kind, const Endpoint *endpoint) {
    if (!thread || !thread->id || !endpoint) {
        return false;
    }

    return (
        thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue &&
        thread->interrupt_context_ready &&
        thread->interrupt_rsp &&
        thread->wait_result == THREAD_WAIT_RESULT_PENDING &&
        thread_wait_matches(thread, kind, endpoint)
    );
}

static bool ipc_thread_reserved_waiter(const Thread *thread, ThreadWaitKind kind, const Endpoint *endpoint) {
    if (!thread || !thread->id || !endpoint) return false;
    if (!thread_wait_matches(thread, kind, endpoint)) return false;
    if (thread->wait_result != THREAD_WAIT_RESULT_PENDING) return false;

    if (thread->state == THREAD_STATE_BLOCKED) {
        return (
            !thread->on_run_queue &&
            thread->interrupt_context_ready &&
            thread->interrupt_rsp
        );
    }

    if (thread->state == THREAD_STATE_READY) {
        return (
            thread->on_run_queue &&
            thread->interrupt_context_ready &&
            thread->interrupt_rsp
        );
    }   
    return false;
}

static Endpoint *ipc_resolve_endpoint(Process *process, CapabilityHandle handle, CapabilityRights rights) {
    if (!process || !process->initialized) return 0;

    CapabilityTable *caps = process_capabilities(process);

    if (!caps) return 0;

    void *object = 0;

    if (!capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT, rights, &object)) return 0;
    return (Endpoint *)object;
}

/* Interrupts must already be disabled. */
static bool ipc_send_locked(Endpoint *endpoint, const IpcMessage *message) {
    if (!endpoint || !ipc_message_valid(message)) return false;
    if (endpoint->closed) return false;
    if (endpoint_message_ready(endpoint)) return false;

    Thread *waiting = endpoint->waiting_receiver;

    if (waiting && !ipc_thread_blocked_waiting(waiting, THREAD_WAIT_IPC_RECEIVE, endpoint)) return false;
    if (!endpoint_try_send(endpoint, message)) return false;
    if (waiting && !scheduler_wake(waiting)) {
        IpcMessage discarded;

        if (!endpoint_try_receive(endpoint, &discarded)) cpu_halt_forever();
        return false;
    }

    return true;
}

/*
 * Interrupts must already be disabled.
 *
 * If a sender is blocked because the mailbox
 * was full, consuming the current message makes
 * room for that sender's staged message.
 */
static bool ipc_receive_locked(Endpoint *endpoint, Thread *current, IpcMessage *out_message) {
    if (!endpoint || !out_message) return false;
    if (endpoint->closed) return false;

    Thread *waiting_receiver = endpoint->waiting_receiver;

    /* A message belonging to a blocked/woken receiver is reserved for that receiver. */
    if (waiting_receiver && waiting_receiver != current) return false;
    if (!endpoint_message_ready(endpoint)) return false;

    Thread *waiting_sender = endpoint->waiting_sender;
    bool sender_pending = endpoint->waiting_sender_message_ready;

    if (!waiting_sender && sender_pending) return false;

    if (waiting_sender && sender_pending && !ipc_thread_blocked_waiting(waiting_sender, THREAD_WAIT_IPC_SEND, endpoint)) {
        return false;
    }

    IpcMessage received;
    k_memset(&received, 0, sizeof(received));
    if (!endpoint_try_receive(endpoint, &received)) return false;

    /*
     * The mailbox is now empty.
     *
     * Promote a blocked sender's staged message
     * directly into the mailbox, then wake it.
     */
    if (waiting_sender && sender_pending) {
        IpcMessage pending = endpoint->waiting_sender_message;

        if (!endpoint_try_send(endpoint, &pending)) {
            /* Restore the message we were about to return. */
            if (!endpoint_try_send(endpoint, &received)) cpu_halt_forever();
            return false;
        }

        /*
         * Do not clear waiting_sender.
         *
         * It remains an endpoint lifetime
         * reservation until the sender itself
         * resumes.
         */
        if (!scheduler_wake(waiting_sender)) {
            IpcMessage discarded;

            if (!endpoint_try_receive(endpoint, &discarded)) cpu_halt_forever();
            if (!endpoint_try_send(endpoint, &received)) cpu_halt_forever();
            return false;
        }

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

    if (!endpoint) {
        ipc_interrupt_restore(flags);
        return false;
    }

    bool sent = ipc_send_locked(endpoint, message);

    ipc_interrupt_restore(flags);
    return sent;
}

bool ipc_try_receive(Process *process, CapabilityHandle handle, IpcMessage *out_message) {
    if (!out_message) return false;

    k_memset(out_message, 0, sizeof(*out_message));

    if (!process) return false;

    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_RECEIVE);

    if (!endpoint) {
        ipc_interrupt_restore(flags);
        return false;
    }

    bool received = ipc_receive_locked(endpoint, thread_current(), out_message);

    ipc_interrupt_restore(flags);
    return received;
}

bool ipc_receive_blocking(Process *process, CapabilityHandle handle, IpcMessage *out_message) {
    if (!process || !out_message) return false;

    k_memset(out_message, 0, sizeof(*out_message));
    Thread *current = thread_current();

    if (!current || !current->id || current->process != process) return false;

    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_RECEIVE);

    if (!endpoint) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (endpoint->closed) {
        ipc_interrupt_restore(flags);
        return false;
    }

    /* Fast path. */
    if (endpoint_message_ready(endpoint)) {
        bool received = ipc_receive_locked(endpoint, current, out_message);

        ipc_interrupt_restore(flags);
        return received;
    }

    if (endpoint->waiting_receiver) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (!thread_wait_begin(current, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_receiver = current;

    if (!scheduler_block_current()) {
        if (endpoint->waiting_receiver == current) endpoint->waiting_receiver = 0;

        if (!thread_wait_end(current, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        ipc_interrupt_restore(flags);
        return false;
    }
    if (endpoint->waiting_receiver != current || !thread_wait_matches(current, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }

    bool cancelled = thread_wait_cancelled(current, THREAD_WAIT_IPC_RECEIVE, endpoint);
    endpoint->waiting_receiver = 0;

    if (!thread_wait_end(current, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
        /* Preserve the lifetime relationship rather than silently losing one side. */
        endpoint->waiting_receiver = current;
        ipc_interrupt_restore(flags);
        return false;
    }

    if (cancelled) {
        ipc_interrupt_restore(flags);
        return false;
    }

    bool received = ipc_receive_locked(endpoint, current, out_message);
    ipc_interrupt_restore(flags);
    return received;
}

bool ipc_send_blocking(Process *process, CapabilityHandle handle, const IpcMessage *message) {
    if (!process || !ipc_message_valid(message)) return false;

    Thread *current = thread_current();

    /* Blocking IPC may only block a thread on authority belonging to its own Process. */
    if (!current || !current->id || current->process != process) return false;

    u64 flags = ipc_interrupt_save();
    Endpoint *endpoint = ipc_resolve_endpoint(process, handle, CAPABILITY_RIGHT_SEND);

    if (!endpoint) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (endpoint->closed) {
        ipc_interrupt_restore(flags);
        return false;
    }

    /* Fast path: mailbox has room. */
    if (!endpoint_message_ready(endpoint)) {
        bool sent = ipc_send_locked(endpoint, message);
        ipc_interrupt_restore(flags);
        return sent;
    }

    if (endpoint->waiting_sender || endpoint->waiting_sender_message_ready) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (!thread_wait_begin(current, THREAD_WAIT_IPC_SEND, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_sender = current;

    k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));

    endpoint->waiting_sender_message = *message;
    endpoint->waiting_sender_message_ready = true;

    /*
     * The Endpoint is full.
     *
     * Stay blocked until a receiver consumes the
     * current mailbox message, promotes ours,
     * and wakes us.
     */
    if (!scheduler_block_current()) {
        endpoint->waiting_sender = 0;
        k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
        endpoint->waiting_sender_message_ready = false;

        if (!thread_wait_end(current, THREAD_WAIT_IPC_SEND, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        ipc_interrupt_restore(flags);
        return false;
    }

    if (endpoint->waiting_sender != current || 
        !thread_wait_matches(current, THREAD_WAIT_IPC_SEND, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }
    bool cancelled = thread_wait_cancelled(current, THREAD_WAIT_IPC_SEND, endpoint);

    if (endpoint->waiting_sender_message_ready) {
       ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_sender = 0;

    if (!thread_wait_end(current, THREAD_WAIT_IPC_SEND, endpoint)) {
        endpoint->waiting_sender = current;

        ipc_interrupt_restore(flags);
        return false;
    }

    k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
    ipc_interrupt_restore(flags);
    return !cancelled;
}

static bool ipc_thread_abortable_waiter(const Thread *thread, ThreadWaitKind kind, const Endpoint *endpoint) {
    if (!thread || !thread->id || !endpoint) return false;
    if (!thread_wait_matches(thread, kind, endpoint)) return false;
    if (thread->wait_result != THREAD_WAIT_RESULT_PENDING && thread->wait_result != THREAD_WAIT_RESULT_CANCELLED) {
        return false;
    }
    if (thread->state == THREAD_STATE_BLOCKED) {
        return (
            !thread->on_run_queue &&
            thread->interrupt_context_ready &&
            thread->interrupt_rsp
        );
    }
    if (thread->state == THREAD_STATE_READY) {
        return (
            thread->on_run_queue &&
            thread->interrupt_context_ready &&
            thread->interrupt_rsp
        );
    }

    return false;
}

bool ipc_abort_thread_wait(Thread *thread) {
    if (!thread || !thread->id || thread == thread_current() || !thread_wait_active(thread)) {
        return false;
    }

    u64 flags = ipc_interrupt_save();
    ThreadWaitKind kind = thread->wait_kind;
    Endpoint *endpoint = (Endpoint *)thread->wait_object;

    if (!endpoint || !endpoint->initialized || !endpoint->id) {
        ipc_interrupt_restore(flags);
        return false;
    }

    if (kind == THREAD_WAIT_IPC_RECEIVE) {
        if (endpoint->waiting_receiver != thread || !ipc_thread_abortable_waiter(thread, kind, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        /*
         * If this receiver had already been
         * woken, its reserved message remains in
         * the mailbox and becomes available again.
         */
        if (!thread_wait_abort(thread, kind, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        endpoint->waiting_receiver = 0;

    } else if (kind == THREAD_WAIT_IPC_SEND) {
        if (endpoint->waiting_sender != thread || !ipc_thread_abortable_waiter(thread, kind, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        /*
         * If the staged message still exists,
         * SEND had not committed. Discard it.
         *
         * If it was already promoted, leave the
         * mailbox message alone.
         */
        if (!thread_wait_abort(thread, kind, endpoint)) {
            ipc_interrupt_restore(flags);
            return false;
        }

        endpoint->waiting_sender = 0;

        if (endpoint->waiting_sender_message_ready) {
            k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));
            endpoint-> waiting_sender_message_ready = false;
        }

    } else {
        ipc_interrupt_restore(flags);
        return false;
    }

    ipc_interrupt_restore(flags);
    return true;
}

bool ipc_endpoint_close(Endpoint *endpoint) {
    if (!endpoint) return false;

    u64 flags = ipc_interrupt_save();

    if (!endpoint->initialized || !endpoint->id || endpoint->closed) {
        ipc_interrupt_restore(flags);
        return false;
    }

    Thread *receiver = endpoint->waiting_receiver;
    Thread *sender = endpoint->waiting_sender;
    bool sender_pending = endpoint->waiting_sender_message_ready;

    /* Validate the complete reservation state before mutating anything. */
    if (!sender && sender_pending) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (receiver && !ipc_thread_reserved_waiter(receiver, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }
    if (sender && !ipc_thread_reserved_waiter(sender, THREAD_WAIT_IPC_SEND, endpoint)) {
        ipc_interrupt_restore(flags);
        return false;
    }

    /*
     * A blocked receiver has not received a
     * message yet.
     *
     * A READY receiver has been woken because a
     * message is currently reserved for it.
     */
    if (receiver) {
        if (receiver->state == THREAD_STATE_BLOCKED && endpoint->message_ready) {
            ipc_interrupt_restore(flags);
            return false;
        }

        if (receiver->state == THREAD_STATE_READY && !endpoint->message_ready) {
            ipc_interrupt_restore(flags);
            return false;
        }
    }

    /*
     * waiting_sender_message_ready == true:
     *
     *   SEND is not committed.
     *
     * waiting_sender_message_ready == false:
     *
     *   SEND has already been promoted into the
     *   mailbox and the sender has been woken.
     */
    if (sender) {
        if (sender_pending) {
            if (sender->state != THREAD_STATE_BLOCKED || !endpoint->message_ready) {
                ipc_interrupt_restore(flags);
                return false;
            }

        } 
        else {
            if (sender->state != THREAD_STATE_READY) {
                ipc_interrupt_restore(flags);
                return false;
            }
        }
    }

    endpoint->closed = true;

    /*
     * RECEIVE has not committed until its exact
     * continuation consumes the reserved message.
     *
     * Closing therefore cancels both BLOCKED and
     * READY-but-not-resumed receives.
     */
    if (receiver) {
        if (!thread_wait_cancel(receiver, THREAD_WAIT_IPC_RECEIVE, endpoint)) {
            cpu_halt_forever();
        }
    }

    /*
     * Only an unpromoted SEND is cancelled.
     *
     * A promoted SEND has already committed and
     * must eventually return SUCCESS even if the
     * Endpoint closes before that Thread resumes.
     */
    if (sender && sender_pending) {
        if (!thread_wait_cancel(sender, THREAD_WAIT_IPC_SEND, endpoint)) {
            cpu_halt_forever();
        }

        k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));

        endpoint->waiting_sender_message_ready = false;
    }

    /*
     * Closing an Endpoint discards any unconsumed
     * mailbox message.
     *
     * Send-success means committed/accepted by the
     * Endpoint, not guaranteed consumption by a
     * peer.
     */
    k_memset(&endpoint->message, 0, sizeof(endpoint->message));

    endpoint->message_ready = false;

    /*
     * Keep endpoint->waiting_* installed.
     *
     * Those pointers are the lifetime reservation
     * which prevents Endpoint destruction before
     * the cancelled/committed continuation resumes.
     */
    if (receiver && receiver->state == THREAD_STATE_BLOCKED) {
        if (!scheduler_wake(receiver)) cpu_halt_forever();
    }

    if (sender && sender_pending && sender->state == THREAD_STATE_BLOCKED) {
        if (!scheduler_wake(sender)) cpu_halt_forever();
    }

    ipc_interrupt_restore(flags);
    return true;
}