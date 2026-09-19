#include "address_space.h"
#include "lib.h"
#include "interrupts.h"
#include "object_storage.h"
#include "construction_test.h"
#include "pmm_test.h"

#define USER_PML4_FIRST 1U
#define USER_PML4_END   256U

#define KERNEL_LOW_PML4_SLOT   0U
#define KERNEL_HIGH_PML4_FIRST 256U

static AddressSpace g_kernel_space;
static bool g_kernel_ready;
static u64 g_next_id;
static void *g_space_storage[ADDRESS_SPACE_STORAGE_CAPACITY];
static VmPageMap g_unpublished_map = { .root_frame = FRAME_INVALID };
static struct {
    AddressSpace *target;
    SpaceCreateTestFault fault;
    bool fail_rollback;
    bool armed;
} g_create_fault;

static u64 space_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void space_irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }

static bool space_storage_live(const AddressSpace *space) {
    return object_storage_find(g_space_storage, ADDRESS_SPACE_STORAGE_CAPACITY, space) < ADDRESS_SPACE_STORAGE_CAPACITY;
}

bool address_space_storage_in_use(const AddressSpace *space) {
    u64 flags = space_irq_save();
    bool in_use = space_storage_live(space);
    space_irq_restore(flags);
    return in_use;
}

bool address_space_creation_cleanup_pending(void) {
    u64 flags = space_irq_save();
    bool pending = g_unpublished_map.root_frame != FRAME_INVALID;
    space_irq_restore(flags);
    return pending;
}

bool address_space_reclaim_unpublished(void) {
    u64 flags = space_irq_save();
    bool result = g_unpublished_map.root_frame == FRAME_INVALID || vmm_page_map_destroy(&g_unpublished_map);
    space_irq_restore(flags);
    return result;
}

u32 address_space_object_count(void) {
    u64 flags = space_irq_save();
    u32 count = object_storage_count(g_space_storage, ADDRESS_SPACE_STORAGE_CAPACITY);
    space_irq_restore(flags);
    return count;
}

bool address_space_test_fail_create_once(AddressSpace *target, SpaceCreateTestFault fault, bool fail_rollback) {
    if (!target || (u32)fault > SPACE_CREATE_TEST_AFTER_SHARE ||
        (fault == SPACE_CREATE_TEST_ALLOCATE && fail_rollback)) return false;
    u64 flags = space_irq_save();
    bool valid = !g_create_fault.armed && !space_storage_live(target) &&
        g_unpublished_map.root_frame == FRAME_INVALID && !pmm_test_free_failure_armed();
    if (valid) {
        g_create_fault.target = target;
        g_create_fault.fault = fault;
        g_create_fault.fail_rollback = fail_rollback;
        g_create_fault.armed = true;
    }
    space_irq_restore(flags);
    return valid;
}

bool address_space_test_create_fault_armed(void) { return g_create_fault.armed; }
void address_space_test_clear_create_fault(void) {
    u64 flags = space_irq_save();
    k_memset(&g_create_fault, 0, sizeof(g_create_fault));
    space_irq_restore(flags);
}
frame_t address_space_test_unpublished_root(void) { return g_unpublished_map.root_frame; }

static bool space_create_fault(AddressSpace *target, SpaceCreateTestFault fault) {
    if (!g_create_fault.armed || g_create_fault.target != target || g_create_fault.fault != fault) return false;
    bool fail_rollback = g_create_fault.fail_rollback;
    k_memset(&g_create_fault, 0, sizeof(g_create_fault));
    if (fail_rollback) (void)pmm_test_fail_free_range_once(g_unpublished_map.root_frame, 1ULL);
    return true;
}

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
    g_space_storage[0] = &g_kernel_space;
    g_kernel_ready = true;

    return true;
}

AddressSpace *address_space_kernel(void) {
    if (!g_kernel_ready) return 0;

    return &g_kernel_space;
}

