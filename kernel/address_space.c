#include "address_space.h"

#define USER_PML4_FIRST 1U
#define USER_PML4_END   256U

#define KERNEL_LOW_PML4_SLOT   0U
#define KERNEL_HIGH_PML4_FIRST 256U

static AddressSpace g_kernel_space;
static bool g_kernel_ready;
static u64 g_next_id;

static bool user_virtual_address(u64 address) {
    return
        address >= ADDRESS_SPACE_USER_BASE &&
        address < ADDRESS_SPACE_USER_LIMIT;
}

bool address_space_kernel_init(void) {
    if (g_kernel_ready) return false;

    g_kernel_space.id = 0;
    g_kernel_space.kernel = true;

    if (!vmm_page_map_create(&g_kernel_space.page_map)) return false;

    g_next_id = 1;
    g_kernel_ready = true;

    return true;
}

AddressSpace *address_space_kernel(void) {
    if (!g_kernel_ready) return 0;

    return &g_kernel_space;
}

bool address_space_create(AddressSpace *space) {
    if (!space || !g_kernel_ready || !g_next_id) return false;

    space->id = 0;
    space->kernel = false;

    /* User address spaces own PML4 slots 1..255. Slot 0 and the upper half remain shared. */
    if (!vmm_page_map_create_owned_range(&space->page_map, USER_PML4_FIRST, USER_PML4_END)) return false;

    /* Transitional low kernel mapping. */
    if (!vmm_page_map_share_pml4_entry(&space->page_map, &g_kernel_space.page_map, KERNEL_LOW_PML4_SLOT)) goto fail;

    /* Share the higher-half kernel mappings, including the physical direct map. */
    for (u16 index = KERNEL_HIGH_PML4_FIRST; index < VM_PML4_ENTRY_COUNT; ++index) {
        if (!vmm_page_map_share_pml4_entry(&space->page_map, &g_kernel_space.page_map, index)) goto fail;
    }

    space->id = g_next_id++;

    return true;

fail:
    vmm_page_map_destroy(&space->page_map);

    return false;
}

void address_space_destroy(AddressSpace *space) {
    if (!space || space->kernel) return;

    vmm_page_map_destroy(&space->page_map);

    space->id = 0;
}

bool address_space_map_page(AddressSpace *space, u64 virtual_address, frame_t frame, vm_flags_t flags) {
    if (!space) return false;
    if (!space->kernel) {
        if (!user_virtual_address(virtual_address)) return false;

        /* Every mapping owned by a user address space must be reachable from CPL3. */
        flags |= VM_USER;
    }

    return vmm_map_page(&space->page_map, virtual_address, frame, flags);
}

bool address_space_unmap_page(AddressSpace *space, u64 virtual_address, frame_t *old_frame) {
    if (!space) return false;
    if (!space->kernel && !user_virtual_address(virtual_address)) return false;

    return vmm_unmap_page(&space->page_map, virtual_address, old_frame);
}

bool address_space_query_page(const AddressSpace *space, u64 virtual_address, frame_t *frame, vm_flags_t *flags) {
    if (!space) return false;

    /* Queries are allowed for shared kernel mappings as well as owned user mappings. */
    return vmm_query_page(&space->page_map, virtual_address, frame, flags);
}

u64 address_space_cr3(const AddressSpace *space) {
    if (!space || space->page_map.root_frame == FRAME_INVALID) return 0;

    return frame_to_phys(space->page_map.root_frame);
}