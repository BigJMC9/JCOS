#ifndef JA_OS_VMM_H
#define JA_OS_VMM_H

#include "types.h"
#include "pmm.h"

#define VM_PAGE_SIZE 4096ULL

typedef u64 vm_flags_t;

/*
 * Public mapping permissions.
 *
 * PRESENT is intentionally not public:
 * if vmm_map_page() succeeds, the page is present.
 */
#define VM_WRITE (1ULL << 0)
#define VM_USER  (1ULL << 1)

typedef struct {
    /*
     * Physical frame containing the PML4.
     *
     * This page map owns all of its page-table
     * frames, but NOT frames mapped by leaf PTEs.
     */
    frame_t root_frame;
} VmPageMap;

bool vmm_page_map_create(
    VmPageMap *map
);

void vmm_page_map_destroy(
    VmPageMap *map
);

bool vmm_map_page(
    VmPageMap *map,
    u64 virtual_address,
    frame_t frame,
    vm_flags_t flags
);

bool vmm_unmap_page(
    VmPageMap *map,
    u64 virtual_address,
    frame_t *old_frame
);

bool vmm_query_page(
    const VmPageMap *map,
    u64 virtual_address,
    frame_t *frame,
    vm_flags_t *flags
);

#endif