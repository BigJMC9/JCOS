#include "thread.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"
#include "arch.h"
#include "gdt.h"
#include "interrupts.h"
#include "object_storage.h"
#include "construction_test.h"
#include "pmm_test.h"
#include "kernel_stack.h"

static Thread g_bootstrap_thread;
static Thread *g_current_thread;

static u64 g_next_thread_id;
/* Never reissue an operation identity, including after Thread storage reuse. */
static u64 g_next_wait_id = 1ULL;
static bool g_initialized;

static void *g_thread_storage[THREAD_STORAGE_CAPACITY];
/* No Thread/Process pointer escapes a failed constructor. */
static u64 g_unpublished_stack;
static u64 g_unpublished_stack_virtual;
static struct {
    Thread *target;
    ThreadCreateTestFault fault;
    bool fail_rollback;
    bool armed;
} g_create_fault;

static bool thread_storage_live(const Thread *thread) {
    return object_storage_find(g_thread_storage, THREAD_STORAGE_CAPACITY, thread) < THREAD_STORAGE_CAPACITY;
}

static u64 thread_reclaim_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void thread_reclaim_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}


static NORETURN void thread_kernel_trampoline(void) {
    Thread *thread = thread_current();

    if (!thread || !thread->entry) cpu_halt_forever();

    ThreadEntry entry = thread->entry;
    void *argument = thread->argument;
    entry(argument);

    /* Kernel-thread entries must currently terminate explicitly. */
    cpu_halt_forever();
}

static bool thread_build_interrupt_context(Thread *thread, u64 rip, u64 cs, u64 rflags, u64 return_rsp, u64 return_ss) {
    if (!thread || !rip || !cs || !return_rsp || !return_ss || !thread->kernel_stack_base ||
        !thread->kernel_stack_top) {
        return false;
    }

    const u64 frame_size = sizeof(InterruptFrame) + sizeof(InterruptStackFrame);

    /*
     * Keep an unused return-sized slot at the
     * very top. For a kernel first-entry this
     * also gives the trampoline normal SysV
     * function-entry alignment.
     */
    u64 anchor = thread->kernel_stack_top - sizeof(u64);
    if (anchor < thread->kernel_stack_base + frame_size) return false;

    *(u64 *)(u64)anchor = 0;

    u64 frame_address = anchor - frame_size;
    InterruptFrame *frame = (InterruptFrame *)(u64) frame_address;
    InterruptStackFrame *stack = (InterruptStackFrame *) ((u8 *)frame + sizeof(InterruptFrame));

    k_memset((void *)(u64)frame_address, 0, (usize)frame_size);

    frame->rip = rip;
    frame->cs = cs;
    frame->rflags = rflags;

    stack->rsp = return_rsp;
    stack->ss = return_ss;

    thread->interrupt_rsp = frame_address;
    thread->interrupt_context_ready = true;
    return true;
}

bool thread_reclaim_unpublished_stack(void) {
    u64 flags = thread_reclaim_irq_save();
    bool result = !g_unpublished_stack ||
        kernel_stack_discard_unpublished(g_unpublished_stack, g_unpublished_stack_virtual);
    if (result) {
        g_unpublished_stack = 0;
        g_unpublished_stack_virtual = 0;
    }
    thread_reclaim_irq_restore(flags);
    return result;
}

bool thread_creation_cleanup_pending(void) {
    u64 flags = thread_reclaim_irq_save();
    bool pending = g_unpublished_stack != 0 || g_unpublished_stack_virtual != 0;
    thread_reclaim_irq_restore(flags);
    return pending;
}

bool thread_storage_in_use(const Thread *thread) {
    u64 flags = thread_reclaim_irq_save();
    bool result = thread_storage_live(thread);
    thread_reclaim_irq_restore(flags);
    return result;
}

u32 thread_object_count(void) {
    u64 flags = thread_reclaim_irq_save();
    u32 count = object_storage_count(g_thread_storage, THREAD_STORAGE_CAPACITY);
    thread_reclaim_irq_restore(flags);
    return count;
}

bool thread_test_fail_create_once(Thread *target, ThreadCreateTestFault fault, bool fail_rollback) {
    if (!target || (u32)fault > THREAD_CREATE_TEST_ATTACH ||
        (fault == THREAD_CREATE_TEST_ALLOCATE && fail_rollback)) return false;
    u64 flags = thread_reclaim_irq_save();
    bool valid = !g_create_fault.armed && !thread_storage_live(target) && !g_unpublished_stack &&
        !g_unpublished_stack_virtual && !pmm_test_free_failure_armed();
    if (valid) {
        g_create_fault.target = target;
        g_create_fault.fault = fault;
        g_create_fault.fail_rollback = fail_rollback;
        g_create_fault.armed = true;
    }
    thread_reclaim_irq_restore(flags);
    return valid;
}

