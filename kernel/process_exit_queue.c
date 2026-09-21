#include "process_exit_queue.h"

#include "arch.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "scheduler.h"

#define EXIT_QUEUE_RFLAGS_IF (1ULL << 9)

static void *g_exit_queues[PROCESS_EXIT_QUEUE_STORAGE_CAPACITY];
static u64 g_next_exit_queue_id = 1ULL;

static u64 queue_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void queue_irq_restore(u64 flags) {
    if (flags & EXIT_QUEUE_RFLAGS_IF) interrupts_enable();
}

static bool queue_live(const ProcessExitQueue *queue) {
    return object_storage_find(g_exit_queues, PROCESS_EXIT_QUEUE_STORAGE_CAPACITY, queue) <
        PROCESS_EXIT_QUEUE_STORAGE_CAPACITY;
}

bool process_exit_queue_storage_in_use(const ProcessExitQueue *queue) {
    u64 flags = queue_irq_save();
    bool result = queue_live(queue);
    queue_irq_restore(flags);
    return result;
}

u32 process_exit_queue_object_count(void) {
    u64 flags = queue_irq_save();
    u32 result = object_storage_count(g_exit_queues, PROCESS_EXIT_QUEUE_STORAGE_CAPACITY);
    queue_irq_restore(flags);
    return result;
}

static bool wait_reservation_valid(const ProcessExitQueue *queue) {
    Thread *thread = queue->waiting_receiver;
    u64 id = queue->waiting_receiver_id;
    if (!thread) return id == 0;
    if (!thread_wait_matches_id(thread, THREAD_WAIT_PROCESS_EXIT, queue, id)) return false;
    if (thread == thread_current() && thread->state == THREAD_STATE_RUNNING) {
        return thread->on_run_queue && thread->run_next;
    }
    if (!thread->interrupt_context_ready || !thread->interrupt_rsp) return false;
    if (thread->state == THREAD_STATE_BLOCKED) return !thread->on_run_queue && !thread->run_next;
    if (thread->state == THREAD_STATE_READY) return thread->on_run_queue && thread->run_next;
    return false;
}

static bool queue_valid(const ProcessExitQueue *queue) {
    if (!queue_live(queue) || !queue->initialized || !queue->id ||
        queue->watch_count > PROCESS_EXIT_QUEUE_CAPACITY ||
        queue->pending_count > PROCESS_EXIT_QUEUE_CAPACITY ||
        queue->watch_count + queue->pending_count > PROCESS_EXIT_QUEUE_CAPACITY ||
        queue->pending_head >= PROCESS_EXIT_QUEUE_CAPACITY ||
        queue->pending_tail >= PROCESS_EXIT_QUEUE_CAPACITY || !wait_reservation_valid(queue)) return false;

    u32 watching = 0;
    u32 pending = 0;
    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        const ProcessExitQueueSlot *slot = &queue->slots[i];
        if (slot->watching) {
            ++watching;
            if (slot->pending || !slot->process || !slot->process_id ||
                slot->event.reason != PROCESS_EXIT_NONE || !process_storage_in_use(slot->process) ||
                slot->process->id != slot->process_id || slot->process->exit_queue != queue ||
                slot->process->exit_queue_id != queue->id) return false;
        } else if (slot->pending) {
            ++pending;
            if (slot->process || !slot->process_id ||
                slot->event.reason == PROCESS_EXIT_NONE || slot->event.process_id != slot->process_id) return false;
        } else if (slot->process || slot->process_id ||
            slot->event.reason != PROCESS_EXIT_NONE) return false;
    }
    if (watching != queue->watch_count || pending != queue->pending_count ||
        queue->pending_tail != (queue->pending_head + queue->pending_count) % PROCESS_EXIT_QUEUE_CAPACITY) return false;
    bool seen[PROCESS_EXIT_QUEUE_CAPACITY];
    k_memset(seen, 0, sizeof(seen));
    for (u32 i = 0; i < queue->pending_count; ++i) {
        u32 order = (queue->pending_head + i) % PROCESS_EXIT_QUEUE_CAPACITY;
        u32 index = queue->pending_order[order];
        if (index >= PROCESS_EXIT_QUEUE_CAPACITY || seen[index] || !queue->slots[index].pending) return false;
        seen[index] = true;
    }
    return true;
}

bool process_exit_queue_create(ProcessExitQueue *queue) {
    if (!queue) return false;
    u64 flags = queue_irq_save();
    bool result = false;
    u32 slot = object_storage_empty(g_exit_queues, PROCESS_EXIT_QUEUE_STORAGE_CAPACITY);
    if (!queue_live(queue) && slot < PROCESS_EXIT_QUEUE_STORAGE_CAPACITY && g_next_exit_queue_id) {
        k_memset(queue, 0, sizeof(*queue));
        queue->id = g_next_exit_queue_id++;
        queue->initialized = true;
        g_exit_queues[slot] = queue;
        result = true;
    }
    queue_irq_restore(flags);
    return result;
}

