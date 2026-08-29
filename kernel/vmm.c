#include "vmm.h"
#include "lib.h"

#define PAGE_TABLE_ENTRIES 512U

/* x86-64 page-table entry bits. */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_HUGE    (1ULL << 7)

/*
 * Bits 12..51 contain the physical address.
 *
 * This supports the architectural 52-bit
 * physical-address field.
 */
#define PTE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static bool page_aligned(u64 address) {
    return
        (address & (VM_PAGE_SIZE - 1ULL)) == 0;
}

static bool virtual_address_canonical(u64 address) {
    /*
     * Current four-level x86-64 paging uses
     * 48-bit canonical virtual addresses.
     *
     * Bits 63..48 must duplicate bit 47.
     */
    u64 upper = address >> 48;
    bool negative = ((address >> 47) & 1ULL) != 0;

    if (negative) return upper == 0xFFFFULL;

    return upper == 0;
}

static u32 pml4_index(u64 address) {
    return
        (u32)((address >> 39) & 0x1FFULL);
}

static u32 pdpt_index(u64 address) {
    return
        (u32)((address >> 30) & 0x1FFULL);
}

static u32 pd_index(u64 address) {
    return
        (u32)((address >> 21) & 0x1FFULL);
}

static u32 pt_index(u64 address) {
    return
        (u32)((address >> 12) & 0x1FFULL);
}

/*
 * Stage 1 assumption:
 *
 * Physical RAM allocated through PMM is still
 * directly accessible at its physical address
 * using the mappings inherited from UEFI.
 *
 * When JCOS gets its own physical direct map,
 * we'll need to use that instead of the current
 * boot mappings.
 */
static u64 *table_pointer(frame_t frame) {
    u64 physical = frame_to_phys(frame);

    if (!physical) return 0;

    return (u64 *)(u64)physical;
}

static frame_t entry_frame(u64 entry) {
    u64 physical = entry & PTE_ADDRESS_MASK;

    return phys_to_frame(physical);
}

static bool valid_mapping_frame(frame_t frame) {
    if (frame == FRAME_INVALID) return false;

    u64 physical = frame_to_phys(frame);

    if (!physical) return false;

    /* Reject physical addresses outside the address field we currently support. */
    if (physical & ~PTE_ADDRESS_MASK) return false;

    return true;
}

static bool allocate_table(frame_t *frame_out, u64 **table_out) {
    if (!frame_out || !table_out) return false;

    frame_t frame = frame_alloc();

    if (frame == FRAME_INVALID) return false;

    u64 *table = table_pointer(frame);

    if (!table) {
        (void)frame_free(frame);
        return false;
    }

    k_memset(table, 0, (usize)VM_PAGE_SIZE);

    *frame_out = frame;
    *table_out = table;

    return true;
}

/* Resolve an existing child page table. */
static bool existing_table(u64 *parent, u32 index, frame_t *frame_out, u64 **table_out) {
    if (!parent || !frame_out || !table_out) return false;

    u64 entry = parent[index];

    if (!(entry & PTE_PRESENT)) return false;

    /*
     * Stage 1 only supports 4 KiB mappings.
     *
     * Encountering a 1 GiB or 2 MiB huge-page
     * entry means this isn't one of the ordinary
     * child page tables.
     */
    if (entry & PTE_HUGE) return false;

    frame_t frame = entry_frame(entry);

    if (frame == FRAME_INVALID) return false;

    u64 *table = table_pointer(frame);

    if (!table) return false;

    *frame_out = frame;
    *table_out = table;

    return true;
}

/*
 * Resolve or allocate a child page table.
 *
 * created_frame is FRAME_INVALID when table
 * already existed.
 */
