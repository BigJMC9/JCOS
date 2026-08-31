#include "thread.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"
#include "arch.h"
#include "gdt.h"

static Thread g_bootstrap_thread;
static Thread *g_current_thread;

static u64 g_next_thread_id;
static bool g_initialized;

static NORETURN void thread_kernel_trampoline(void) {
    Thread *thread = thread_current();

    if (!thread || !thread->entry) cpu_halt_forever();

    ThreadEntry entry = thread->entry;
    void *argument = thread->argument;
    entry(argument);

    /* Kernel-thread entries must currently terminate explicitly. */
    cpu_halt_forever();
}

static NORETURN void thread_user_trampoline(void) {
    Thread *thread = thread_current();

    if (!thread || !thread->user_rip || !thread->user_rsp) cpu_halt_forever();
    arch_enter_user(thread->user_rip, thread->user_rsp);
}

bool thread_prepare_kernel(Thread *thread, ThreadEntry entry, void *argument) {
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !entry || thread->state != THREAD_STATE_READY || !thread->owns_kernel_stack || thread->context_ready) return false;
    if (!thread->kernel_stack_base || !thread->kernel_stack_top || thread->kernel_stack_top <= thread->kernel_stack_base) return false;
    if (thread->kernel_stack_top & 0xFULL) return false;
    if (thread->kernel_stack_top - thread->kernel_stack_base < sizeof(u64)) return false;

    k_memset(&thread->context, 0, sizeof(thread->context));

    thread->entry = entry;
    thread->argument = argument;

    /*
     * A normal SysV function begins with
     * RSP % 16 == 8 because CALL pushed a
     * return address.
     *
     * Enter with JMP, so reserve a dummy
     * return slot ourselves.
     */
    u64 initial_rsp = thread->kernel_stack_top - sizeof(u64);

    *(u64 *)(u64)initial_rsp = 0;

    thread->context.rsp = initial_rsp;
    thread->context.rip = (u64)(void *) thread_kernel_trampoline;
    thread->user_rip = 0;
    thread->user_rsp = 0;
    thread->context_ready = true;

    return true;
}

bool thread_prepare_user(Thread *thread, u64 user_rip, u64 user_rsp) {
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !thread->address_space || thread->address_space->kernel || thread->state != THREAD_STATE_READY || !thread->owns_kernel_stack || thread->context_ready) return false;
    if (!thread->kernel_stack_base || !thread->kernel_stack_top || thread->kernel_stack_top <= thread->kernel_stack_base) return false;
    if (thread->kernel_stack_top & 0xFULL) return false;
    if (user_rip < ADDRESS_SPACE_USER_BASE || user_rip >= ADDRESS_SPACE_USER_LIMIT) return false;
    if (user_rsp <= ADDRESS_SPACE_USER_BASE || user_rsp > ADDRESS_SPACE_USER_LIMIT) return false;

    u64 code_page = user_rip & ~(VM_PAGE_SIZE - 1ULL);
    u64 stack_page = (user_rsp - 1ULL) & ~(VM_PAGE_SIZE - 1ULL);

    frame_t code_frame = FRAME_INVALID;
    frame_t stack_frame = FRAME_INVALID;

    vm_flags_t code_flags = 0;
    vm_flags_t stack_flags = 0;

    if (!address_space_query_page(thread->address_space, code_page, &code_frame, &code_flags)) return false;
    if (!(code_flags & VM_USER)) return false;
    if (!address_space_query_page(thread->address_space, stack_page, &stack_frame, &stack_flags)) return false;
    if (!(stack_flags & VM_USER) || !(stack_flags & VM_WRITE)) return false;

    k_memset(&thread->context, 0, sizeof(thread->context));

    thread->entry = 0;
    thread->argument = 0;
    thread->user_rip = user_rip;
    thread->user_rsp = user_rsp;

    /* We JMP into the trampoline rather than CALL it, so synthesize the usual SysV entry alignment. */
    u64 initial_rsp = thread->kernel_stack_top - sizeof(u64);

    *(u64 *)(u64)initial_rsp = 0;

    thread->context.rsp = initial_rsp;
    thread->context.rip = (u64)(void *) thread_user_trampoline;
    thread->context_ready = true;

    return true;
}

static bool release_kernel_stack(u64 physical, u64 size) {
    if (!physical || !size || (physical & (FRAME_SIZE - 1ULL)) || (size & (FRAME_SIZE - 1ULL))) return false;

    frame_t first = phys_to_frame(physical);

    if (first == FRAME_INVALID) return false;

    u64 pages = size / FRAME_SIZE;
    bool result = true;

    for (u64 i = 0; i < pages; ++i) {
        if (!frame_free(first + i)) result = false;
    }

    return result;
}

