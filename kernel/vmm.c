#include "vmm.h"
#include "lib.h"
#include "physmap.h"
#include "arch.h"
#include "interrupts.h"
#include "vmm_test.h"

#define PAGE_TABLE_ENTRIES 512U

/* x86-64 page-table entry bits. */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PCD     (1ULL << 4)
#define PTE_HUGE    (1ULL << 7)
#define PTE_NX      (1ULL << 63)

#define CR0_WRITE_PROTECT        (1ULL << 16)
#define IA32_EFER_MSR            0xC0000080U
#define IA32_EFER_NXE            (1ULL << 11)
#define CPUID_EXTENDED_MAX       0x80000000U
#define CPUID_EXTENDED_FEATURES  0x80000001U
#define CPUID_EXTENDED_NX        (1U << 20)

/*
 * Bits 12..51 contain the physical address.
 *
 * This supports the architectural 52-bit
 * physical-address field.
 */
#define PTE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

/*
 * Zero-initialized deliberately.
 *
 * Before JCOS has switched to its own page map,
 * paging structures are accessed through the
 * inherited low identity mappings.
 *
 * After the physical direct map has been proven
 * active, this becomes true.
 */
static bool g_use_phys_map;
static bool g_nx_enabled;

/* UP normal context only. No NMI callers, PCID, or remote page-map users. */
static u64 vmm_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void vmm_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}

static bool map_active(const VmPageMap *map) {
    return map && map->root_frame != FRAME_INVALID &&
        (arch_read_cr3() & PTE_ADDRESS_MASK) == frame_to_phys(map->root_frame);
}

static bool profile_supported(void) {
    return !(arch_read_cr4() & (1ULL << 17)); /* CR4.PCIDE */
}

/* No global leaf mappings are created by this VMM. */
static void flush_active_map(const VmPageMap *map) {
    if (map_active(map)) arch_write_cr3(arch_read_cr3());
}

/* A not-yet-linked allocation can survive loss of its constructor's storage. */
static frame_t g_unlinked_table = FRAME_INVALID;

/* Exact-map, one-shot diagnostics. Never exposed by a userspace syscall. */
static struct {
    VmPageMap *map;
    u32 skip;
    bool armed;
} g_faults[VMM_TEST_FAULT_COUNT];

static bool test_fault(VmPageMap *map, VmmTestFault fault) {
    if (!g_faults[fault].armed || g_faults[fault].map != map) return false;
    if (g_faults[fault].skip) { --g_faults[fault].skip; return false; }
    g_faults[fault].armed = false;
    g_faults[fault].map = 0;
    return true;
}

bool vmm_test_fail_once(VmPageMap *map, VmmTestFault fault, u32 skip) {
    if (!map || (u32)fault >= VMM_TEST_FAULT_COUNT) return false;
    u64 flags = vmm_irq_save();
    bool valid = !g_faults[fault].armed;
    if (valid) {
        g_faults[fault].map = map;
        g_faults[fault].skip = skip;
        g_faults[fault].armed = true;
    }
    vmm_irq_restore(flags);
    return valid;
}

u32 vmm_test_faults_armed(void) {
    u64 flags = vmm_irq_save();
    u32 count = 0;
    for (u32 i = 0; i < VMM_TEST_FAULT_COUNT; ++i) if (g_faults[i].armed) ++count;
    vmm_irq_restore(flags);
    return count;
}

void vmm_test_clear_faults(void) {
    u64 flags = vmm_irq_save();
    k_memset(g_faults, 0, sizeof(g_faults));
    vmm_irq_restore(flags);
}

frame_t vmm_test_unlinked_table(void) { return g_unlinked_table; }

bool vmm_unlinked_table_cleanup_pending(void) {
    u64 flags = vmm_irq_save();
    bool pending = g_unlinked_table != FRAME_INVALID;
    vmm_irq_restore(flags);
    return pending;
}