bool thread_test_create_fault_armed(void) { return g_create_fault.armed; }
void thread_test_clear_create_fault(void) {
    u64 flags = thread_reclaim_irq_save();
    k_memset(&g_create_fault, 0, sizeof(g_create_fault));
    thread_reclaim_irq_restore(flags);
}
frame_t thread_test_unpublished_stack(void) {
    return g_unpublished_stack ? phys_to_frame(g_unpublished_stack) : FRAME_INVALID;
}

static bool thread_create_fault(Thread *target, ThreadCreateTestFault fault, u64 physical) {
    if (!g_create_fault.armed || g_create_fault.target != target || g_create_fault.fault != fault) return false;
    bool fail_rollback = g_create_fault.fail_rollback;
    k_memset(&g_create_fault, 0, sizeof(g_create_fault));
    if (fail_rollback) {
        /* The diagnostic checks that this exact PMM fault was consumed. */
        (void)pmm_test_fail_free_range_once(phys_to_frame(physical), THREAD_KERNEL_STACK_PAGES);
    }
    return true;
}

bool thread_prepare_kernel(Thread *thread, ThreadEntry entry, void *argument) {
    if (!g_initialized || !thread_storage_live(thread) || thread == g_current_thread || !thread->id || !entry ||
        thread->state != THREAD_STATE_READY || !thread->owns_kernel_stack) {
        return false;
    }
    if (!thread->kernel_stack_base || !thread->kernel_stack_top ||
        thread->kernel_stack_top <= thread->kernel_stack_base) {
        return false;
    }
    if (thread->kernel_stack_top & 0xFULL) return false;
    if (thread->kernel_stack_top - thread->kernel_stack_base < sizeof(u64)) return false;
    if (thread->interrupt_context_ready || thread->interrupt_rsp) return false;

    /* Reserve the normal SysV function-entry return slot for the kernel trampoline. */
    u64 initial_rsp = thread->kernel_stack_top - sizeof(u64);

    thread->entry = entry;
    thread->argument = argument;

    if (!thread_build_interrupt_context(thread, (u64)(void *) thread_kernel_trampoline,
            GDT_KERNEL_CODE_SELECTOR, 0x202ULL, initial_rsp, GDT_KERNEL_DATA_SELECTOR)) {
        thread->entry = 0;
        thread->argument = 0;
        return false;
    }
    return true;
}

bool thread_prepare_user(Thread *thread, u64 user_rip, u64 user_rsp) {
    if (!g_initialized || !thread_storage_live(thread) || thread == g_current_thread || !thread->id || !thread->process ||
        thread->state != THREAD_STATE_READY || !thread->owns_kernel_stack) {
        return false;
    }

    Process *process = thread->process;
    AddressSpace *space = process_address_space(process);

    if (!space || process->kernel || space->kernel || !address_space_cr3(space)) return false;
    if (!thread->kernel_stack_base || !thread->kernel_stack_top ||
        thread->kernel_stack_top <= thread->kernel_stack_base) {
        return false;
    }

    if (thread->kernel_stack_top & 0xFULL) return false;
    if (thread->interrupt_context_ready || thread->interrupt_rsp) return false;
    if (user_rip < ADDRESS_SPACE_USER_BASE || user_rip >= ADDRESS_SPACE_USER_LIMIT) return false;
    if (user_rsp <= ADDRESS_SPACE_USER_BASE || user_rsp > ADDRESS_SPACE_USER_LIMIT) return false;

    u64 code_page = user_rip & ~(VM_PAGE_SIZE - 1ULL);
    u64 stack_page = (user_rsp - 1ULL) & ~(VM_PAGE_SIZE - 1ULL);
    frame_t code_frame = FRAME_INVALID;
    frame_t stack_frame = FRAME_INVALID;
    vm_flags_t code_flags = 0;
    vm_flags_t stack_flags = 0;

    if (!address_space_query_page(space, code_page, &code_frame, &code_flags)) return false;
    if (!(code_flags & VM_USER) || !(code_flags & VM_EXEC) || (code_flags & VM_WRITE)) return false;
    if (!address_space_query_page(space, stack_page, &stack_frame, &stack_flags)) return false;
    if (!(stack_flags & VM_USER) || !(stack_flags & VM_WRITE) || (stack_flags & VM_EXEC)) return false;

    thread->entry = 0;
    thread->argument = 0;

    return
        thread_build_interrupt_context(thread, user_rip, GDT_USER_CODE_SELECTOR, 0x202ULL, user_rsp,
            GDT_USER_DATA_SELECTOR);
}

