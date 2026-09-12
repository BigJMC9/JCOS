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
    if (!process || !process->initialized || !process->id || process->kernel || process == process_kernel()) {
        return false;
    }

    Thread *current = thread_current();

    if (!current || current->process == process) {
        return false;
    }

    u64 flags = task_interrupt_save();
    bool result = true;
    u64 initial_count = process_thread_count(process);
    Thread *thread = process_thread_first(process);

    /*
     * Each Thread remains represented in the
     * Process list until thread_destroy()
     * succeeds.
     *
     * A failure therefore never silently loses
     * ownership records.
     */
    for (u64 i = 0; i < initial_count; ++i) {
        if (!thread) {
            result = false;
            break;
        }

        Thread *next = thread->process_next;

        if (thread->process != process || !process_thread_contains(process, thread)) {
            result = false;
            thread = next;
            continue;
        }

        if (thread->state != THREAD_STATE_DEAD) {
            if (!task_terminate_thread(thread)) {
                result = false;
                thread = next;
                continue;
            }
        } else {
            if (thread == current || thread->on_run_queue || thread_wait_active(thread)) {
                result = false;
                thread = next;
                continue;
            }
        }
        if (!thread_destroy(thread)) result = false;

        thread = next;
    }

    /* Do not revoke authority while any Thread remains live or unreaped. */
    if (process_thread_count(process) || process_thread_first(process)) {
        result = false;
    }

    if (result) {
        CapabilityTable *caps = process_capabilities(process);

        if (!caps || !capability_revoke_all(caps)) {
            result = false;
        }
    }

    task_interrupt_restore(flags);
    return result;
}