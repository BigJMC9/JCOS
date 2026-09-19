#ifndef JA_OS_ADDRESS_SPACE_H
#define JA_OS_ADDRESS_SPACE_H

#include "types.h"
#include "vmm.h"

/*
 * Slot 0 remains shared while the kernel itself
 * still executes from low virtual addresses.
 */
#define ADDRESS_SPACE_USER_BASE 0x0000008000000000ULL
#define ADDRESS_SPACE_USER_LIMIT 0x0000800000000000ULL

typedef struct {
    u64 id;
    VmPageMap page_map;
    bool kernel;

    /* Owned by occupied capability slots; changed only by capability.c. */
    u64 capability_refs;
} AddressSpace;

bool address_space_kernel_init(void);
AddressSpace *address_space_kernel(void);

/* Fresh storage may be uninitialized. false has no caller-owned resources;
 * live storage is rejected unchanged. Failed rollback is module-owned. */
bool address_space_create(AddressSpace *space);
bool address_space_reclaim_unpublished(void);
bool address_space_creation_cleanup_pending(void);
u32 address_space_object_count(void);
bool address_space_storage_in_use(const AddressSpace *space);

/* false retains identity and unreleased paging structures; retry is required. */
bool address_space_destroy(AddressSpace *space);

bool address_space_map_page(
    AddressSpace *space,
    u64 virtual_address,
    frame_t frame,
    vm_flags_t flags
);

bool address_space_unmap_page(
    AddressSpace *space,
    u64 virtual_address,
    frame_t *old_frame
);

bool address_space_query_page(
    const AddressSpace *space,
    u64 virtual_address,
    frame_t *frame,
    vm_flags_t *flags
);

u64 address_space_cr3(
    const AddressSpace *space
);

#endif