static bool queue_make_ready(Thread *thread) {
    if (thread->state == THREAD_STATE_BLOCKED) return scheduler_wake(thread);
    return (thread == thread_current() && thread->state == THREAD_STATE_RUNNING && thread->on_run_queue) ||
        (thread->state == THREAD_STATE_READY && thread->on_run_queue);
}

bool process_exit_queue_watch(ProcessExitQueue *queue, Process *process) {
    u64 flags = queue_irq_save();
    bool result = false;
    if (!queue_valid(queue) || queue->closed || !process_storage_in_use(process) || !process ||
        !process->initialized || !process->id || process->kernel || process->exit_info.reason != PROCESS_EXIT_NONE ||
        process->exit_queue || process->exit_queue_id ||
        queue->watch_count + queue->pending_count >= PROCESS_EXIT_QUEUE_CAPACITY) goto done;
    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        ProcessExitQueueSlot *slot = &queue->slots[i];
        if (slot->watching || slot->pending) continue;
        slot->process = process;
        slot->process_id = process->id;
        slot->watching = true;
        process->exit_queue = queue;
        process->exit_queue_id = queue->id;
        ++queue->watch_count;
        result = true;
        break;
    }
done:
    queue_irq_restore(flags);
    return result;
}

bool process_exit_queue_unwatch_process(Process *process) {
    if (!process) return false;
    u64 flags = queue_irq_save();
    if (!process->exit_queue && !process->exit_queue_id) {
        queue_irq_restore(flags);
        return true;
    }
    ProcessExitQueue *queue = process->exit_queue;
    bool result = false;
    if (!queue_valid(queue) || process->exit_queue_id != queue->id || !process_storage_in_use(process)) goto done;
    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        ProcessExitQueueSlot *slot = &queue->slots[i];
        if (!slot->watching || slot->process != process || slot->process_id != process->id) continue;
        k_memset(slot, 0, sizeof(*slot));
        --queue->watch_count;
        process->exit_queue = 0;
        process->exit_queue_id = 0;
        result = true;
        break;
    }
done:
    queue_irq_restore(flags);
    return result;
}

bool process_exit_queue_publish_process(Process *process, const ProcessExitInfo *info) {
    if (!process || !info) return false;
    u64 flags = queue_irq_save();
    if (!process->exit_queue && !process->exit_queue_id) {
        queue_irq_restore(flags);
        return true;
    }
    ProcessExitQueue *queue = process->exit_queue;
    bool result = false;
    if (!queue_valid(queue) || queue->closed || process->exit_queue_id != queue->id ||
        info->reason == PROCESS_EXIT_NONE || info->process_id != process->id) goto done;
    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        ProcessExitQueueSlot *slot = &queue->slots[i];
        if (!slot->watching || slot->process != process || slot->process_id != process->id) continue;
        slot->process = 0;
        slot->watching = false;
        slot->pending = true;
        slot->event = *info;
        queue->pending_order[queue->pending_tail] = (u8)i;
        queue->pending_tail = (queue->pending_tail + 1U) % PROCESS_EXIT_QUEUE_CAPACITY;
        --queue->watch_count;
        ++queue->pending_count;
        process->exit_queue = 0;
        process->exit_queue_id = 0;

        Thread *receiver = queue->waiting_receiver;
        if (receiver && receiver->wait_result == THREAD_WAIT_RESULT_PENDING) {
            if (!thread_wait_try_complete(receiver, THREAD_WAIT_PROCESS_EXIT, queue,
                    queue->waiting_receiver_id, THREAD_WAIT_RESULT_COMPLETED) ||
                !queue_make_ready(receiver)) cpu_halt_forever();
        }
        result = true;
        break;
    }
done:
    queue_irq_restore(flags);
    return result;
}

static bool receive_locked(ProcessExitQueue *queue, ProcessExitInfo *out) {
    if (!queue->pending_count) return false;
    u32 index = queue->pending_order[queue->pending_head];
    if (index >= PROCESS_EXIT_QUEUE_CAPACITY) return false;
    ProcessExitQueueSlot *slot = &queue->slots[index];
    if (!slot->pending) return false;
    *out = slot->event;
    k_memset(slot, 0, sizeof(*slot));
    queue->pending_order[queue->pending_head] = 0U;
    queue->pending_head = (queue->pending_head + 1U) % PROCESS_EXIT_QUEUE_CAPACITY;
    --queue->pending_count;
    return true;
}

bool process_exit_queue_try_receive(ProcessExitQueue *queue, ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    u64 flags = queue_irq_save();
    bool result = queue_valid(queue) && !queue->waiting_receiver && !queue->waiting_receiver_id &&
        queue->pending_count && receive_locked(queue, out);
    queue_irq_restore(flags);
    return result;
}

