#include "task.h"

#include "arch.h"
#include "interrupts.h"
#include "ipc.h"
#include "scheduler.h"

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
        if (!ipc_abort_thread_wait(thread)) {
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