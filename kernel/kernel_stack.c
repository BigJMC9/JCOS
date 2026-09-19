#include "kernel_stack.h"

#include "arch.h"
#include "object_storage.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"

#define KERNEL_STACK_GUARD_PAGES 1ULL
#define KERNEL_STACK_SLOT_PAGES (THREAD_KERNEL_STACK_PAGES + 2ULL * KERNEL_STACK_GUARD_PAGES)
#define KERNEL_STACK_SLOT_SIZE (KERNEL_STACK_SLOT_PAGES * VM_PAGE_SIZE)
#define KERNEL_STACK_ARENA_SIZE 0x01000000ULL
#define KERNEL_STACK_ARENA_BASE (ADDRESS_SPACE_USER_BASE - KERNEL_STACK_ARENA_SIZE)
#define KERNEL_STACK_PT_SPAN 0x200000ULL

_Static_assert(FRAME_SIZE == VM_PAGE_SIZE, "kernel stack frame/page size mismatch");
_Static_assert((KERNEL_STACK_ARENA_BASE & (KERNEL_STACK_PT_SPAN - 1ULL)) == 0,
    "kernel stack arena must start on a page-table span");
_Static_assert((u64)THREAD_STORAGE_CAPACITY * KERNEL_STACK_SLOT_SIZE <= KERNEL_STACK_ARENA_SIZE,
    "kernel stack arena too small for thread storage capacity");

static AddressSpace *g_kernel_space;
static bool g_arena_ready;

static bool kernel_map_active(void) {
    return g_kernel_space && address_space_cr3(g_kernel_space) &&
        (arch_read_cr3() & ~0xFFFULL) == address_space_cr3(g_kernel_space);
}

static bool slot_stack_base(u32 slot, u64 *base_out) {
    if (!base_out || !slot || slot >= THREAD_STORAGE_CAPACITY) return false;
    u64 slot_offset = (u64)slot * KERNEL_STACK_SLOT_SIZE;
    if (slot_offset > KERNEL_STACK_ARENA_SIZE - KERNEL_STACK_SLOT_SIZE) return false;
    u64 slot_base = KERNEL_STACK_ARENA_BASE + slot_offset;
    *base_out = slot_base + VM_PAGE_SIZE;
    return true;
}

static bool page_absent(u64 address) {
    return g_kernel_space && !address_space_query_page(g_kernel_space, address, 0, 0);
}

static bool stack_base_valid(u64 stack_base) {
    if (!stack_base || (stack_base & (VM_PAGE_SIZE - 1ULL))) return false;
    if (stack_base < KERNEL_STACK_ARENA_BASE + VM_PAGE_SIZE) return false;
    u64 slot_start = stack_base - VM_PAGE_SIZE;
    if (slot_start < KERNEL_STACK_ARENA_BASE) return false;
    u64 offset = slot_start - KERNEL_STACK_ARENA_BASE;
    if (offset % KERNEL_STACK_SLOT_SIZE) return false;
    u64 slot = offset / KERNEL_STACK_SLOT_SIZE;
    return slot > 0 && slot < THREAD_STORAGE_CAPACITY;
}

static bool slot_clear(u64 stack_base) {
    if (!g_kernel_space || !stack_base_valid(stack_base)) return false;
    if (!page_absent(stack_base - VM_PAGE_SIZE)) return false;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        if (!page_absent(stack_base + i * VM_PAGE_SIZE)) return false;
    }
    return page_absent(stack_base + THREAD_KERNEL_STACK_SIZE);
}

bool kernel_stack_mapping_valid(u64 physical, u64 stack_base) {
    if (!g_arena_ready || !g_kernel_space || !physical || !stack_base_valid(stack_base)) return false;
    if (physical & (FRAME_SIZE - 1ULL)) return false;
    if (!page_absent(stack_base - VM_PAGE_SIZE) ||
        !page_absent(stack_base + THREAD_KERNEL_STACK_SIZE)) return false;

    frame_t first = phys_to_frame(physical);
    if (first == FRAME_INVALID) return false;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        if (!address_space_query_page(g_kernel_space, stack_base + i * VM_PAGE_SIZE, &frame, &flags)) return false;
        if (frame != first + i) return false;
        if ((flags & (VM_WRITE | VM_USER | VM_EXEC)) != VM_WRITE) return false;
    }
    return true;
}

bool kernel_stack_virtual_released(u64 stack_base) {
    return g_arena_ready && slot_clear(stack_base);
}

