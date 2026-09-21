#include "user_memory.h"

#include "address_space.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"

bool user_memory_read(const Process *process, u64 user_address, void *destination, u64 size) {
    if (!process || process->kernel || !destination || !size ||
        user_address < ADDRESS_SPACE_USER_BASE || user_address >= ADDRESS_SPACE_USER_LIMIT ||
        size > ADDRESS_SPACE_USER_LIMIT - user_address) return false;

    const AddressSpace *space = process->address_space;
    if (!space || space->kernel) return false;

    u8 *out = destination;
    u64 remaining = size;
    u64 address = user_address;
    while (remaining) {
        u64 page = address & ~(VM_PAGE_SIZE - 1ULL);
        u64 offset = address - page;
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        if (!address_space_query_page(space, page, &frame, &flags) ||
            frame == FRAME_INVALID || !(flags & VM_USER)) return false;
        const u8 *source = (const u8 *)phys_to_virt(frame_to_phys(frame));
        if (!source) return false;
        u64 chunk = VM_PAGE_SIZE - offset;
        if (chunk > remaining) chunk = remaining;
        k_memcpy(out, source + offset, (usize)chunk);
        out += chunk;
        address += chunk;
        remaining -= chunk;
    }
    return true;
}