bool process_exit_queue_receive_blocking(ProcessExitQueue *queue, ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    Thread *current = thread_current();
    Process *kernel = process_kernel();
    if (!current || !kernel || current->process != kernel || !current->id) return false;

    u64 flags = queue_irq_save();
    if (!queue_valid(queue)) { queue_irq_restore(flags); return false; }
    if (queue->pending_count) {
        bool received = receive_locked(queue, out);
        queue_irq_restore(flags);
        return received;
    }
    if (queue->closed || queue->waiting_receiver || queue->waiting_receiver_id ||
        !thread_wait_begin(current, THREAD_WAIT_PROCESS_EXIT, queue)) {
        queue_irq_restore(flags);
        return false;
    }

    u64 id = current->wait_id;
    queue->waiting_receiver = current;
    queue->waiting_receiver_id = id;
    for (;;) {
        if (!thread_wait_matches_id(current, THREAD_WAIT_PROCESS_EXIT, queue, id) ||
            !wait_reservation_valid(queue)) cpu_halt_forever();
        if (current->wait_result != THREAD_WAIT_RESULT_PENDING) break;
        if (queue->pending_count) {
            if (!thread_wait_try_complete(current, THREAD_WAIT_PROCESS_EXIT, queue, id,
                    THREAD_WAIT_RESULT_COMPLETED)) cpu_halt_forever();
            break;
        }
        if (!scheduler_wait_current()) {
            if (!thread_wait_try_complete(current, THREAD_WAIT_PROCESS_EXIT, queue, id,
                    THREAD_WAIT_RESULT_CANCELLED)) cpu_halt_forever();
            break;
        }
    }

    bool received = current->wait_result == THREAD_WAIT_RESULT_COMPLETED &&
        queue->pending_count && receive_locked(queue, out);
    if (!thread_wait_end(current, THREAD_WAIT_PROCESS_EXIT, queue)) cpu_halt_forever();
    queue->waiting_receiver = 0;
    queue->waiting_receiver_id = 0;
    queue_irq_restore(flags);
    return received;
}

bool process_exit_queue_abort_wait(Thread *thread) {
    if (!thread || thread == thread_current()) return false;
    u64 flags = queue_irq_save();
    bool result = false;
    if (!thread_wait_active(thread) || thread->wait_kind != THREAD_WAIT_PROCESS_EXIT ||
        (thread->state != THREAD_STATE_BLOCKED && thread->state != THREAD_STATE_READY)) goto done;
    ProcessExitQueue *queue = (ProcessExitQueue *)thread->wait_object;
    if (!queue_valid(queue) || queue->waiting_receiver != thread ||
        queue->waiting_receiver_id != thread->wait_id ||
        !thread_wait_abort(thread, THREAD_WAIT_PROCESS_EXIT, queue)) goto done;
    queue->waiting_receiver = 0;
    queue->waiting_receiver_id = 0;
    result = true;
done:
    queue_irq_restore(flags);
    return result;
}

bool process_exit_queue_close(ProcessExitQueue *queue) {
    u64 flags = queue_irq_save();
    bool result = false;
    if (!queue_valid(queue) || queue->closed) goto done;

    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        ProcessExitQueueSlot *slot = &queue->slots[i];
        if (!slot->watching) continue;
        Process *process = slot->process;
        if (!process_storage_in_use(process) || process->id != slot->process_id ||
            process->exit_queue != queue || process->exit_queue_id != queue->id) goto done;
    }

    queue->closed = true;
    for (u32 i = 0; i < PROCESS_EXIT_QUEUE_CAPACITY; ++i) {
        ProcessExitQueueSlot *slot = &queue->slots[i];
        if (!slot->watching) continue;
        Process *process = slot->process;
        process->exit_queue = 0;
        process->exit_queue_id = 0;
        k_memset(slot, 0, sizeof(*slot));
        --queue->watch_count;
    }

    Thread *receiver = queue->waiting_receiver;
    if (receiver && receiver->wait_result == THREAD_WAIT_RESULT_PENDING) {
        if (!thread_wait_try_complete(receiver, THREAD_WAIT_PROCESS_EXIT, queue,
                queue->waiting_receiver_id, THREAD_WAIT_RESULT_PEER_CLOSED) ||
            !queue_make_ready(receiver)) cpu_halt_forever();
    }
    result = true;
done:
    queue_irq_restore(flags);
    return result;
}

bool process_exit_queue_destroy(ProcessExitQueue *queue) {
    u64 flags = queue_irq_save();
    bool result = false;
    if (!queue_valid(queue) || !queue->closed || queue->watch_count || queue->pending_count ||
        queue->waiting_receiver || queue->waiting_receiver_id) goto done;
    u32 slot = object_storage_find(g_exit_queues, PROCESS_EXIT_QUEUE_STORAGE_CAPACITY, queue);
    if (slot == PROCESS_EXIT_QUEUE_STORAGE_CAPACITY) goto done;
    g_exit_queues[slot] = 0;
    k_memset(queue, 0, sizeof(*queue));
    result = true;
done:
    queue_irq_restore(flags);
    return result;
}

u32 process_exit_queue_watch_count(const ProcessExitQueue *queue) {
    u64 flags = queue_irq_save();
    u32 result = queue_live(queue) && queue->initialized ? queue->watch_count : 0U;
    queue_irq_restore(flags);
    return result;
}

u32 process_exit_queue_pending_count(const ProcessExitQueue *queue) {
    u64 flags = queue_irq_save();
    u32 result = queue_live(queue) && queue->initialized ? queue->pending_count : 0U;
    queue_irq_restore(flags);
    return result;
}