static bool restore_mapping(u64 physical, u64 stack_base) {
    frame_t first = phys_to_frame(physical);
    if (first == FRAME_INVALID) return false;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        u64 address = stack_base + i * VM_PAGE_SIZE;
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        if (address_space_query_page(g_kernel_space, address, &frame, &flags)) {
            if (frame != first + i || (flags & (VM_WRITE | VM_USER | VM_EXEC)) != VM_WRITE) return false;
            continue;
        }
        if (!vmm_map_page(&g_kernel_space->page_map, address, first + i, VM_WRITE)) return false;
    }
    return kernel_stack_mapping_valid(physical, stack_base);
}

static bool unmap_mapping(u64 physical, u64 stack_base) {
    if (!kernel_stack_mapping_valid(physical, stack_base)) return false;
    frame_t first = phys_to_frame(physical);
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        frame_t old = FRAME_INVALID;
        if (!vmm_unmap_page(&g_kernel_space->page_map, stack_base + i * VM_PAGE_SIZE, &old)) {
            if (!restore_mapping(physical, stack_base)) cpu_halt_forever();
            return false;
        }
        if (old != first + i) cpu_halt_forever();
    }
    return kernel_stack_virtual_released(stack_base);
}

bool kernel_stack_arena_init(AddressSpace *kernel_space, u64 probe_physical) {
    if (g_arena_ready) return kernel_space == g_kernel_space;
    if (!kernel_space || !kernel_space->kernel || !address_space_cr3(kernel_space)) return false;
    if ((arch_read_cr3() & ~0xFFFULL) != address_space_cr3(kernel_space)) return false;
    frame_t probe = phys_to_frame(probe_physical & ~(FRAME_SIZE - 1ULL));
    if (probe == FRAME_INVALID) return false;

    g_kernel_space = kernel_space;
    u64 used = (u64)THREAD_STORAGE_CAPACITY * KERNEL_STACK_SLOT_SIZE;
    u64 end = KERNEL_STACK_ARENA_BASE + used;
    for (u64 address = KERNEL_STACK_ARENA_BASE; address < end; address += KERNEL_STACK_PT_SPAN) {
        if (!page_absent(address)) return false;
        if (!vmm_map_page(&kernel_space->page_map, address, probe, VM_WRITE)) return false;
        frame_t old = FRAME_INVALID;
        if (!vmm_unmap_page(&kernel_space->page_map, address, &old) || old != probe) return false;
        if (!page_absent(address)) return false;
    }

    g_arena_ready = true;
    return true;
}

bool kernel_stack_arena_ready(void) {
    return g_arena_ready;
}

bool kernel_stack_map(u32 storage_slot, u64 physical, u64 *virtual_base_out) {
    if (virtual_base_out) *virtual_base_out = 0;
    if (!g_arena_ready || !kernel_map_active() || !physical || !virtual_base_out) return false;
    if (physical & (FRAME_SIZE - 1ULL)) return false;

    u64 stack_base = 0;
    if (!slot_stack_base(storage_slot, &stack_base) || !slot_clear(stack_base)) return false;
    frame_t first = phys_to_frame(physical);
    if (first == FRAME_INVALID) return false;

    u64 mapped = 0;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        if (!vmm_map_page(&g_kernel_space->page_map, stack_base + i * VM_PAGE_SIZE, first + i, VM_WRITE)) {
            while (mapped) {
                --mapped;
                frame_t old = FRAME_INVALID;
                if (!vmm_unmap_page(&g_kernel_space->page_map,
                        stack_base + mapped * VM_PAGE_SIZE, &old) || old != first + mapped) {
                    cpu_halt_forever();
                }
            }
            return false;
        }
        ++mapped;
    }

    if (!kernel_stack_mapping_valid(physical, stack_base)) cpu_halt_forever();
    *virtual_base_out = stack_base;
    return true;
}

bool kernel_stack_discard_unpublished(u64 physical, u64 stack_base) {
    if (!physical || (physical & (FRAME_SIZE - 1ULL))) return false;
    if (stack_base) {
        if (!kernel_map_active()) return false;
        if (!kernel_stack_virtual_released(stack_base) && !unmap_mapping(physical, stack_base)) return false;
    }
    frame_t first = phys_to_frame(physical);
    return first != FRAME_INVALID && frame_free_range(first, THREAD_KERNEL_STACK_PAGES);
}

bool kernel_stack_release(u64 physical, u64 stack_base) {
    if (!g_arena_ready || !kernel_map_active() || !physical || !stack_base) return false;
    if (!unmap_mapping(physical, stack_base)) return false;

    frame_t first = phys_to_frame(physical);
    if (first == FRAME_INVALID) cpu_halt_forever();
    if (frame_free_range(first, THREAD_KERNEL_STACK_PAGES)) return true;

    /* Destruction is retryable: restore the exact guarded mapping if PMM rejects
       the release (including the existing injected-failure diagnostics). */
    if (!restore_mapping(physical, stack_base)) cpu_halt_forever();
    return false;
}
