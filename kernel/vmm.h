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
#define VM_EXEC  (1ULL << 2)
#define VM_UNCACHED (1ULL << 3)

typedef struct {
    frame_t root_frame;

    /*
     * PML4 entries in [first, end) are owned by
     * this map. Shared entries are never freed.
     */
    u16 owned_pml4_first;
    u16 owned_pml4_end;
    bool destroy_pending;
    /* Exported kernel subtrees are pinned for boot lifetime in this profile. */
    bool shared_source;
} VmPageMap;

bool vmm_page_map_create(VmPageMap *map);

/* Retryable, empty-owned-tree destruction. Never frees leaf DATA frames.
 * false retains root and all unreleased tables. Do not reuse live map storage.
 * Only inactive, non-exported maps on the UP/no-PCID profile are reclaimable. */
bool vmm_page_map_destroy(VmPageMap *map);
/* Retry optional empty-table pruning; live DATA mappings remain untouched. */
bool vmm_page_map_collect(VmPageMap *map);
/* One bounded VMM-owned quarantine slot for a never-linked allocation. */
bool vmm_reclaim_unlinked_table(void);
bool vmm_unlinked_table_cleanup_pending(void);

bool vmm_map_page(
    VmPageMap *map, 
    u64 virtual_address, 
    frame_t frame, 
    vm_flags_t flags
);

/* true means leaf removal committed, even if optional table pruning was
 * deferred. The returned DATA frame belongs to the caller. A collector or
 * retryable destroy later reclaims empty tables retained in the tree. */
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
    VmPageMap *source,
    u16 index
);

bool vmm_enable_nx(void);
bool vmm_nx_enabled(void);
bool vmm_enable_write_protect(void);
bool vmm_write_protect_enabled(void);

void vmm_enable_phys_map_access(void);
bool vmm_phys_map_access_enabled(void);

#endif
