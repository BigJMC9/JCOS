#include "user_stack.h"
#include "vmm.h"

static bool stack_base_valid(u64 stack_base) {
    if (stack_base & (VM_PAGE_SIZE - 1ULL)) return false;
    if (stack_base < ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE) return false;
    if (stack_base > ADDRESS_SPACE_USER_LIMIT - 2ULL * VM_PAGE_SIZE) return false;
    return true;
}

static bool page_absent(const AddressSpace *space, u64 address) {
    return space && !address_space_query_page(space, address, 0, 0);
}

bool user_stack_initial_reservation_conflicts(u64 page_first, u64 page_end) {
    if ((page_first & (VM_PAGE_SIZE - 1ULL)) || (page_end & (VM_PAGE_SIZE - 1ULL)) ||
        page_first >= page_end) return true;

    const u64 reserved_first = USER_STACK_INITIAL_BASE - VM_PAGE_SIZE;
    const u64 reserved_end = USER_STACK_INITIAL_BASE + 2ULL * VM_PAGE_SIZE;
    return page_first < reserved_end && reserved_first < page_end;
}

bool user_stack_slot_available(const AddressSpace *space, u64 stack_base) {
    if (!space || space->kernel || !stack_base_valid(stack_base)) return false;
    return page_absent(space, stack_base - VM_PAGE_SIZE) &&
        page_absent(space, stack_base) &&
        page_absent(space, stack_base + VM_PAGE_SIZE);
}

bool user_stack_mapping_valid(const AddressSpace *space, u64 stack_base, frame_t frame) {
    if (!space || space->kernel || frame == FRAME_INVALID || !stack_base_valid(stack_base)) return false;
    if (!page_absent(space, stack_base - VM_PAGE_SIZE) ||
        !page_absent(space, stack_base + VM_PAGE_SIZE)) return false;

    frame_t mapped = FRAME_INVALID;
    vm_flags_t flags = 0;
    if (!address_space_query_page(space, stack_base, &mapped, &flags) || mapped != frame) return false;
    return (flags & VM_USER) && (flags & VM_WRITE) && !(flags & VM_EXEC);
}

bool user_stack_map_page(AddressSpace *space, u64 stack_base, frame_t frame) {
    if (frame == FRAME_INVALID || !user_stack_slot_available(space, stack_base)) return false;
    if (!address_space_map_page(space, stack_base, frame, VM_WRITE)) return false;
    if (user_stack_mapping_valid(space, stack_base, frame)) return true;

    frame_t old = FRAME_INVALID;
    (void)address_space_unmap_page(space, stack_base, &old);
    return false;
}