static bool create_or_get_table(u64 *parent, u32 index, bool user, frame_t *created_frame, u64 **table_out) {
    if (!parent || !created_frame || !table_out) return false;

    *created_frame = FRAME_INVALID;

    u64 entry = parent[index];

    if (entry & PTE_PRESENT) {
        if (entry & PTE_HUGE) return false;

        /*
         * Intermediate entries must permit writes
         * for writable leaf mappings.
         *
         * Making an intermediate USER accessible
         * is safe becase every leaf still has its
         * own USER permission bit.
         */
        parent[index] |= PTE_WRITE;
        if (user) parent[index] |= PTE_USER;

        frame_t frame = entry_frame(parent[index]);

        if (frame == FRAME_INVALID) return false;

        u64 *table = table_pointer(frame);

        if (!table) return false;

        *table_out = table;

        return true;
    }

    frame_t frame;
    u64 *table;

    if (!allocate_table(&frame, &table)) return false;

    u64 physical = frame_to_phys(frame);

    if (!physical || (physical & ~PTE_ADDRESS_MASK)) {

        (void)frame_free(frame);
        return false;
    }

    u64 flags = PTE_PRESENT | PTE_WRITE;
    if (user) flags |= PTE_USER;

    parent[index] = physical | flags;
    *created_frame = frame;
    *table_out = table;

    return true;
}

static void rollback_created_tables(u64 **entries, frame_t *frames, u32 count) {
    while (count) {
        --count;

        if (entries[count]) *entries[count] = 0;
        if (frames[count] != FRAME_INVALID) (void)frame_free(frames[count]);
    }
}

static bool table_empty(const u64 *table) {
    if (!table) return true;
    for (u32 i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        if (table[i] & PTE_PRESENT) return false;
    }

    return true;
}

bool vmm_page_map_create(VmPageMap *map) {
    if (!map) return false;

    map->root_frame = FRAME_INVALID;

    frame_t root;
    u64 *table;

    if (!allocate_table(&root, &table)) return false;

    /* allocate_table() already zeroed it. */
    map->root_frame = root;

    return true;
}

void vmm_page_map_destroy(VmPageMap *map) {
    if (!map || map->root_frame == FRAME_INVALID) return;

    u64 *pml4 = table_pointer(map->root_frame);

    if (!pml4) {
        map->root_frame = FRAME_INVALID;

        return;
    }

    /*
     * Walk all owned paging-structure frames.
     *
     * Leaf data frames are deliberately NOT
     * released here.
     */
    for (u32 i = 0; i < PAGE_TABLE_ENTRIES; ++i) {

        u64 pml4e = pml4[i];

        if (!(pml4e & PTE_PRESENT)) continue;

        frame_t pdpt_frame = entry_frame(pml4e);
        u64 *pdpt = table_pointer(pdpt_frame);

        if (!pdpt) continue;
        for (u32 j = 0; j < PAGE_TABLE_ENTRIES; ++j) {

            u64 pdpte = pdpt[j];

            if (!(pdpte & PTE_PRESENT)) continue;

            /* A 1 GiB huge page is a leaf mapping, not another owned table. */
            if (pdpte & PTE_HUGE) continue;

            frame_t pd_frame = entry_frame(pdpte);
            u64 *pd = table_pointer(pd_frame);

            if (!pd) continue;
            for (u32 k = 0; k < PAGE_TABLE_ENTRIES; ++k) {

                u64 pde = pd[k];

                if (!(pde & PTE_PRESENT)) continue;

                /* A 2 MiB huge page is also a leaf mapping. */
                if (pde & PTE_HUGE) continue;

                frame_t pt_frame = entry_frame(pde);

                /* PT entries themselves point to DATA frames, so we free only the PT frame here. */
                (void)frame_free(pt_frame);
            }

            (void)frame_free(pd_frame);
        }

        (void)frame_free(pdpt_frame);
    }

    (void)frame_free(map->root_frame);

    map->root_frame = FRAME_INVALID;
}