static bool address_space_create_locked(AddressSpace *space) {
    if (!space || space_storage_live(space)) return false;
    k_memset(space, 0, sizeof(*space));
    space->page_map.root_frame = FRAME_INVALID;
    if (!g_kernel_ready || !g_next_id || g_unpublished_map.root_frame != FRAME_INVALID) return false;
    u32 slot = object_storage_empty(g_space_storage, ADDRESS_SPACE_STORAGE_CAPACITY);
    if (slot == ADDRESS_SPACE_STORAGE_CAPACITY) return false;
    if (space_create_fault(space, SPACE_CREATE_TEST_ALLOCATE)) return false;

    /* The scratch map is never activated, exported, or published to a caller. */
    if (!vmm_page_map_create_owned_range(&g_unpublished_map, USER_PML4_FIRST, USER_PML4_END)) goto fail;
    if (space_create_fault(space, SPACE_CREATE_TEST_AFTER_ROOT)) goto fail;
    if (!vmm_page_map_share_pml4_entry(&g_unpublished_map, &g_kernel_space.page_map, KERNEL_LOW_PML4_SLOT)) goto fail;
    if (space_create_fault(space, SPACE_CREATE_TEST_AFTER_SHARE)) goto fail;
    for (u16 index = KERNEL_HIGH_PML4_FIRST; index < VM_PML4_ENTRY_COUNT; ++index) {
        if (!vmm_page_map_share_pml4_entry(&g_unpublished_map, &g_kernel_space.page_map, index)) goto fail;
    }

    /* Ownership move, not a copy of a live address space. No fallible work follows. */
    space->page_map = g_unpublished_map;
    space->id = g_next_id++;
    g_space_storage[slot] = space;
    k_memset(&g_unpublished_map, 0, sizeof(g_unpublished_map));
    g_unpublished_map.root_frame = FRAME_INVALID;
    return true;
fail:
    (void)address_space_reclaim_unpublished();
    return false;
}

bool address_space_create(AddressSpace *space) {
    u64 flags = space_irq_save();
    bool created = address_space_create_locked(space);
    space_irq_restore(flags);
    return created;
}

bool address_space_destroy(AddressSpace *space) {
    u64 flags = space_irq_save();
    u32 slot = object_storage_find(g_space_storage, ADDRESS_SPACE_STORAGE_CAPACITY, space);
    bool destroyed = false;
    if (slot == ADDRESS_SPACE_STORAGE_CAPACITY || space == &g_kernel_space || space->kernel ||
        space->capability_refs) goto done;
    if (!vmm_page_map_destroy(&space->page_map)) goto done;
    g_space_storage[slot] = 0;
    space->id = 0;
    destroyed = true;
done:
    space_irq_restore(flags);
    return destroyed;
}

bool address_space_map_page(AddressSpace *space, u64 virtual_address, frame_t frame, vm_flags_t flags) {
    if (!space_storage_live(space)) return false;
    if (!space->kernel) {
        if (!user_virtual_address(virtual_address)) return false;

        /* Every mapping owned by a user address space must be reachable from CPL3. */
        flags |= VM_USER;
    }

    return vmm_map_page(&space->page_map, virtual_address, frame, flags);
}

bool address_space_unmap_page(AddressSpace *space, u64 virtual_address, frame_t *old_frame) {
    if (!space_storage_live(space)) return false;
    if (!space->kernel && !user_virtual_address(virtual_address)) return false;

    return vmm_unmap_page(&space->page_map, virtual_address, old_frame);
}

bool address_space_query_page(const AddressSpace *space, u64 virtual_address, frame_t *frame, vm_flags_t *flags) {
    if (!space_storage_live(space)) return false;

    /* Queries are allowed for shared kernel mappings as well as owned user mappings. */
    return vmm_query_page(&space->page_map, virtual_address, frame, flags);
}

u64 address_space_cr3(const AddressSpace *space) {
    if (!space_storage_live(space) || space->page_map.root_frame == FRAME_INVALID || space->page_map.destroy_pending) return 0;

    return frame_to_phys(space->page_map.root_frame);
}
