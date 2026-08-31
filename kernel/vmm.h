#ifndef JA_OS_VMM_H
#define JA_OS_VMM_H

#include "types.h"
#include "pmm.h"

#define VM_PAGE_SIZE 4096ULL
#define VM_PML4_ENTRY_COUNT 512U

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
    frame_t root_frame;

    /*
     * PML4 entries in [first, end) are owned by
     * this map. Shared entries are never freed.
     */
    u16 owned_pml4_first;
    u16 owned_pml4_end;
} VmPageMap;

bool vmm_page_map_create(VmPageMap *map);

void vmm_page_map_destroy(VmPageMap *map);

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

bool vmm_map_range(
    VmPageMap *map, 
    u64 virtual_address, 
    u64 physical_address,
    u64 size, 
    vm_flags_t flags
);

bool vmm_identity_map_range(
    VmPageMap *map, 
    u64 physical_address, 
    u64 size, 
    vm_flags_t flags
);

bool vmm_page_map_create_owned_range(
    VmPageMap *map,
    u16 owned_first,
    u16 owned_end
);

bool vmm_page_map_share_pml4_entry(
    VmPageMap *destination,
    const VmPageMap *source,
    u16 index
);

void vmm_enable_phys_map_access(void);
bool vmm_phys_map_access_enabled(void);

#endif