bool vmm_map_page(VmPageMap *map, u64 virtual_address, frame_t frame, vm_flags_t flags) {
    if (!map || map->root_frame == FRAME_INVALID) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;
    if (!valid_mapping_frame(frame)) return false;

    /* Refuse flags we do not understand yet. */
    if (flags & ~(VM_WRITE | VM_USER)) return false;

    u64 *pml4 = table_pointer(map->root_frame);

    if (!pml4) return false;

    bool user = (flags & VM_USER) != 0;

    /* Keep track of page-table frames created during this call so an allocation failure doesn't leak them. */
    u64 *created_entries[3] = {
        0, 0, 0
    };

    frame_t created_frames[3] = {
        FRAME_INVALID,
        FRAME_INVALID,
        FRAME_INVALID
    };

    u32 created_count = 0;

    u64 *pdpt = 0;
    u64 *pd = 0;
    u64 *pt = 0;

    frame_t created = FRAME_INVALID;

    u32 i4 = pml4_index(virtual_address);

    if (!create_or_get_table(pml4, i4, user, &created, &pdpt)) return false;
    if (created != FRAME_INVALID) {

        created_entries[
            created_count
        ] = &pml4[i4];

        created_frames[
            created_count
        ] = created;

        ++created_count;
    }

    u32 i3 = pdpt_index(virtual_address);

    created = FRAME_INVALID;

    if (!create_or_get_table(pdpt, i3, user, &created, &pd)) {

        rollback_created_tables(created_entries, created_frames, created_count);

        return false;
    }

    if (created != FRAME_INVALID) {

        created_entries[
            created_count
        ] = &pdpt[i3];

        created_frames[
            created_count
        ] = created;

        ++created_count;
    }

    u32 i2 = pd_index(virtual_address);

    created = FRAME_INVALID;

    if (!create_or_get_table(pd, i2, user, &created, &pt)) {

        rollback_created_tables(created_entries, created_frames, created_count);

        return false;
    }

    if (created != FRAME_INVALID) {

        created_entries[
            created_count
        ] = &pd[i2];

        created_frames[
            created_count
        ] = created;

        ++created_count;
    }

    u32 i1 = pt_index(virtual_address);

    /* Don't silently replace an existing mapping. */
    if (pt[i1] & PTE_PRESENT) {

        rollback_created_tables(created_entries, created_frames, created_count);

        return false;
    }

    u64 physical = frame_to_phys(frame);
    u64 entry_flags = PTE_PRESENT;

    if (flags & VM_WRITE) entry_flags |= PTE_WRITE;
    if (flags & VM_USER) entry_flags |= PTE_USER;

    pt[i1] = physical | entry_flags;

    return true;
}

bool vmm_query_page(const VmPageMap *map, u64 virtual_address, frame_t *frame, vm_flags_t *flags) {
    if (!map || map->root_frame == FRAME_INVALID) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;

    u64 *pml4 = table_pointer(map->root_frame);

    if (!pml4) return false;

    u32 i4 = pml4_index(virtual_address);
    u64 pml4e = pml4[i4];

    frame_t pdpt_frame;
    u64 *pdpt;

    if (!existing_table(pml4, i4, &pdpt_frame, &pdpt)) return false;

    (void)pdpt_frame;

    u32 i3 = pdpt_index(virtual_address);
    u64 pdpte = pdpt[i3];

    frame_t pd_frame;
    u64 *pd;

    if (!existing_table(pdpt, i3, &pd_frame, &pd)) return false;

    (void)pd_frame;

    u32 i2 = pd_index(virtual_address);
    u64 pde = pd[i2];

    frame_t pt_frame;
    u64 *pt;

    if (!existing_table(pd, i2, &pt_frame, &pt)) return false;

    (void)pt_frame;

    u32 i1 = pt_index(virtual_address);
    u64 pte = pt[i1];

    if (!(pte & PTE_PRESENT)) return false;
    if (frame) *frame = entry_frame(pte);
    if (flags) {
        vm_flags_t result = 0;

        /* Permissions are effective only when every level permits them. */
        if ((pml4e & PTE_WRITE) && (pdpte & PTE_WRITE) && (pde & PTE_WRITE) && (pte & PTE_WRITE)) result |= VM_WRITE;
        if ((pml4e & PTE_USER) && (pdpte & PTE_USER) && (pde & PTE_USER) && (pte & PTE_USER)) result |= VM_USER;

        *flags = result;
    }

    return true;
}

