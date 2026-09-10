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

static bool ipc_thread_blocked(const Thread *thread) {
    if (!thread || !thread->id) return false;

    return
        thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue &&
        thread->interrupt_context_ready &&
        thread->interrupt_rsp;
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
    if (endpoint_message_ready(endpoint)) return false;

    Thread *waiting = endpoint->waiting_receiver;

    if (waiting && !ipc_thread_blocked(waiting)) return false;
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

    Thread *waiting_receiver = endpoint->waiting_receiver;

    /* A message belonging to a blocked/woken receiver is reserved for that receiver. */
    if (waiting_receiver && waiting_receiver != current) return false;
    if (!endpoint_message_ready(endpoint)) return false;

    Thread *waiting_sender = endpoint->waiting_sender;
    bool sender_pending = endpoint->waiting_sender_message_ready;

    if (!waiting_sender && sender_pending) return false;

    /* sender_pending means the sender has not yet been woken, so it must still genuinely be BLOCKED. */
    if (waiting_sender && sender_pending && !ipc_thread_blocked(waiting_sender)) return false;

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

    /* Fast path. */
    if (endpoint_message_ready(endpoint)) {
        bool received = ipc_receive_locked(endpoint, current, out_message);

        ipc_interrupt_restore(flags);
        return received;
    }

    /* First implementation supports one blocked receiver per Endpoint. */
    if (endpoint->waiting_receiver) {
        ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_receiver = current;

    if (!scheduler_block_current()) {
        if (endpoint->waiting_receiver == current) endpoint->waiting_receiver = 0;

        ipc_interrupt_restore(flags);
        return false;
    }

    /* Sender wakes us but leaves this pointer installed until our exact context resumes. */
    if (endpoint->waiting_receiver != current) {
        ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_receiver = 0;

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

    /* Fast path: mailbox has room. */
    if (!endpoint_message_ready(endpoint)) {
        bool sent = ipc_send_locked(endpoint, message);

        ipc_interrupt_restore(flags);
        return sent;
    }

    /* First implementation supports exactly one blocked sender per Endpoint. */
    if (endpoint->waiting_sender || endpoint->waiting_sender_message_ready) {
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

        ipc_interrupt_restore(flags);
        return false;
    }

    /*
     * A successful receive promotes our staged
     * message before waking us.
     *
     * Therefore:
     *
     *   waiting_sender == current
     *   pending == false
     */
    if (endpoint->waiting_sender != current || endpoint->waiting_sender_message_ready) {
        ipc_interrupt_restore(flags);
        return false;
    }

    endpoint->waiting_sender = 0;

    k_memset(&endpoint->waiting_sender_message, 0, sizeof(endpoint->waiting_sender_message));

    ipc_interrupt_restore(flags);
    return true;
}