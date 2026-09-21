#include "process.h"
#include "lib.h"
#include "thread.h"
#include "arch.h"
#include "interrupts.h"
#include "object_storage.h"
#include "endpoint.h"
#include "process_exit_queue.h"

static Process g_kernel_process;

static u64 g_next_process_id;
static bool g_initialized;
static void *g_process_storage[PROCESS_STORAGE_CAPACITY];

static u64 process_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void process_irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }

static bool process_storage_live(const Process *process) {
    return object_storage_find(g_process_storage, PROCESS_STORAGE_CAPACITY, process) < PROCESS_STORAGE_CAPACITY;
}

u32 process_object_count(void) {
    u64 flags = process_irq_save();
    u32 count = object_storage_count(g_process_storage, PROCESS_STORAGE_CAPACITY);
    process_irq_restore(flags);
    return count;
}

bool process_storage_in_use(const Process *process) {
    u64 flags = process_irq_save();
    bool result = process_storage_live(process);
    process_irq_restore(flags);
    return result;
}

bool process_system_init(AddressSpace *kernel_space) {
    if (g_initialized || !kernel_space || !kernel_space->kernel || !address_space_cr3(kernel_space)) return false;

    k_memset(&g_kernel_process, 0, sizeof(g_kernel_process));

    if (!capability_table_init(&g_kernel_process.capabilities)) return false;

    g_kernel_process.id = 1ULL;
    g_kernel_process.address_space = kernel_space;
    g_kernel_process.kernel = true;
    g_kernel_process.owns_address_space = false;
    g_kernel_process.initialized = true;
    g_next_process_id = 2ULL;
    g_process_storage[0] = &g_kernel_process;
    g_initialized = true;
    g_kernel_process.thread_count = 0;

    return true;
}

Process *process_kernel(void) {
    if (!g_initialized || !g_kernel_process.initialized) return 0;
    return &g_kernel_process;
}

static bool process_create_locked(Process *process) {
    if (!process || process_storage_live(process) ||
        address_space_storage_in_use(&process->owned_address_space) ||
        capability_table_storage_in_use(&process->capabilities)) return false;
    k_memset(process, 0, sizeof(*process));
    if (!g_initialized || !g_next_process_id) return false;
    u32 slot = object_storage_empty(g_process_storage, PROCESS_STORAGE_CAPACITY);
    if (slot == PROCESS_STORAGE_CAPACITY) return false;

    /* Nonallocating setup first. Nothing fallible follows address-space publication. */
    if (!capability_table_init(&process->capabilities)) return false;
    if (!address_space_create(&process->owned_address_space)) {
        /* Unpublished empty table must not retain the failed caller's storage. */
        if (!capability_table_destroy(&process->capabilities)) cpu_halt_forever();
        k_memset(process, 0, sizeof(*process));
        return false;
    }
    process->id = g_next_process_id++;
    process->address_space = &process->owned_address_space;
    process->owns_address_space = true;
    process->initialized = true;
    g_process_storage[slot] = process;
    return true;
}

bool process_create(Process *process) {
    u64 flags = process_irq_save();
    bool created = process_create_locked(process);
    process_irq_restore(flags);
    return created;
}

static bool process_destroy_locked(Process *process) {
    u32 slot = object_storage_find(g_process_storage, PROCESS_STORAGE_CAPACITY, process);
    if (slot == PROCESS_STORAGE_CAPACITY) return false;
    if (!g_initialized ||
        !process ||
        process == &g_kernel_process ||
        !process->initialized ||
        !process->id ||
        process->kernel ||
        !process->owns_address_space ||
        process->address_space != &process->owned_address_space) {
        return false;
    }

    /* The address space must remain alive until every thread belonging to this process has been destroyed/reaped. */
    if (process->thread_count != 0ULL || process->thread_head || process->thread_tail) {
        return false;
    }
    /* A failed ELF cleanup must keep its address space alive. */
    if (process->elf_image) return false;
    /* Capabilities represent authority owned by the process. Require explicit revocation before destroying the process. */
    if (!capability_table_empty(&process->capabilities)) return false;
    /* An open service endpoint means peer-death notification has not yet been
     * published. Closed endpoints may outlive this Process while external caps
     * or waiter reservations retain their own storage references. */
    if (endpoint_process_has_open_owned(process->id)) return false;
    if (!address_space_destroy(&process->owned_address_space)) return false;
    /* The only remaining fallible destructor has committed. A live watch now
     * belongs to an unpublished/nonterminal incarnation and must be detached
     * before this Process storage can be reused. */
    if ((process->exit_queue || process->exit_queue_id) &&
        !process_exit_queue_unwatch_process(process)) cpu_halt_forever();
    if (!capability_table_destroy(&process->capabilities)) cpu_halt_forever();
    g_process_storage[slot] = 0;
    k_memset(process, 0, sizeof(*process));
    return true;
}

bool process_destroy(Process *process) {
    u64 flags = process_irq_save();
    bool destroyed = process_destroy_locked(process);
    process_irq_restore(flags);
    return destroyed;
}

AddressSpace *process_address_space(Process *process) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized) return 0;
    return process->address_space;
}

