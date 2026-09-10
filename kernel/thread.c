#include "thread.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"
#include "arch.h"
#include "gdt.h"
#include "interrupts.h"

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

static bool release_kernel_stack(u64 physical, u64 size) {
    if (!physical || !size) return false;
    if (physical & (FRAME_SIZE - 1ULL)) return false;
    if (size & (FRAME_SIZE - 1ULL)) return false;

    frame_t first = phys_to_frame(physical);

    if (first == FRAME_INVALID) return false;

    u64 pages = size / FRAME_SIZE;
    bool result = true;

    for (u64 i = 0; i < pages; ++i) {
        if (!frame_free(first + i)) result = false;
    }

    return result;
}

bool thread_prepare_kernel(Thread *thread, ThreadEntry entry, void *argument) {
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !entry ||
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
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !thread->process ||
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
    if (!(code_flags & VM_USER)) return false;
    if (!address_space_query_page(space, stack_page, &stack_frame, &stack_flags)) return false;
    if (!(stack_flags & VM_USER) || !(stack_flags & VM_WRITE)) return false;

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
    if (bootstrap_stack_base > ~0ULL - bootstrap_stack_size) return false;

    u64 stack_end = bootstrap_stack_base + bootstrap_stack_size;
    u64 stack_top = bootstrap_stack_top & ~0xFULL;

    if (stack_top <= bootstrap_stack_base || stack_top > stack_end) return false;

    k_memset(&g_bootstrap_thread, 0, sizeof(g_bootstrap_thread));

    /* Adopt the boot thread as the first thread owned by the kernel process. */
    if (!process_thread_attach(kernel_process)) return false;

    g_bootstrap_thread.id = 1;
    g_bootstrap_thread.process = kernel_process;
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
    if (!g_initialized || !g_current_thread || !thread || !thread->id || !thread->process ||
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

bool thread_create(Thread *thread, Process *process) {
    if (!g_initialized || !thread || thread == g_current_thread || !process || !process->initialized ||
        !g_next_thread_id) {
        return false;
    }

    AddressSpace *space = process_address_space(process);

    if (!space || !address_space_cr3(space)) return false;
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

    /* From this point onward the process owns one additional Thread object. */
    if (!process_thread_attach(process)) {
        (void)release_kernel_stack(physical, THREAD_KERNEL_STACK_SIZE);
        return false;
    }

    thread->id = g_next_thread_id++;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    thread->kernel_stack_physical = physical;
    thread->kernel_stack_base = virtual_base;
    thread->kernel_stack_top = virtual_top;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->owns_kernel_stack = true;
    return true;
}

bool thread_destroy(Thread *thread) {
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || !thread->process ||
        thread->state == THREAD_STATE_INVALID || thread->state == THREAD_STATE_RUNNING ||
        thread->on_run_queue) {
        return false;
    }

    Process *process = thread->process;

    /* Refuse to start destruction if the process ownership accounting is already invalid. */
    if (!process_thread_count(process)) return false;
    if (!process_thread_detach(process)) return false;

    bool result = true;

    if (thread->owns_kernel_stack) {
        result = release_kernel_stack(thread->kernel_stack_physical, thread->kernel_stack_size);
    }

    thread->process = 0;
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
    return result;
}