bool thread_system_init(Process *kernel_process, u64 bootstrap_stack_base, u64 bootstrap_stack_size,
    u64 bootstrap_stack_top) {
    if (g_initialized || !kernel_process || !kernel_process->initialized || !kernel_process->kernel ||
        !bootstrap_stack_base || !bootstrap_stack_size || !bootstrap_stack_top) {
        return false;
    }

    AddressSpace *kernel_space = process_address_space(kernel_process);

    if (!kernel_space || !kernel_space->kernel || !address_space_cr3(kernel_space)) return false;
    if (!kernel_stack_arena_init(kernel_space, bootstrap_stack_base)) return false;
    if (bootstrap_stack_base > ~0ULL - bootstrap_stack_size) return false;

    u64 stack_end = bootstrap_stack_base + bootstrap_stack_size;
    u64 stack_top = bootstrap_stack_top & ~0xFULL;

    if (stack_top <= bootstrap_stack_base || stack_top > stack_end) return false;

    k_memset(&g_bootstrap_thread, 0, sizeof(g_bootstrap_thread));

    g_bootstrap_thread.id = 1;
    g_bootstrap_thread.process = kernel_process;
    g_bootstrap_thread.state = THREAD_STATE_RUNNING;
    g_bootstrap_thread.kernel_stack_physical = bootstrap_stack_base;
    g_bootstrap_thread.kernel_stack_base = bootstrap_stack_base;
    g_bootstrap_thread.kernel_stack_top = stack_top;
    g_bootstrap_thread.kernel_stack_size = stack_top - bootstrap_stack_base;
    g_bootstrap_thread.owns_kernel_stack = false;

    if (!process_thread_attach(kernel_process, &g_bootstrap_thread)) {
        k_memset(&g_bootstrap_thread, 0, sizeof(g_bootstrap_thread));
        return false;
    }

    g_thread_storage[0] = &g_bootstrap_thread;
    g_current_thread = &g_bootstrap_thread;
    g_next_thread_id = 2;
    g_initialized = true;
    return true;
}

Thread *thread_current(void) {
    if (!g_initialized) return 0;
    return g_current_thread;
}

bool thread_activate(Thread *thread) {
    if (!g_initialized || !g_current_thread || !thread_storage_live(thread) || !thread->id || !thread->process ||
        !thread->kernel_stack_top) {
        return false;
    }

    if (thread->kernel_stack_top & 0xFULL) return false;
    AddressSpace *space = process_address_space(thread->process);
    if (!space) return false;
    u64 target_cr3 = address_space_cr3(space);

    if (!target_cr3) return false;
    if (thread == g_current_thread) {
        if (thread->state != THREAD_STATE_RUNNING) return false;

        gdt_set_rsp0(thread->kernel_stack_top);

        if ((arch_read_cr3() & ~0xFFFULL) != target_cr3) arch_write_cr3(target_cr3);
        return true;
    }

    if (thread->state != THREAD_STATE_READY) return false;
    if (g_current_thread->state != THREAD_STATE_RUNNING) return false;

    Thread *previous = g_current_thread;

    gdt_set_rsp0(thread->kernel_stack_top);

    if ((arch_read_cr3() & ~0xFFFULL) != target_cr3) arch_write_cr3(target_cr3);

    previous->state = THREAD_STATE_READY;
    thread->state = THREAD_STATE_RUNNING;

    g_current_thread = thread;
    return true;
}

