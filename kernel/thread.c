#include "thread.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"

static Thread g_bootstrap_thread;
static Thread *g_current_thread;

static u64 g_next_thread_id;
static bool g_initialized;

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
    if (!g_initialized || !thread || thread == g_current_thread || !thread->id || thread->state == THREAD_STATE_INVALID || thread->state == THREAD_STATE_DEAD) return false;

    bool result = true;

    if (thread->owns_kernel_stack) result = release_kernel_stack(thread->kernel_stack_physical, thread->kernel_stack_size);

    thread->address_space = 0;
    thread->kernel_stack_physical = 0;
    thread->kernel_stack_base = 0;
    thread->kernel_stack_top = 0;
    thread->kernel_stack_size = 0;
    thread->owns_kernel_stack = false;
    thread->state = THREAD_STATE_DEAD;

    return result;
}