#include "polaris_mmio.h"
#include "address_space.h"
#include "amd_polaris_registers.h"
#include "arch.h"
#include "boot_display.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "physmap.h"
#include "device_mmio.h"

static void *g_bindings[POLARIS_MMIO_BINDING_CAPACITY];
static void *g_holds[POLARIS_MMIO_HOLD_CAPACITY];
static u64 g_next_binding_identity = 1ULL;
static u64 g_next_hold_identity = 1ULL;
static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }
static bool same_bytes(const void *a, const void *b, u32 size) {
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < size; ++i) if (x[i] != y[i]) return false;
    return true;
}
bool polaris_mmio_plan(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u16 pmcsr, PolarisMmioPlan *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    BootDisplayBar bar;
    if (!device || !info || device->vendor_id != 0x1002U || device->device_id != 0x67DFU ||
        device->revision != 0xEFU || device->subclass || device->prog_if ||
        (device->header_type & 0x7FU) ||
        info->subsystem_vendor_id != 0x1458U || info->subsystem_device_id != 0x2310U ||
        !info->polaris10_candidate || !(info->command & PCI_COMMAND_MEMORY_SPACE) ||
        info->pm_offset < 0x40U || (info->pm_offset & 3U) || info->pm_offset > 0xF8U ||
        !(info->status & (1U << 4)) || pmcsr == 0xFFFFU || (pmcsr & 3U) ||
        info->bars[5].index != 5U || info->bars[5].type != PCI_BAR_MMIO32 || info->bars[5].prefetchable ||
        !display_probe_firmware_bar(boot, device, info, 5U, &bar) ||
        bar.size != 0x40000ULL || (bar.base & (VM_PAGE_SIZE - 1ULL))) return false;
    bool pm_found = false;
    if (info->capability_count > DISPLAY_PROBE_CAPACITY) return false;
    for (u32 i = 0; i < info->capability_count; ++i)
        if (info->capability_offsets[i] == info->pm_offset && info->capability_ids[i] == 1U)
            pm_found = true;
    if (!pm_found) return false;
    const BootDisplayBarQuery *query = &boot->framebuffer_bar_queries[boot->framebuffer_bar_query_index[5]];
    const BootDisplayBarAudit *audit = &boot->framebuffer_bar_audit[5];
    BootDisplayBar decoded;
    BootDisplayBarAudit decoded_audit;
    /* A truncated prefix or multi-descriptor audit is sufficient for inventory,
     * never for this first authority-bearing binding. Reparse the entire list. */
    if (query->prefix_size != 48U || query->audit.query_status || audit->query_status ||
        !boot_display_decode_bar_audited(query->prefix, query->prefix_size,
            (u32)bar.base, 0U, 5U, &decoded, &decoded_audit) ||
        !same_bytes(&decoded, &bar, sizeof(bar)) ||
        !same_bytes(&decoded_audit, &query->audit, sizeof(decoded_audit)) ||
        !same_bytes(&decoded_audit, audit, sizeof(decoded_audit))) return false;
    PolarisMmioPlan plan;
    k_memset(&plan, 0, sizeof(plan));
    plan.bar = bar;
    plan.command = info->command;
    plan.pmcsr = pmcsr;
    plan.pm_offset = info->pm_offset;
    for (u32 pipe = 0; pipe < POLARIS_DISPLAY_PIPE_COUNT; ++pipe) {
        for (u32 field = 0; field < POLARIS_DISPLAY_REGISTER_COUNT; ++field) {
            u32 offset = 0;
            if (!jcos_polaris_register_offset(pipe, (PolarisDisplayRegister)field, bar.size, &offset))
                return false;
            u32 page = offset & ~(u32)(VM_PAGE_SIZE - 1ULL);
            u32 i = 0;
            while (i < plan.page_count && plan.page_offsets[i] != page) ++i;
            if (i < plan.page_count) continue;
            if (plan.page_count == POLARIS_MMIO_PAGE_COUNT) return false;
            plan.page_offsets[plan.page_count++] = page;
        }
    }
    if (plan.page_count != POLARIS_MMIO_PAGE_COUNT) return false;
    k_memcpy(out, &plan, sizeof(plan));
    return true;
}
static bool registered(const PolarisMmioBinding *b) {
    return b && object_storage_find(g_bindings, POLARIS_MMIO_BINDING_CAPACITY, b) <
        POLARIS_MMIO_BINDING_CAPACITY;
}
static bool retained_identity(const PolarisMmioBinding *b) {
    if (!registered(b) || !b->initialized || !pci_assignment_valid(b->assignment)) return false;
    DeviceResource *resource = pci_assignment_resource(b->assignment);
    return resource && resource->id == b->assignment_id &&
        pci_assignment_owner_process_id(b->assignment) == b->owner_id;
}
static bool owner_live(Process *owner) {
    ProcessExitInfo exit;
    return owner && process_storage_in_use(owner) && owner->initialized && !owner->kernel &&
        owner->id && !process_exit_info(owner, &exit);
}
static bool kernel_context(VmPageMap *map, frame_t root) {
    AddressSpace *kernel = address_space_kernel();
    return kernel && map == &kernel->page_map && map->root_frame == root &&
        !map->destroy_pending && !(arch_read_cr4() & (1ULL << 17)) &&
        (arch_read_cr3() & 0x000FFFFFFFFFF000ULL) == frame_to_phys(root);
}
bool polaris_mmio_binding_valid(const PolarisMmioBinding *b) {
    u64 flags = irq_save();
    bool valid = registered(b);
    irq_restore(flags);
    return valid;
}
bool polaris_mmio_binding_prepared(const PolarisMmioBinding *b) {
    u64 flags = irq_save();
    bool ready = retained_identity(b) && b->prepared && !b->closing && !b->poisoned &&
        b->mapped_pages == b->plan.page_count && b->child_pinned && b->access_pinned &&
        pci_assignment_state(b->assignment) == PCI_ASSIGNMENT_ASSIGNED &&
        owner_live(b->owner) && b->owner->id == b->owner_id;
    irq_restore(flags);
    return ready;
}
u32 polaris_mmio_binding_count(void) {
    u64 flags = irq_save();
    u32 count = object_storage_count(g_bindings, POLARIS_MMIO_BINDING_CAPACITY);
    irq_restore(flags);
    return count;
}
u64 polaris_mmio_binding_identity(const PolarisMmioBinding *b) {
    u64 flags = irq_save();
    u64 id = registered(b) && b->initialized ? b->identity : 0ULL;
    irq_restore(flags);
    return id;
}
u32 polaris_mmio_hold_count(void) {
    u64 flags = irq_save();
    u32 count = object_storage_count(g_holds, POLARIS_MMIO_HOLD_CAPACITY);
    irq_restore(flags);
    return count;
}
bool polaris_mmio_close(PolarisMmioBinding *b) {
    u64 flags = irq_save();
    bool closed = registered(b);
    if (closed) { b->closing = true; b->prepared = false; }
    irq_restore(flags);
    return closed;
}
bool polaris_mmio_hold_acquire(PolarisMmioHold *hold, PolarisMmioBinding *b,
    u64 expected_binding_identity, Process *owner, u64 *out_identity) {
    if (!out_identity) return false;
    *out_identity = 0ULL;
    if (!hold || !expected_binding_identity) return false;
    u64 flags = irq_save();
    bool acquired = false;
    u32 slot = object_storage_empty(g_holds, POLARIS_MMIO_HOLD_CAPACITY);
    if (slot >= POLARIS_MMIO_HOLD_CAPACITY || !g_next_hold_identity ||
        object_storage_find(g_holds, POLARIS_MMIO_HOLD_CAPACITY, hold) < POLARIS_MMIO_HOLD_CAPACITY ||
        !polaris_mmio_binding_prepared(b) || b->identity != expected_binding_identity ||
        b->owner != owner || !owner_live(owner) || b->owner_id != owner->id ||
        pci_assignment_dma_gate_active(b->assignment) ||
        b->active_holds >= POLARIS_MMIO_HOLD_CAPACITY) goto done;
    k_memset(hold, 0, sizeof(*hold));
    hold->binding = b;
    hold->binding_identity = b->identity;
    hold->identity = g_next_hold_identity++;
    hold->initialized = true;
    g_holds[slot] = hold;
    ++b->active_holds;
    *out_identity = hold->identity;
    acquired = true;
done:
    irq_restore(flags);
    return acquired;
}
bool polaris_mmio_hold_release(PolarisMmioHold *hold, u64 expected_hold_identity) {
    if (!hold || !expected_hold_identity) return false;
    u64 flags = irq_save();
    bool released = false;
    u32 slot = object_storage_find(g_holds, POLARIS_MMIO_HOLD_CAPACITY, hold);
    /* Registry membership precedes token dereference. Cleanup does not depend
     * on a live owner, ASSIGNED state, usable capability or prepared admission. */
    if (slot >= POLARIS_MMIO_HOLD_CAPACITY || !hold->initialized ||
        hold->identity != expected_hold_identity) goto done;
    PolarisMmioBinding *b = hold->binding;
    if (!retained_identity(b) || b->identity != hold->binding_identity || !b->active_holds) goto done;
    --b->active_holds;
    g_holds[slot] = 0;
    k_memset(hold, 0, sizeof(*hold));
    released = true;
done:
    irq_restore(flags);
    return released;
}
static PolarisMmioBinding *held_binding(PolarisMmioHold *hold, u64 expected_hold_identity,
    Process *owner) {
    if (!device_mmio_interrupts_enabled() || !expected_hold_identity ||
        object_storage_find(g_holds, POLARIS_MMIO_HOLD_CAPACITY, hold) >= POLARIS_MMIO_HOLD_CAPACITY ||
        !hold->initialized || hold->identity != expected_hold_identity) return 0;
    PolarisMmioBinding *b = hold->binding;
    if (!polaris_mmio_binding_prepared(b) || b->identity != hold->binding_identity ||
        b->owner != owner || !owner_live(owner) || owner->id != b->owner_id ||
        pci_assignment_dma_gate_active(b->assignment) || !b->active_holds ||
        !kernel_context(b->map, b->root_frame)) return 0;
    return b;
}
static bool read_leaf(PolarisMmioBinding *b, u32 page, u32 slot, u32 offset, u32 *out) {
    u64 address = b->virtual_base + (u64)slot * VM_PAGE_SIZE;
    frame_t frame;
    vm_flags_t flags;
    if (!vmm_query_page(b->map, address, &frame, &flags) ||
        frame != phys_to_frame(b->plan.bar.base + page) || flags != VM_DEVICE ||
        !device_mmio_interrupts_enabled()) return false;
    /* No IRQ-off section spans this load. The retained hold prevents teardown.
     * A stalled physical transaction has no software-enforced bus timeout. */
    *out = device_mmio_load32(address + (offset & (VM_PAGE_SIZE - 1ULL)));
    return true;
}
bool polaris_mmio_read_held(PolarisMmioHold *hold, u64 expected_hold_identity,
    Process *owner, u32 pipe, PolarisDisplayRegister field, u32 *out) {
    if (!out) return false;
    *out = 0;
    PolarisMmioBinding *b = held_binding(hold, expected_hold_identity, owner);
    if (!b) return false;
    u32 offset;
    if (!jcos_polaris_register_offset(pipe, field, b->plan.bar.size, &offset)) return false;
    u32 page = offset & ~(u32)(VM_PAGE_SIZE - 1ULL), slot = 0;
    while (slot < b->plan.page_count && b->plan.page_offsets[slot] != page) ++slot;
    if (slot == b->plan.page_count) return false;
    return read_leaf(b, page, slot, offset, out);
}
bool polaris_mmio_read_witness_held(PolarisMmioHold *hold,u64 expected_hold_identity,
    Process *owner,u32 pipe,PolarisPlaneWitnessRegister field,u32 *out) {
    if (!out) return false;
    *out=0;
    PolarisMmioBinding *b=held_binding(hold,expected_hold_identity,owner);
    u32 offset;
    if (!b || !jcos_polaris_witness_offset(pipe,field,b->plan.bar.size,&offset)) return false;
    u32 page=offset&~0xFFFU,slot=0;
    while (slot<b->plan.page_count && b->plan.page_offsets[slot]!=page) ++slot;
    return slot<b->plan.page_count && read_leaf(b,page,slot,offset,out);
}
bool polaris_mmio_read_location_held(PolarisMmioHold *hold, u64 expected_hold_identity,
    Process *owner, PolarisLocationRegister field, u32 *out) {
    if (!out) return false;
    *out = 0;
    if ((u32)field >= POLARIS_LOCATION_COUNT) return false;
    PolarisMmioBinding *b = held_binding(hold, expected_hold_identity, owner);
    if (!b || !b->location_mapped) return false;
    u32 offset = field == POLARIS_LOCATION_MC_FB ?
        POLARIS_MC_VM_FB_LOCATION_BYTE : POLARIS_HDP_NONSURFACE_BASE_BYTE;
    if (offset + 4ULL > b->plan.bar.size) return false;
    return read_leaf(b, offset & ~0xFFFU, b->plan.page_count, offset, out);
}
static bool cleanup_locked(PolarisMmioBinding *b) {
    if (!polaris_mmio_close(b) || !retained_identity(b) || b->poisoned || b->active_holds)
        return false;
    if ((b->mapped_pages || b->location_mapped) && !kernel_context(b->map, b->root_frame)) return false;
    if (b->location_mapped) {
        u64 address = b->virtual_base + (u64)b->plan.page_count * VM_PAGE_SIZE;
        frame_t expected = phys_to_frame(b->plan.bar.base + 0x2000ULL);
        frame_t actual = FRAME_INVALID, old = FRAME_INVALID;
        vm_flags_t permissions = 0;
        if (!vmm_query_page(b->map, address, &actual, &permissions) ||
            actual != expected || permissions != VM_DEVICE ||
            !vmm_unmap_page(b->map, address, &old)) return false;
        b->location_mapped = false;
        if (old != expected) { b->poisoned = true; return false; }
    }
    while (b->mapped_pages) {
        u32 i = b->mapped_pages - 1U;
        u64 address = b->virtual_base + (u64)i * VM_PAGE_SIZE;
        frame_t expected = phys_to_frame(b->plan.bar.base + b->plan.page_offsets[i]);
        frame_t actual = FRAME_INVALID, old = FRAME_INVALID;
        vm_flags_t permissions = 0;
        if (!vmm_query_page(b->map, address, &actual, &permissions) ||
            actual != expected || permissions != VM_DEVICE ||
            !vmm_unmap_page(b->map, address, &old)) return false;
        --b->mapped_pages; /* Removal committed; retain the remaining exact prefix. */
        if (old != expected) { b->poisoned = true; return false; }
    }
    if (b->access_pinned) {
        if (!pci_assignment_access_release(b->assignment, b->owner_id)) return false;
        b->access_pinned = false;
    }
    if (b->child_pinned) {
        if (!pci_assignment_child_release(b->assignment, b->owner_id)) return false;
        b->child_pinned = false;
    }
    u32 slot = object_storage_find(g_bindings, POLARIS_MMIO_BINDING_CAPACITY, b);
    g_bindings[slot] = 0;
    k_memset(b, 0, sizeof(*b));
    return true;
}
bool polaris_mmio_cleanup(PolarisMmioBinding *b) {
    u64 flags = irq_save();
    bool cleaned = cleanup_locked(b);
    irq_restore(flags);
    return cleaned;
}
static bool current_plan(const BootInfo *boot, PciAssignment *assignment,
    u64 owner_id, PolarisMmioPlan *plan) {
    const PciDevice *device = pci_assignment_enumerated_device(assignment);
    DisplayProbeInfo info;
    if (!device) return false;
    /* Record command first: a malformed/disappearing config must not erase an
     * observation made before the full inventory decoder rejected it. */
    u16 command = pci_config_read16(device->address, 4U);
    if ((command & PCI_COMMAND_BUS_MASTER) &&
        !pci_assignment_note_inherited_bus_master(assignment, owner_id)) return false;
    if (command == 0xFFFFU || !display_probe_read(device, &info)) return false;
    if ((info.command & PCI_COMMAND_BUS_MASTER) &&
        !pci_assignment_note_inherited_bus_master(assignment, owner_id)) return false;
    if (!info.pm_offset || info.pm_offset > 0xF8U) return false;
    u16 pmc = pci_config_read16(device->address, (u16)(info.pm_offset + 2U));
    if (!(pmc & 7U) || (pmc & 7U) > 3U || pmc == 0xFFFFU) return false;
    u16 pmcsr = pci_config_read16(device->address, (u16)(info.pm_offset + 4U));
    return polaris_mmio_plan(boot, device, &info, pmcsr, plan);
}
static bool page_not_ram(const BootInfo *boot, u64 physical) {
    if (!boot->memory_map || !boot->memory_map_size ||
        boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor) ||
        boot->memory_map_size % boot->memory_map_descriptor_size ||
        boot->memory_map_size / boot->memory_map_descriptor_size > 4096ULL ||
        boot->memory_map > ~0ULL - boot->memory_map_size) return false;
    for (u64 offset = 0; offset < boot->memory_map_size; offset += boot->memory_map_descriptor_size) {
        const BootMemoryDescriptor *d = (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);
        if (!d->number_of_pages) continue;
        if (d->number_of_pages > (~0ULL - d->physical_start) / VM_PAGE_SIZE) return false;
        u64 end = d->physical_start + d->number_of_pages * VM_PAGE_SIZE;
        if (physical < end && d->physical_start < physical + VM_PAGE_SIZE && d->type != 11U)
            return false;
    }
    /* PCI MMIO may be absent from the UEFI map. The complete BAR descriptor,
     * not absence here, supplies its extent; conflicting entries are refused. */
    return true;
}
bool polaris_mmio_prepare(PolarisMmioBinding *b, const BootInfo *boot,
    PciAssignment *assignment, Process *owner, CapabilityHandle assignment_handle) {
    if (!b || !boot || !assignment || !owner) return false;
    u64 flags = irq_save();
    bool result = false;
    if (registered(b) || !owner_live(owner) ||
        !pci_assignment_authorized(assignment, owner, assignment_handle) ||
        pci_assignment_dma_gate_active(assignment)) goto done;
    for (u32 i = 0; i < POLARIS_MMIO_BINDING_CAPACITY; ++i) {
        const PolarisMmioBinding *other = g_bindings[i];
        if (other && other->assignment == assignment) goto done;
    }
    u32 slot = object_storage_empty(g_bindings, POLARIS_MMIO_BINDING_CAPACITY);
    AddressSpace *kernel = address_space_kernel();
    if (slot == POLARIS_MMIO_BINDING_CAPACITY || !g_next_binding_identity || !kernel ||
        !kernel_context(&kernel->page_map, kernel->page_map.root_frame) ||
        !vmm_device_memory_supported()) goto done;
    PolarisMmioPlan plan;
    if (!current_plan(boot, assignment, owner->id, &plan)) goto done;
    u64 window = POLARIS_MMIO_WINDOW_BASE + (u64)slot * POLARIS_MMIO_WINDOW_STRIDE;
    for (u32 i = 0; i < plan.page_count; ++i) {
        u64 physical = plan.bar.base + plan.page_offsets[i], direct = 0;
        if (!page_not_ram(boot, physical) || !physmap_virtual_address(physical, &direct) ||
            vmm_query_page(&kernel->page_map, physical, 0, 0) ||
            vmm_query_page(&kernel->page_map, direct, 0, 0) ||
            vmm_query_page(&kernel->page_map, window + (u64)i * VM_PAGE_SIZE, 0, 0)) goto done;
    }
    k_memset(b, 0, sizeof(*b));
    b->assignment = assignment;
    b->identity = g_next_binding_identity++;
    b->assignment_id = pci_assignment_resource(assignment)->id;
    b->owner = owner;
    b->owner_id = owner->id;
    b->map = &kernel->page_map;
    b->root_frame = b->map->root_frame;
    b->virtual_base = window;
    k_memcpy(&b->plan, &plan, sizeof(plan));
    b->initialized = true;
    g_bindings[slot] = b; /* Publish the cleanup ledger before acquiring pins. */
    if (!pci_assignment_child_acquire(assignment, owner, assignment_handle)) goto rollback;
    b->child_pinned = true;
    if (!pci_assignment_access_acquire(assignment, owner->id)) goto rollback;
    b->access_pinned = true;
    for (u32 i = 0; i < plan.page_count; ++i) {
        if (!vmm_map_page(b->map, window + (u64)i * VM_PAGE_SIZE,
                phys_to_frame(plan.bar.base + plan.page_offsets[i]), VM_DEVICE)) goto rollback;
        ++b->mapped_pages;
    }
    PolarisMmioPlan after;
    if (!current_plan(boot, assignment, owner->id, &after) ||
        !same_bytes(&after, &plan, sizeof(plan))) goto rollback;
    b->prepared = true;
    result = true;
    goto done;
rollback:
    (void)cleanup_locked(b); /* Failure retains the ledger and unreleased pins. */
done:
    irq_restore(flags);
    return result;
}