static bool thread_create_locked(Thread *thread, Process *process) {
    if (!thread || thread_storage_live(thread)) return false;

    /* Existing callers may pass fresh uninitialized storage. Do not read it. */
    k_memset(thread, 0, sizeof(*thread));
    if (!g_initialized || !process || !g_next_thread_id || !kernel_stack_arena_ready() ||
        g_unpublished_stack || g_unpublished_stack_virtual) return false;
    u32 slot = object_storage_empty(g_thread_storage, THREAD_STORAGE_CAPACITY);
    if (slot == THREAD_STORAGE_CAPACITY) return false;

    AddressSpace *space = process_address_space(process);
    if (!space || !address_space_cr3(space)) return false;
    if (!pmm_phys_map_access_enabled() || !vmm_phys_map_access_enabled()) return false;
    if (thread_create_fault(thread, THREAD_CREATE_TEST_ALLOCATE, 0)) return false;

    u64 physical = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);
    if (!physical) return false;
    /* Own the unpublished allocation before the next fallible operation. */
    g_unpublished_stack = physical;
    if (thread_create_fault(thread, THREAD_CREATE_TEST_ACCESS, physical)) goto fail;
    void *direct = phys_to_virt(physical);
    if (!direct) goto fail;
    k_memset(direct, 0, (usize)THREAD_KERNEL_STACK_SIZE);

    u64 virtual_base = 0;
    if (!kernel_stack_map(slot, physical, &virtual_base)) goto fail;
    g_unpublished_stack_virtual = virtual_base;
    if (virtual_base > ~0ULL - THREAD_KERNEL_STACK_SIZE) goto fail;
    u64 virtual_top = virtual_base + THREAD_KERNEL_STACK_SIZE;
    if (virtual_top & 0xFULL) goto fail;

    thread->id = g_next_thread_id;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    thread->kernel_stack_physical = physical;
    thread->kernel_stack_base = virtual_base;
    thread->kernel_stack_top = virtual_top;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->owns_kernel_stack = true;
    if (thread_create_fault(thread, THREAD_CREATE_TEST_ATTACH, physical)) goto fail;
    if (!process_thread_attach(process, thread)) goto fail;

    /* Publication commit: no fallible operation after ownership attachment. */
    g_thread_storage[slot] = thread;
    g_unpublished_stack = 0;
    g_unpublished_stack_virtual = 0;
    ++g_next_thread_id;
    return true;

fail:
    /* If this fails, the module-owned slot retains the allocation, not 'thread'. */
    (void)thread_reclaim_unpublished_stack();
    k_memset(thread, 0, sizeof(*thread));
    return false;
}

bool thread_create(Thread *thread, Process *process) {
    u64 flags = thread_reclaim_irq_save();
    bool created = thread_create_locked(thread, process);
    thread_reclaim_irq_restore(flags);
    return created;
}

static bool wait_kind_valid(ThreadWaitKind kind) {
    return kind == THREAD_WAIT_IPC_RECEIVE || kind == THREAD_WAIT_IPC_SEND;
}

static bool wait_terminal(ThreadWaitResult result) {
    return result == THREAD_WAIT_RESULT_COMPLETED || result == THREAD_WAIT_RESULT_CANCELLED ||
        result == THREAD_WAIT_RESULT_PEER_CLOSED || result == THREAD_WAIT_RESULT_TIMED_OUT;
}

static void wait_clear(Thread *thread) {
    thread->wait_kind = THREAD_WAIT_NONE;
    thread->wait_result = THREAD_WAIT_RESULT_NONE;
    thread->wait_object = 0;
    thread->wait_id = 0;
}

bool thread_wait_begin(Thread *thread, ThreadWaitKind kind, void *object) {
    /* Caller holds local IRQ exclusion through reverse-link publication/park. */
    if (!g_initialized || !thread_storage_live(thread) || thread != g_current_thread ||
        !thread->id || !object || !wait_kind_valid(kind) || !g_next_wait_id) return false;
    if (thread->state != THREAD_STATE_RUNNING || !thread->on_run_queue) return false;
    if (thread_wait_active(thread)) return false;

    thread->wait_kind = kind;
    thread->wait_result = THREAD_WAIT_RESULT_PENDING;
    thread->wait_object = object;
    thread->wait_id = g_next_wait_id++;
    /* Unsigned wrap leaves zero: subsequent registrations fail closed. */
    return true;
}

bool thread_wait_matches(const Thread *thread, ThreadWaitKind kind, const void *object) {
    if (!g_initialized || !thread_storage_live(thread) || !thread->id || !object ||
        !wait_kind_valid(kind) || !thread->wait_id) return false;
    return thread->wait_kind == kind && thread->wait_object == object;
}

bool thread_wait_matches_id(const Thread *thread, ThreadWaitKind kind, const void *object, u64 wait_id) {
    return wait_id && thread_wait_matches(thread, kind, object) && thread->wait_id == wait_id;
}

bool thread_wait_try_complete(Thread *thread, ThreadWaitKind kind, const void *object,
    u64 wait_id, ThreadWaitResult result) {
    u64 flags = thread_reclaim_irq_save();
    bool changed = false;
    if (!wait_terminal(result) || !thread_wait_matches_id(thread, kind, object, wait_id) ||
        thread->wait_result != THREAD_WAIT_RESULT_PENDING) goto done;
    if (thread->state != THREAD_STATE_BLOCKED && thread->state != THREAD_STATE_READY &&
        !(thread == g_current_thread && thread->state == THREAD_STATE_RUNNING)) goto done;
    thread->wait_result = result;
    changed = true;
done:
    thread_reclaim_irq_restore(flags);
    return changed;
}

