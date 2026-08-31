#ifndef JA_OS_PHYSMAP_H
#define JA_OS_PHYSMAP_H

#include "types.h"

/*
 * Higher-half physical-map window.
 *
 * PHYS_MAP_BASE + physical yields the corresponding
 * virtual address for physical addresses below 128 TiB.
 *
 * This only computes an address; the physical range must
 * also have been mapped into the active page tables.
 */
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define PHYS_MAP_SIZE 0x0000800000000000ULL
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define PHYS_MAP_SIZE 0x0000800000000000ULL

bool physmap_virtual_address(u64 physical, u64 *virtual_out);

void *phys_to_virt(u64 physical);

bool virt_to_phys(const void *virtual_address, u64 *physical_out);

#endif