#include "task.h"

#include "arch.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "scheduler.h"
#include "process_exit_queue.h"

#define TASK_RFLAGS_IF (1ULL << 9)

static u64 task_interrupt_save(void) {
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

static void task_interrupt_restore(u64 flags) {
    if (flags & TASK_RFLAGS_IF) interrupts_enable();
}

bool task_terminate_thread(Thread *thread) {
    if (!thread || thread == thread_current() || !thread->id) {
        return false;
    }

    u64 flags = task_interrupt_save();

    /* Check scheduler state before changing an IPC relationship. */
    if (!scheduler_can_terminate_thread(thread)) {
        task_interrupt_restore(flags);
        return false;
    }

    if (thread_wait_active(thread)) {
        bool detached = false;
        if (thread->wait_kind == THREAD_WAIT_IPC_RECEIVE || thread->wait_kind == THREAD_WAIT_IPC_SEND) {
            detached = ipc_abort_thread_wait(thread);
        } else if (thread->wait_kind == THREAD_WAIT_PROCESS_EXIT) {
            detached = process_exit_queue_abort_wait(thread);
        }
        if (!detached) {
            task_interrupt_restore(flags);
            return false;
        }
    }

    /*
     * At this point the Thread has been detached
     * from external lifetime ownership.
     *
     * Failure now would be an internal invariant
     * violation: allowing the Thread to continue
     * would be unsafe.
     */
    if (!scheduler_terminate_thread(thread)) cpu_halt_forever();

    task_interrupt_restore(flags);
    return true;
}

bool task_user_fault_supported(u64 vector) {
    /* Explicit, tested set: #DE, #UD, #NM, #GP, #PF. Not #DF/NMI/#MC. */
    return vector == 0ULL || vector == 6ULL || vector == 7ULL || vector == 13ULL || vector == 14ULL;
}

static Thread *task_user_terminal_current(const InterruptFrame *frame) {
    Thread *current = thread_current();
    if (!frame || !interrupt_from_user(frame) || !current || !current->id ||
        !current->process || !process_capabilities(current->process) || current->process->kernel ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || thread_wait_active(current) ||
        current->process->exit_info.reason != PROCESS_EXIT_NONE ||
        !process_thread_can_detach(current->process, current) || !scheduler_can_terminate_current()) {
        cpu_halt_forever();
    }
    return current;
}

static void task_publish_exit(Process *process, Thread *thread, ProcessExitReason reason,
    const InterruptFrame *frame, u64 fault_address) {
    ProcessExitInfo info;
    k_memset(&info, 0, sizeof(info));
    info.reason = reason;
    info.process_id = process->id;
    info.thread_id = thread->id;
    if (frame && reason == PROCESS_EXIT_FAULT) {
        info.vector = frame->vector;
        info.error_code = frame->error_code;
        info.rip = frame->rip;
        info.cr2 = frame->vector == 14ULL ? fault_address : 0ULL;
    }
    if (!process_exit_publish(process, &info)) cpu_halt_forever();
}

InterruptFrame *task_exit_current_from_interrupt(InterruptFrame *frame) {
    interrupts_disable();
    Thread *current = task_user_terminal_current(frame);
    Process *process = current->process;
    bool last = true;
    for (Thread *t = process_thread_first(process); t; t = t->process_next) {
        if (t != current && t->state != THREAD_STATE_DEAD) last = false;
    }
    if (last) {
        if (!ipc_close_owned_endpoints(process->id)) cpu_halt_forever();
        task_publish_exit(process, current, PROCESS_EXIT_NORMAL, 0, 0);
    }
    return scheduler_terminate_current_from_interrupt(frame);
}

InterruptFrame *task_fault_current_from_interrupt(InterruptFrame *frame, u64 fault_address) {
    interrupts_disable();
    Thread *current = task_user_terminal_current(frame);
    Process *process = current->process;
    if (!task_user_fault_supported(frame->vector)) cpu_halt_forever();

    /* No destructor here: even a DEAD sibling's stack and the current CR3 stay
     * owned. R2 abort detaches BLOCKED and READY-but-unresumed IPC references. */
    for (Thread *t = process_thread_first(process); t; t = t->process_next) {
        if (t != current && t->state != THREAD_STATE_DEAD && !task_terminate_thread(t)) cpu_halt_forever();
    }
    /* Remove sibling reservations first, then wake external peers by close. */
    if (!ipc_close_owned_endpoints(process->id)) cpu_halt_forever();
    task_publish_exit(process, current, PROCESS_EXIT_FAULT, frame, fault_address);
    return scheduler_terminate_current_from_interrupt(frame);
}

bool task_quiesce_process(Process *process) {
    if (!process || process == process_kernel() || !process_capabilities(process) || process->kernel) return false;
    Thread *current = thread_current();
    if (!current || current->process == process) return false;
    u64 flags = task_interrupt_save();
    bool result = false;

    /* Peer death is published before this process loses thread/capability state.
     * Closing is idempotent across cleanup retries: already-closed endpoints are
     * skipped, while any failed close leaves the Process available for retry. */
    if (!ipc_close_owned_endpoints(process->id)) goto done;

    u64 count = process_thread_count(process);
    Thread *first = process_thread_first(process);
    if ((count && !process_thread_can_detach(process, first)) ||
        (!count && (process->thread_head || process->thread_tail))) goto done;

    /* Stop everyone before dropping any self-thread authority. No reaping yet. */
    bool stopped = true;
    Thread *thread = first;
    for (u64 i = 0; i < count; ++i) {
        if (!thread) { stopped = false; break; }
        if (thread->state != THREAD_STATE_DEAD && !task_terminate_thread(thread)) stopped = false;
        if (thread->state != THREAD_STATE_DEAD || thread->on_run_queue || thread_wait_active(thread)) stopped = false;
        thread = thread->process_next;
    }
    if (!stopped || thread) goto done;

    /* Preserve an existing fault/normal record even across partial reap retry.
     * An unpublished empty Process has no execution outcome to report. */
    if (first && process->exit_info.reason == PROCESS_EXIT_NONE) {
        task_publish_exit(process, first, PROCESS_EXIT_TERMINATED, 0, 0);
    }

    CapabilityTable *caps = process_capabilities(process);
    /* A process may hold caps to its own threads. Delete those pins before reap.
     * Other process-owned caps remain until every reap succeeds, as before. */
    for (thread = first; thread; thread = thread->process_next) {
        if (!capability_revoke_object(caps, thread, CAPABILITY_TYPE_THREAD)) goto done;
    }

    bool reaped = true;
    thread = first;
    for (u64 i = 0; i < count; ++i) {
        if (!thread) { reaped = false; break; }
        Thread *next = thread->process_next;
        if (!thread_destroy(thread)) reaped = false;
        thread = next;
    }
    if (!reaped || process_thread_count(process) || process->thread_head || process->thread_tail) goto done;
    result = capability_revoke_all(caps);
done:
    task_interrupt_restore(flags);
    return result;
}