bool thread_system_init(AddressSpace *kernel_space, u64 bootstrap_stack_base, u64 bootstrap_stack_size, u64 bootstrap_stack_top) {
    if (g_initialized || !kernel_space || !kernel_space->kernel || !address_space_cr3(kernel_space) || !bootstrap_stack_base || !bootstrap_stack_size || !bootstrap_stack_top) return false;
    if (bootstrap_stack_base > ~0ULL - bootstrap_stack_size) return false;

    u64 stack_end = bootstrap_stack_base + bootstrap_stack_size;
    u64 stack_top = bootstrap_stack_top & ~0xFULL;

    if (stack_top <= bootstrap_stack_base || stack_top > stack_end) return false;

    k_memset(&g_bootstrap_thread, 0, sizeof(g_bootstrap_thread));

    /* The boot thread runs on the loader-provided identity-mapped kernel stack. */
    g_bootstrap_thread.id = 1;
    g_bootstrap_thread.address_space = kernel_space;
    g_bootstrap_thread.state = THREAD_STATE_RUNNING;
    g_bootstrap_thread.kernel_stack_physical = bootstrap_stack_base;
    g_bootstrap_thread.kernel_stack_base = bootstrap_stack_base;
    g_bootstrap_thread.kernel_stack_top = stack_top;
    g_bootstrap_thread.kernel_stack_size = stack_top - bootstrap_stack_base;
    g_bootstrap_thread.owns_kernel_stack = false;
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
    if (!g_initialized || !g_current_thread || !thread || !thread->id || !thread->address_space || !thread->kernel_stack_top) return false;
    if (thread->kernel_stack_top & 0xFULL) return false;

    u64 target_cr3 = address_space_cr3(thread->address_space);
    if (!target_cr3) return false;

    /* Re-activating the current thread is allowed. It also repairs CR3/RSP0 if necessary. */
    if (thread == g_current_thread) {
        if (thread->state != THREAD_STATE_RUNNING) return false;
        gdt_set_rsp0(thread->kernel_stack_top);

        if ((arch_read_cr3() & ~0xFFFULL) != target_cr3) arch_write_cr3(target_cr3);
        return true;
    }

    /* Only READY threads may become current. */
    if (thread->state != THREAD_STATE_READY) return false;
    if (g_current_thread->state != THREAD_STATE_RUNNING) return false;

    Thread *previous = g_current_thread;

    /* Install the hardware state first while interrupts are disabled. */
    gdt_set_rsp0(thread->kernel_stack_top);
    if ((arch_read_cr3() & ~0xFFFULL) != target_cr3) arch_write_cr3(target_cr3);

    /* The hardware context now belongs to the target thread. */
    previous->state = THREAD_STATE_READY;
    thread->state = THREAD_STATE_RUNNING;
    g_current_thread = thread;

    return true;
}

bool thread_switch(Thread *next) {
    if (!g_initialized || !g_current_thread || !next || !next->id) return false;

    Thread *previous = g_current_thread;

    if (next == previous) return true;
    if (!next->context_ready || next->state != THREAD_STATE_READY) return false;

    /*
     * Installs:
     *
     *   current thread
     *   CR3
     *   TSS.RSP0
     *   thread states
     *
     * Interrupts must already be disabled.
     */
    if (!thread_activate(next)) return false;

    /* The outgoing context becomes resumable once arch_context_switch stores it. */
    previous->context_ready = true;

    arch_context_switch(&previous->context, &next->context);

    /* Reached only when another thread later switches back to this one. */
    return true;
}

bool thread_create(Thread *thread, AddressSpace *address_space) {
    if (!g_initialized || !thread || thread == g_current_thread || !address_space || !address_space_cr3(address_space) || !g_next_thread_id) return false;

    /* New stacks use the physmap, so thread creation requires the paging migration to have completed. */
    if (!pmm_phys_map_access_enabled() || !vmm_phys_map_access_enabled()) return false;

    k_memset(thread, 0, sizeof(*thread));
    u64 physical = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);

    if (!physical) return false;

    void *direct = phys_to_virt(physical);

    if (!direct) {
        (void)release_kernel_stack(physical, THREAD_KERNEL_STACK_SIZE);
        return false;
    }

    u64 virtual_base = (u64)(void *)direct;

    if (virtual_base > ~0ULL - THREAD_KERNEL_STACK_SIZE) {
        (void)release_kernel_stack(physical, THREAD_KERNEL_STACK_SIZE);
        return false;
    }

    u64 virtual_top = virtual_base + THREAD_KERNEL_STACK_SIZE;

    if (virtual_top & 0xFULL) {
        (void)release_kernel_stack(physical, THREAD_KERNEL_STACK_SIZE);
        return false;
    }

    k_memset(direct, 0, (usize)THREAD_KERNEL_STACK_SIZE);

    thread->id = g_next_thread_id++;
    thread->address_space = address_space;
    thread->state = THREAD_STATE_READY;
    thread->kernel_stack_physical = physical;
    thread->kernel_stack_base = virtual_base;
    thread->kernel_stack_top = virtual_top;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->owns_kernel_stack = true;

    return true;
}

bool thread_destroy(Thread *thread) {
    if (!g_initialized || 
        !thread || 
        thread == g_current_thread || 
        !thread->id || 
        thread->state == THREAD_STATE_INVALID || 
        thread->state == THREAD_STATE_RUNNING || 
        thread->on_run_queue) {
        return false;
    }

    bool result = true;
    if (thread->owns_kernel_stack) {
        result = release_kernel_stack(thread->kernel_stack_physical, thread->kernel_stack_size);
    }

    k_memset(&thread->context, 0, sizeof(thread->context));

    thread->address_space = 0;
    thread->entry = 0;
    thread->argument = 0;
    thread->kernel_stack_physical = 0;
    thread->kernel_stack_base = 0;
    thread->kernel_stack_top = 0;
    thread->kernel_stack_size = 0;
    thread->owns_kernel_stack = false;
    thread->context_ready = false;
    thread->run_next = 0;
    thread->on_run_queue = false;
    thread->id = 0;
    thread->state = THREAD_STATE_DEAD;

    return result;
}