CapabilityTable *process_capabilities(Process *process) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized) return 0;
    return &process->capabilities;
}

u64 process_thread_count(const Process *process) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized) return 0;
    return process->thread_count;
}

bool process_exit_info(const Process *process, ProcessExitInfo *out) {
    if (!out) return false;
    u64 flags = process_irq_save();
    bool live = g_initialized && process_storage_live(process) && process->initialized;
    /* The query must not clear the canonical record when its output aliases it. */
    if (live && out == &process->exit_info) { process_irq_restore(flags); return false; }
    k_memset(out, 0, sizeof(*out));
    bool result = live && !process->kernel && process->exit_info.reason != PROCESS_EXIT_NONE;
    if (result) k_memcpy(out, &process->exit_info, sizeof(*out));
    process_irq_restore(flags);
    return result;
}

bool process_exit_publish(Process *process, const ProcessExitInfo *info) {
    if (!info) return false;
    u64 flags = process_irq_save();
    bool result = false;
    if (!g_initialized || !process_storage_live(process) || !process->initialized || process->kernel ||
        process->exit_info.reason != PROCESS_EXIT_NONE || info->process_id != process->id ||
        !info->thread_id || (info->reason != PROCESS_EXIT_NORMAL && info->reason != PROCESS_EXIT_FAULT &&
        info->reason != PROCESS_EXIT_TERMINATED)) goto done;
    /* Validate the still-owned list before publishing: dead threads are retained,
     * with at most the caller's terminal handoff thread still RUNNING. */
    Thread *first = process_thread_first(process);
    if (!first || !process_thread_can_detach(process, first)) goto done;
    Thread *current = thread_current();
    bool found = false;
    for (Thread *t = first; t; t = t->process_next) {
        if (thread_wait_active(t)) goto done;
        if (t->id == info->thread_id) found = true;
        if (t == current) {
            if (t->id != info->thread_id || t->state != THREAD_STATE_RUNNING || !t->on_run_queue) goto done;
        } else if (t->state != THREAD_STATE_DEAD || t->on_run_queue || t->run_next ||
            t->interrupt_context_ready || t->interrupt_rsp) goto done;
    }
    if (!found || endpoint_process_has_open_owned(process->id)) goto done;
    /* Publish into the reserved recovery slot before making the canonical
     * Process record visible. IRQ exclusion means a woken observer cannot run
     * until both copies are committed. */
    if (!process_exit_queue_publish_process(process, info)) goto done;
    k_memcpy(&process->exit_info, info, sizeof(*info));
    result = true;
done:
    process_irq_restore(flags);
    return result;
}

bool process_thread_contains(const Process *process, const Thread *thread) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized || !thread || !thread->id || thread->process != process) {
        return false;
    }

    const Thread *current = process->thread_head;

    for (u64 i = 0; i < process->thread_count; ++i) {
        if (!current || !current->id || current->process != process) {
            return false;
        }

        if (current == thread) return true;
        current = current->process_next;
    }
    return false;
}


Thread *process_thread_first(const Process *process) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized || !process->thread_count) {
        return 0;
    }
    return process->thread_head;
}


bool process_thread_attach(Process *process, Thread *thread) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized || !process->id || !thread || !thread->id ||
        thread->process != process || process->thread_count == ~0ULL) {
        return false;
    }
    if (process->exit_info.reason != PROCESS_EXIT_NONE) return false;
    if (process_thread_contains(process, thread)) return false;
    if (thread->process_prev || thread->process_next) return false;

    if (!process->thread_count) {
        if (process->thread_head || process->thread_tail) return false;

        process->thread_head = thread;
        process->thread_tail = thread;

    } else {
        if (!process->thread_head || !process->thread_tail || process->thread_tail->process_next) {
            return false;
        }

        thread->process_prev = process->thread_tail;
        process->thread_tail->process_next = thread;
        process->thread_tail = thread;
    }
    ++process->thread_count;
    return true;
}

bool process_thread_can_detach(const Process *process, const Thread *thread) {
    if (!g_initialized || !process_storage_live(process) || !process->initialized || !process->id ||
        !thread || !thread->id || thread->process != process || !process->thread_count) return false;
    if (!process->thread_head || !process->thread_tail) return false;

    const Thread *current = process->thread_head;
    const Thread *previous = 0;
    bool found = false;

    for (u64 i = 0; i < process->thread_count; ++i) {
        if (!current || !current->id || current->process != process) return false;
        if (current->process_prev != previous) return false;
        if (current == thread) found = true;
        previous = current;
        current = current->process_next;
    }
    return found && !current && previous == process->thread_tail;
}

bool process_thread_detach(Process *process, Thread *thread) {
    if (!process_thread_can_detach(process, thread)) return false;

    Thread *previous = thread->process_prev;
    Thread *next = thread->process_next;

    if (previous) previous->process_next = next;
    else process->thread_head = next;

    if (next) next->process_prev = previous;
    else process->thread_tail = previous;

    thread->process_prev = 0;
    thread->process_next = 0;

    --process->thread_count;

    if (!process->thread_count) {
        process->thread_head = 0;
        process->thread_tail = 0;
    }

    return true;
}