bool vmm_reclaim_unlinked_table(void) {
    u64 flags = vmm_irq_save();
    bool result = g_unlinked_table == FRAME_INVALID || frame_free(g_unlinked_table);
    if (result) g_unlinked_table = FRAME_INVALID;
    vmm_irq_restore(flags);
    return result;
}

static bool free_table(VmPageMap *map, frame_t frame) {
    return !test_fault(map, VMM_TEST_FREE) && frame_free(frame);
}

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

static bool map_owns_pml4_index(const VmPageMap *map, u32 index) {
    if (!map) return false;

    return
        index >= map->owned_pml4_first &&
        index < map->owned_pml4_end;
}

/* Before the CR3 switch, page tables use bootstrap identity mappings. Afterwards they use the physmap. */
static u64 *table_pointer(frame_t frame) {
    u64 physical = frame_to_phys(frame);

    if (!physical) return 0;
    if (g_use_phys_map) return (u64 *)phys_to_virt(physical);

    /* Bootstrap access before the physmap is active. */
    return (u64 *)(u64)physical;
}

void vmm_enable_phys_map_access(void) {
    g_use_phys_map =true;
}

bool vmm_phys_map_access_enabled(void) {
    return g_use_phys_map;
}

bool vmm_enable_nx(void) {
    if (g_nx_enabled) return true;

    u32 a = 0, b = 0, c = 0, d = 0;
    arch_cpuid(CPUID_EXTENDED_MAX, 0, &a, &b, &c, &d);
    (void)b;
    (void)c;
    (void)d;
    if (a < CPUID_EXTENDED_FEATURES) return false;

    arch_cpuid(CPUID_EXTENDED_FEATURES, 0, &a, &b, &c, &d);
    (void)a;
    (void)b;
    (void)c;
    if (!(d & CPUID_EXTENDED_NX)) return false;

    u64 efer = arch_read_msr(IA32_EFER_MSR);
    if (!(efer & IA32_EFER_NXE)) {
        arch_write_msr(IA32_EFER_MSR, efer | IA32_EFER_NXE);
        efer = arch_read_msr(IA32_EFER_MSR);
    }

    g_nx_enabled = (efer & IA32_EFER_NXE) != 0;
    return g_nx_enabled;
}

bool vmm_nx_enabled(void) {
    return g_nx_enabled;
}

bool vmm_enable_write_protect(void) {
    u64 cr0 = arch_read_cr0();
    if (!(cr0 & CR0_WRITE_PROTECT)) {
        arch_write_cr0(cr0 | CR0_WRITE_PROTECT);
        cr0 = arch_read_cr0();
    }
    return (cr0 & CR0_WRITE_PROTECT) != 0;
}