bool thread_wait_cancel(Thread *thread, ThreadWaitKind kind, void *object) {
    /* Synchronous compatibility helper; delayed owners must supply their token. */
    if (!thread_wait_matches(thread, kind, object)) return false;
    return thread_wait_try_complete(thread, kind, object, thread->wait_id, THREAD_WAIT_RESULT_CANCELLED);
}

bool thread_wait_cancelled(const Thread *thread, ThreadWaitKind kind, const void *object) {
    if (!thread_wait_matches(thread, kind, object)) return false;
    return thread->wait_result == THREAD_WAIT_RESULT_CANCELLED ||
        thread->wait_result == THREAD_WAIT_RESULT_PEER_CLOSED;
}

bool thread_wait_end(Thread *thread, ThreadWaitKind kind, void *object) {
    if (thread != g_current_thread || !thread_wait_matches(thread, kind, object) ||
        thread->state != THREAD_STATE_RUNNING || !wait_terminal(thread->wait_result)) return false;
    wait_clear(thread);
    return true;
}

bool thread_wait_abort(Thread *thread, ThreadWaitKind kind, void *object) {
    if (thread == g_current_thread || !thread_wait_matches(thread, kind, object)) return false;
    if (thread->state != THREAD_STATE_BLOCKED && thread->state != THREAD_STATE_READY) return false;
    /* This call chain will never resume. Do not change an already committed result. */
    if (thread->wait_result == THREAD_WAIT_RESULT_PENDING) thread->wait_result = THREAD_WAIT_RESULT_CANCELLED;
    if (!wait_terminal(thread->wait_result)) return false;
    wait_clear(thread);
    return true;
}

bool thread_wait_active(const Thread *thread) {
    if (!g_initialized || !thread_storage_live(thread) || !thread->id) return false;
    return thread->wait_kind != THREAD_WAIT_NONE || thread->wait_result != THREAD_WAIT_RESULT_NONE ||
        thread->wait_object != 0 || thread->wait_id != 0;
}

static bool thread_destroy_locked(Thread *thread) {
    u32 storage_slot = object_storage_find(g_thread_storage, THREAD_STORAGE_CAPACITY, thread);
    if (storage_slot == THREAD_STORAGE_CAPACITY) return false;
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !thread->process ||
        thread->state == THREAD_STATE_INVALID || thread->state == THREAD_STATE_RUNNING ||
        thread->state == THREAD_STATE_BLOCKED || thread->on_run_queue || thread->run_next) {
        return false;
    }

    /* Another kernel object still owns a reference to this Thread. */
    if (thread_wait_active(thread) || thread->capability_refs) return false;

    Process *process = thread->process;

    /* Refuse to start destruction if the process ownership accounting is already invalid. */
    if (!process_thread_can_detach(process, thread)) return false;

    if (thread->owns_kernel_stack &&
        !kernel_stack_release(thread->kernel_stack_physical, thread->kernel_stack_base)) return false;

    /*
    * Resource release succeeded.
    *
    * Failure of a prevalidated ownership unlink at
    * this point is an internal kernel invariant
    * failure.
    */
    if (!process_thread_detach(process, thread)) cpu_halt_forever();    

    g_thread_storage[storage_slot] = 0;
    thread->process = 0;
    thread->process_prev = 0;
    thread->process_next = 0;
    thread->entry = 0;
    thread->argument = 0;
    thread->kernel_stack_physical = 0;
    thread->kernel_stack_base = 0;
    thread->kernel_stack_top = 0;
    thread->kernel_stack_size = 0;
    thread->owns_kernel_stack = false;
    thread->run_next = 0;
    thread->on_run_queue = false;
    thread->id = 0;
    thread->state = THREAD_STATE_DEAD;
    thread->interrupt_rsp = 0;
    thread->interrupt_context_ready = false;
    thread->wait_kind = THREAD_WAIT_NONE;
    thread->wait_result = THREAD_WAIT_RESULT_NONE;
    thread->wait_object = 0;
    thread->wait_id = 0;
    return true;
}

bool thread_destroy(Thread *thread) {
    u64 flags = thread_reclaim_irq_save();
    bool destroyed = thread_destroy_locked(thread);
    thread_reclaim_irq_restore(flags);
    return destroyed;
}