bool polaris_mmio_prepare_location(PolarisMmioBinding *b, const BootInfo *boot,
    Process *owner, u64 expected_binding_identity) {
    if (!boot || !expected_binding_identity) return false;
    u64 flags = irq_save();
    bool result = false;
    if (!polaris_mmio_binding_prepared(b) || b->identity != expected_binding_identity ||
        b->owner != owner || b->active_holds || b->location_mapped ||
        pci_assignment_dma_gate_active(b->assignment) ||
        !kernel_context(b->map, b->root_frame) || !vmm_device_memory_supported()) goto done;
    PolarisMmioPlan before, after;
    if (!current_plan(boot, b->assignment, b->owner_id, &before) ||
        !same_bytes(&before, &b->plan, sizeof(before))) goto done;
    u64 physical = b->plan.bar.base + 0x2000ULL, direct = 0;
    u64 address = b->virtual_base + (u64)b->plan.page_count * VM_PAGE_SIZE;
    if (POLARIS_MC_VM_FB_LOCATION_BYTE / VM_PAGE_SIZE != 2U ||
        POLARIS_HDP_NONSURFACE_BASE_BYTE / VM_PAGE_SIZE != 2U ||
        !page_not_ram(boot, physical) || !physmap_virtual_address(physical, &direct) ||
        vmm_query_page(b->map, physical, 0, 0) || vmm_query_page(b->map, direct, 0, 0) ||
        vmm_query_page(b->map, address, 0, 0) ||
        !vmm_map_page(b->map, address, phys_to_frame(physical), VM_DEVICE)) goto done;
    b->location_mapped = true; /* Publish before post-map revalidation. */
    if (!current_plan(boot, b->assignment, b->owner_id, &after) ||
        !same_bytes(&after, &b->plan, sizeof(after))) {
        (void)polaris_mmio_close(b);
        goto done; /* The existing ledger owns the fifth leaf and both pins. */
    }
    result = true;
done:
    irq_restore(flags);
    return result;
}
