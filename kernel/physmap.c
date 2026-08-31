#include "physmap.h"

bool physmap_virtual_address(u64 physical, u64 *virtual_out) {
    if (!virtual_out) return false;
    if (physical >= PHYS_MAP_SIZE) return false;

    *virtual_out = PHYS_MAP_BASE + physical;

    return true;
}

void *phys_to_virt(u64 physical) {
    u64 virtual_address = 0;

    if (!physmap_virtual_address(physical, &virtual_address)) return 0;

    return
        (void *)(u64)virtual_address;
}

bool virt_to_phys(const void *virtual_address, u64 *physical_out) {
    if (!virtual_address || !physical_out) return false;

    u64 address = (u64)virtual_address;

    if (address < PHYS_MAP_BASE) return false;

    u64 offset = address - PHYS_MAP_BASE;

    if (offset >= PHYS_MAP_SIZE) return false;

    *physical_out = offset;

    return true;
}