bool vmm_write_protect_enabled(void) {
    return (arch_read_cr0() & CR0_WRITE_PROTECT) != 0;
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

static bool allocate_table(VmPageMap *map, frame_t *frame_out, u64 **table_out) {
    if (!map || !frame_out || !table_out) return false;
    if (g_unlinked_table != FRAME_INVALID || test_fault(map, VMM_TEST_ALLOC)) return false;
    frame_t frame = frame_alloc();
    if (frame == FRAME_INVALID) return false;

    u64 *table = valid_mapping_frame(frame) && !test_fault(map, VMM_TEST_ACCESS)
        ? table_pointer(frame) : 0;
    if (!table) {
        if (!free_table(map, frame)) g_unlinked_table = frame;
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

    /* Only 4 KiB mappings are supported. */
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
static bool create_or_get_table(VmPageMap *map, u64 *parent, u32 index, bool user, frame_t *created_frame, u64 **table_out) {
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

    if (entry) return false;
    frame_t frame;
    u64 *table;

    if (!allocate_table(map, &frame, &table)) return false;

    u64 physical = frame_to_phys(frame);

    u64 flags = PTE_PRESENT | PTE_WRITE;
    if (user) flags |= PTE_USER;

    parent[index] = physical | flags;
    *created_frame = frame;
    *table_out = table;

    return true;
}

static bool table_empty(const u64 *table) {
    if (!table) return false;
    for (u32 i = 0; i < PAGE_TABLE_ENTRIES; ++i) if (table[i]) return false;
    return true;
}

/* Inactive, non-exported trees only. The parent link is the ownership ledger. */
static bool release_empty_child(VmPageMap *map, u64 *entry) {
    if (!entry || !(*entry & PTE_PRESENT) || (*entry & PTE_HUGE)) return false;
    frame_t frame = entry_frame(*entry);
    u64 *table = table_pointer(frame);
    if (!table_empty(table)) return false;
    u64 saved = *entry;
    *entry = 0;
    if (!free_table(map, frame)) {
        *entry = saved;
        return false;
    }
    return true;
}

static void rollback_created_tables(VmPageMap *map, u64 **entries, frame_t *frames, u32 count) {
    if (map_active(map) || map->shared_source) return;
    while (count) {
        --count;
        if (!entries[count] || entry_frame(*entries[count]) != frames[count] ||
            !release_empty_child(map, entries[count])) return;
    }
}

/* level 1 is a PT: present entries there are caller-owned DATA mappings. */
static bool tree_has_no_leaves(frame_t frame, u32 level, u32 first, u32 end) {
    u64 *table = table_pointer(frame);
    if (!table || !level) return false;
    for (u32 i = first; i < end; ++i) {
        u64 entry = table[i];
        if (!entry) continue;
        if (level == 1 || !(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return false;
        frame_t child = entry_frame(entry);
        if (child == frame || !tree_has_no_leaves(child, level - 1U, 0, PAGE_TABLE_ENTRIES)) return false;
    }
    return true;
}

static bool collect_children(VmPageMap *map, frame_t frame, u32 level, u32 first, u32 end) {
    u64 *table = table_pointer(frame);
    if (!table || !level) return false;
    if (level == 1) return true; /* Never free or remove DATA entries here. */
    for (u32 i = first; i < end; ++i) {
        u64 entry = table[i];
        if (!entry) continue;
        if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return false;
        frame_t child = entry_frame(entry);
        if (child == frame || !collect_children(map, child, level - 1U, 0, PAGE_TABLE_ENTRIES)) return false;
        if (table_empty(table_pointer(child)) && !release_empty_child(map, &table[i])) return false;
    }
    return true;
}

static bool map_reclaimable(const VmPageMap *map) {
    return map && map->root_frame != FRAME_INVALID &&
        map->owned_pml4_first < map->owned_pml4_end && map->owned_pml4_end <= VM_PML4_ENTRY_COUNT &&
        !map->shared_source && !map_active(map) && profile_supported();
}

bool vmm_page_map_collect(VmPageMap *map) {
    u64 flags = vmm_irq_save();
    bool result = map_reclaimable(map) &&
        collect_children(map, map->root_frame, 4U, map->owned_pml4_first, map->owned_pml4_end);
    vmm_irq_restore(flags);
    return result;
}

static bool create_owned_locked(VmPageMap *map, u16 owned_first, u16 owned_end) {
    if (!map) return false;
    if (owned_first >= owned_end || owned_end > VM_PML4_ENTRY_COUNT) return false;

    map->root_frame = FRAME_INVALID;
    map->owned_pml4_first = 0;
    map->owned_pml4_end = 0;
    map->destroy_pending = false;
    map->shared_source = false;

    frame_t root;
    u64 *table;

    if (!allocate_table(map, &root, &table)) return false;

    map->root_frame = root;
    map->owned_pml4_first = owned_first;
    map->owned_pml4_end = owned_end;

    return true;
}

bool vmm_page_map_create_owned_range(VmPageMap *map, u16 first, u16 end) {
    u64 flags = vmm_irq_save();
    bool result = profile_supported() && create_owned_locked(map, first, end);
    vmm_irq_restore(flags);
    return result;
}

bool vmm_page_map_create(VmPageMap *map) {
    return vmm_page_map_create_owned_range(map, 0, VM_PML4_ENTRY_COUNT);
}

bool vmm_page_map_destroy(VmPageMap *map) {
    u64 flags = vmm_irq_save();
    bool result = false;
    if (!map_reclaimable(map)) goto done;
    if (!tree_has_no_leaves(map->root_frame, 4U, map->owned_pml4_first, map->owned_pml4_end)) goto done;

    map->destroy_pending = true;
    if (!collect_children(map, map->root_frame, 4U, map->owned_pml4_first, map->owned_pml4_end)) goto done;
    if (!free_table(map, map->root_frame)) goto done;
    map->root_frame = FRAME_INVALID;
    map->owned_pml4_first = 0;
    map->owned_pml4_end = 0;
    map->destroy_pending = false;
    result = true;
done:
    vmm_irq_restore(flags);
    return result;
}

static bool map_page_locked(VmPageMap *map, u64 virtual_address, frame_t frame, vm_flags_t flags) {
    if (!map || map->root_frame == FRAME_INVALID || map->destroy_pending) return false;
    if (map->shared_source && !map_active(map)) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;
    if (!valid_mapping_frame(frame)) return false;
    if (!g_nx_enabled) return false;
    /* Refuse flags we do not understand yet. */
    if (flags & ~(VM_WRITE | VM_USER | VM_EXEC | VM_UNCACHED)) return false;

    u32 i4 = pml4_index(virtual_address);
    if (!map_owns_pml4_index(map,i4)) return false;

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

    if (!create_or_get_table(map, pml4, i4, user, &created, &pdpt)) return false;
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

    if (!create_or_get_table(map, pdpt, i3, user, &created, &pd)) {

        rollback_created_tables(map, created_entries, created_frames, created_count);

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

    if (!create_or_get_table(map, pd, i2, user, &created, &pt)) {

        rollback_created_tables(map, created_entries, created_frames, created_count);

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
    if (pt[i1]) {

        rollback_created_tables(map, created_entries, created_frames, created_count);

        return false;
    }

    u64 physical = frame_to_phys(frame);
    u64 entry_flags = PTE_PRESENT | PTE_NX;

    if (flags & VM_WRITE) entry_flags |= PTE_WRITE;
    if (flags & VM_USER) entry_flags |= PTE_USER;
    if (flags & VM_UNCACHED) entry_flags |= PTE_PCD;
    if (flags & VM_EXEC) entry_flags &= ~PTE_NX;

    pt[i1] = physical | entry_flags;

    return true;
}

bool vmm_map_page(VmPageMap *map, u64 address, frame_t frame, vm_flags_t permissions) {
    u64 flags = vmm_irq_save();
    bool result = profile_supported() && map_page_locked(map, address, frame, permissions);
    /* Also flush when a failed attempt changed intermediate permission bits. */
    if (profile_supported()) flush_active_map(map);
    vmm_irq_restore(flags);
    return result;
}

bool vmm_query_page(const VmPageMap *map, u64 virtual_address, frame_t *frame, vm_flags_t *flags) {
    if (!map || map->root_frame == FRAME_INVALID) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;

    u64 *pml4 = table_pointer(map->root_frame);
    if (!pml4) return false;

    u32 i4 = pml4_index(virtual_address);
    frame_t pdpt_frame;
    u64 *pdpt;

    if (!existing_table(pml4, i4, &pdpt_frame, &pdpt)) return false;

    u64 pml4e = pml4[i4];

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
        if (!((pml4e | pdpte | pde | pte) & PTE_NX)) result |= VM_EXEC;
        if (pte & PTE_PCD) result |= VM_UNCACHED;

        *flags = result;
    }

    return true;
}

static vm_flags_t vmm_test_entry_flags(u64 entry) {
    if (!(entry & PTE_PRESENT)) return 0;

    vm_flags_t flags = 0;

    if (entry & PTE_WRITE) flags |= VM_WRITE;
    if (entry & PTE_USER) flags |= VM_USER;
    if (!(entry & PTE_NX)) flags |= VM_EXEC;
    if (entry & PTE_PCD) flags |= VM_UNCACHED;

    return flags;
}

bool vmm_test_page_walk(const VmPageMap *map, u64 virtual_address, VmmTestPageWalk *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));

    frame_t mapped = FRAME_INVALID;
    vm_flags_t effective = 0;
    if (!vmm_query_page(map, virtual_address, &mapped, &effective)) return false;

    u64 *pml4 = table_pointer(map->root_frame);
    if (!pml4) return false;
    u32 i4 = pml4_index(virtual_address);
    u64 pml4e = pml4[i4];

    frame_t ignored = FRAME_INVALID;
    u64 *pdpt = 0;
    if (!existing_table(pml4, i4, &ignored, &pdpt)) return false;
    u32 i3 = pdpt_index(virtual_address);
    u64 pdpte = pdpt[i3];

    u64 *pd = 0;
    if (!existing_table(pdpt, i3, &ignored, &pd)) return false;
    u32 i2 = pd_index(virtual_address);
    u64 pde = pd[i2];

    u64 *pt = 0;
    if (!existing_table(pd, i2, &ignored, &pt)) return false;
    u64 pte = pt[pt_index(virtual_address)];
    if (!(pte & PTE_PRESENT) || entry_frame(pte) != mapped) return false;

    out->frame = mapped;
    out->pml4_flags = vmm_test_entry_flags(pml4e);
    out->pdpt_flags = vmm_test_entry_flags(pdpte);
    out->pd_flags = vmm_test_entry_flags(pde);
    out->pt_flags = vmm_test_entry_flags(pte);
    out->effective_flags = effective;
    return true;
}

static bool unmap_page_locked(VmPageMap *map, u64 virtual_address, frame_t *old_frame) {
    if (old_frame) *old_frame = FRAME_INVALID;
    if (!map || map->root_frame == FRAME_INVALID || map->destroy_pending) return false;
    if (map->shared_source && !map_active(map)) return false;
    if (!page_aligned(virtual_address) || !virtual_address_canonical(virtual_address)) return false;

    u32 i4 = pml4_index(virtual_address);
    if (!map_owns_pml4_index(map,i4)) return false;

    u64 *pml4 = table_pointer(map->root_frame);
    if (!pml4) return false;

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

    if (!(entry & PTE_PRESENT) || (entry & (1ULL << 8))) return false;

    frame_t mapped = entry_frame(entry);
    /* Remove the mapping. */
    pt[i1] = 0;
    if (old_frame) *old_frame = mapped;

    flush_active_map(map);

    /* Leaf removal has committed. Failed optional pruning must not report an
     * unmap failure to its caller. Retain empty tables via their parent links. */
    if (!map_active(map) && !map->shared_source && table_empty(pt)) {
        if (!release_empty_child(map, &pd[i2])) return true;
        if (table_empty(pd)) {
            if (!release_empty_child(map, &pdpt[i3])) return true;
            if (table_empty(pdpt)) (void)release_empty_child(map, &pml4[i4]);
        }
    }

    return true;
}

bool vmm_unmap_page(VmPageMap *map, u64 address, frame_t *old_frame) {
    if (old_frame) *old_frame = FRAME_INVALID;
    u64 flags = vmm_irq_save();
    bool result = profile_supported() && unmap_page_locked(map, address, old_frame);
    vmm_irq_restore(flags);
    return result;
}

bool vmm_map_range(VmPageMap *map, u64 virtual_address, u64 physical_address, u64 size, vm_flags_t flags) {
    if (!map || map->destroy_pending) return false;
    if (!size) return true;

    /* Range mappings must start on page boundaries. */
    if ((virtual_address & (VM_PAGE_SIZE - 1ULL)) != 0) return false;
    if ((physical_address & (VM_PAGE_SIZE - 1ULL)) != 0) return false;

    /* Avoid overflow while calculating how many pages cover the requested byte count. */
    if (size > ~0ULL - (VM_PAGE_SIZE - 1ULL)) return false;

    u64 page_count = (size + VM_PAGE_SIZE - 1ULL) / VM_PAGE_SIZE;

    if (!page_count) return false;
    if (page_count > ~0ULL / VM_PAGE_SIZE) return false;

    u64 span = page_count * VM_PAGE_SIZE;

    if (virtual_address > ~0ULL - (span - 1ULL)) return false;
    if (physical_address > ~0ULL - (span - 1ULL)) return false;
    for (u64 i = 0; i < page_count; ++i) {
        u64 virtual_page = virtual_address + i * VM_PAGE_SIZE;
        u64 physical_page = physical_address + i * VM_PAGE_SIZE;
        if (!map_owns_pml4_index(map, pml4_index(virtual_page))) return false;

        frame_t expected = phys_to_frame(physical_page);

        if (expected == FRAME_INVALID) return false;

        /* Allow the same range to be requested more than once, provided it refers to the same physical frame and has sufficient rights. */
        frame_t existing = FRAME_INVALID;

        vm_flags_t existing_flags = 0;

        if (vmm_query_page(map, virtual_page, &existing, &existing_flags)) {
            if (existing != expected) return false;
            if ((existing_flags & flags) != flags) return false;

            continue;
        }

        if (!vmm_map_page(map, virtual_page, expected, flags)) return false;
    }

    return true;
}

bool vmm_identity_map_range(VmPageMap *map, u64 physical_address, u64 size, vm_flags_t flags) {
    if (!size) return true;

    /* Keep physical page zero unmapped. */
    u64 first = physical_address;
    u64 offset = first & (VM_PAGE_SIZE - 1ULL);
    u64 aligned = first & ~(VM_PAGE_SIZE - 1ULL);

    if (size > ~0ULL - offset) return false;
    u64 adjusted_size = size + offset;

    /* Do not map physical page zero. */
    if (!aligned) {
        if (adjusted_size <= VM_PAGE_SIZE) return true;

        aligned = VM_PAGE_SIZE;

        adjusted_size -= VM_PAGE_SIZE;
    }

    return
        vmm_map_range(map, aligned, aligned, adjusted_size, flags);
}

static bool share_locked(VmPageMap *destination, VmPageMap *source, u16 index) {
    if (!destination || !source || destination->root_frame == FRAME_INVALID || source->root_frame == FRAME_INVALID || index >= VM_PML4_ENTRY_COUNT) return false;

    if (destination == source || destination->destroy_pending || source->destroy_pending || map_active(destination)) return false;
    /* Never install a shared subtree into an entry that this map would later try to destroy. */
    if (map_owns_pml4_index(destination, index)) return false;

    u64 *destination_pml4 = table_pointer(destination->root_frame);

    u64 *source_pml4 = table_pointer(source->root_frame);

    if (!destination_pml4 || !source_pml4) return false;
    if (destination_pml4[index] & PTE_PRESENT) return false;

    /* Copy the PML4 entry, not the subtree. Both address spaces now reference the same lower-level kernel paging structures. */
    destination_pml4[index] = source_pml4[index];
    if (source_pml4[index] & PTE_PRESENT) source->shared_source = true;

    return true;
}

bool vmm_page_map_share_pml4_entry(VmPageMap *destination, VmPageMap *source, u16 index) {
    u64 flags = vmm_irq_save();
    bool result = profile_supported() && share_locked(destination, source, index);
    vmm_irq_restore(flags);
    return result;
}