bool vmm_unmap_page(VmPageMap *map, u64 virtual_address, frame_t *old_frame) {
    if (old_frame) *old_frame = FRAME_INVALID;
    if (!map || map->root_frame == FRAME_INVALID) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;

    u64 *pml4 = table_pointer(map->root_frame);

    if (!pml4) return false;

    u32 i4 = pml4_index(virtual_address);

    frame_t pdpt_frame;
    u64 *pdpt;

    if (!existing_table(pml4, i4, &pdpt_frame, &pdpt)) return false;

    u32 i3 = pdpt_index(virtual_address);

    frame_t pd_frame;
    u64 *pd;

    if (!existing_table(pdpt, i3, &pd_frame, &pd)) return false;

    u32 i2 = pd_index(virtual_address);

    frame_t pt_frame;
    u64 *pt;

    if (!existing_table(pd, i2, &pt_frame, &pt)) return false;

    u32 i1 = pt_index(virtual_address);

    u64 entry = pt[i1];

    if (!(entry & PTE_PRESENT)) return false;

    frame_t mapped = entry_frame(entry);

    /* Remove the mapping. */
    pt[i1] = 0;

    if (old_frame) *old_frame = mapped;

    /*
     * Prune now-empty page-table levels.
     *
     * These are paging-structure frames owned
     * by VmPageMap, so freeing them is correct.
     *
     * Still DO NOT free 'mapped'.
     */
    if (table_empty(pt)) {
        pd[i2] = 0;
        (void)frame_free(pt_frame);

        if (table_empty(pd)) {
            pdpt[i3] = 0;
            (void)frame_free(pd_frame);

            if (table_empty(pdpt)) {
                pml4[i4] = 0;
                (void)frame_free(pdpt_frame);
            }
        }
    }

    return true;
}

bool vmm_identity_map_range(VmPageMap *map, u64 physical_address, u64 size, vm_flags_t flags) {
    if (!map) return false;
    if (!size) return true;

    /* Inclusive final byte. */
    if (physical_address > ~0ULL - (size - 1ULL)) return false;

    u64 last_byte = physical_address + size - 1ULL;
    u64 first_page = physical_address & ~(VM_PAGE_SIZE - 1ULL);
    u64 last_page = last_byte & ~(VM_PAGE_SIZE - 1ULL);
    u64 page = first_page;

    for (;;) {

        /*
         * Deliberately leave virtual address zero
         * unmapped. This will eventually help
         * catch NULL dereferences.
         *
         * Nothing JCOS currently requires lives
         * in physical page zero.
         */
        if (page != 0) {

            frame_t expected = phys_to_frame(page);

            if (expected == FRAME_INVALID) return false;

            /*
             * The function is idempotent.
             *
             * This matters because explicit
             * framebuffer/MMIO mappings may
             * overlap UEFI descriptors.
             */
            frame_t existing = FRAME_INVALID;

            vm_flags_t existing_flags = 0;

            if (vmm_query_page(map, page, &existing, &existing_flags)) {
                if (existing != expected) return false;

                /* Existing mapping must provide at least the requested public permissions. */
                if ((existing_flags & flags) != flags) return false;
            } else {

                if (!vmm_map_page(map, page, expected, flags)) return false;
            }
        }

        if (page == last_page) break;
        if (page > ~0ULL - VM_PAGE_SIZE) return false;

        page += VM_PAGE_SIZE;
    }

    return true;
}