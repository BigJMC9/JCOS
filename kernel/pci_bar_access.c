#include "pci_bar_access.h"

#include "address_space.h"
#include "arch.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "pmm.h"
#include "vmm.h"

#define PCI_BAR_ACCESS_RFLAGS_IF (1ULL << 9)

static void *g_mmio_mappings[PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY];

static u64 access_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void access_irq_restore(u64 flags) {
    if (flags & PCI_BAR_ACCESS_RFLAGS_IF) interrupts_enable();
}

static bool mapping_live(const PciBarMmioMapping *mapping) {
    return object_storage_find(g_mmio_mappings, PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY,
        mapping) < PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY;
}

static bool mmio_type(PciBarType type) {
    return type == PCI_BAR_MMIO32 || type == PCI_BAR_MMIO64;
}

static bool mmio_geometry(const PciBarResource *resource, u64 offset, u64 size,
    u64 *out_physical, u32 *out_pages) {
    if (out_physical) *out_physical = 0ULL;
    if (out_pages) *out_pages = 0U;
    if (!resource || !size || (offset & (VM_PAGE_SIZE - 1ULL)) ||
        (size & (VM_PAGE_SIZE - 1ULL))) return false;

    PciBar bar;
    k_memset(&bar, 0, sizeof(bar));
    if (!pci_bar_resource_descriptor(resource, &bar) ||
        !pci_bar_resource_bounded(resource) || !mmio_type(bar.type) ||
        !pci_bar_resource_contains(resource, offset, size) ||
        bar.base > ~0ULL - offset) return false;

    u64 physical = bar.base + offset;
    if (!physical || (physical & (VM_PAGE_SIZE - 1ULL))) return false;
    u64 pages = size / VM_PAGE_SIZE;
    if (!pages || pages > 0xFFFFFFFFULL ||
        physical > ~0ULL - (size - 1ULL)) return false;

    if (out_physical) *out_physical = physical;
    if (out_pages) *out_pages = (u32)pages;
    return true;
}

bool pci_bar_mmio_range_mappable(const PciBarResource *resource,
    u64 bar_offset, u64 size) {
    u64 flags = access_irq_save();
    bool result = mmio_geometry(resource, bar_offset, size, 0, 0);
    access_irq_restore(flags);
    return result;
}

static bool mapping_valid_locked(const PciBarMmioMapping *mapping) {
    if (!mapping_live(mapping) || !mapping->initialized || !mapping->resource ||
        !mapping->process || !mapping->resource_id || !mapping->process_id ||
        !mapping->size || !mapping->page_count ||
        mapping->mapped_page_count > mapping->page_count ||
        mapping->size != (u64)mapping->page_count * VM_PAGE_SIZE ||
        (mapping->virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
        (mapping->physical_base & (VM_PAGE_SIZE - 1ULL)) ||
        mapping->virtual_base < ADDRESS_SPACE_USER_BASE ||
        mapping->virtual_base >= ADDRESS_SPACE_USER_LIMIT) return false;

    if (!process_storage_in_use(mapping->process) || !mapping->process->initialized ||
        mapping->process->kernel || mapping->process->id != mapping->process_id ||
        !pci_bar_resource_valid(mapping->resource) ||
        mapping->resource->resource.id != mapping->resource_id ||
        pci_bar_resource_owner_process_id(mapping->resource) != mapping->process_id)
        return false;

    return mapping->mapped_page_count != 0U;
}

bool pci_bar_mmio_mapping_valid(const PciBarMmioMapping *mapping) {
    u64 flags = access_irq_save();
    bool valid = mapping && mapping_valid_locked(mapping);
    access_irq_restore(flags);
    return valid;
}

u32 pci_bar_mmio_mapping_object_count(void) {
    u64 flags = access_irq_save();
    u32 count = object_storage_count(g_mmio_mappings,
        PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY);
    access_irq_restore(flags);
    return count;
}

static bool virtual_geometry(u64 virtual_base, u64 size) {
    if (!size || (virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
        virtual_base < ADDRESS_SPACE_USER_BASE || virtual_base >= ADDRESS_SPACE_USER_LIMIT)
        return false;
    return size <= ADDRESS_SPACE_USER_LIMIT - virtual_base;
}

static bool rollback_pages(AddressSpace *space, u64 virtual_base, frame_t first,
    u32 mapped) {
    while (mapped) {
        --mapped;
        u64 virtual_address = virtual_base + (u64)mapped * VM_PAGE_SIZE;
        frame_t old = FRAME_INVALID;
        if (!address_space_unmap_page(space, virtual_address, &old) || old != first + mapped)
            return false;
    }
    return true;
}

bool pci_bar_mmio_map(PciBarMmioMapping *mapping, PciBarResource *resource,
    Process *process, CapabilityHandle handle, u64 bar_offset, u64 size,
    u64 virtual_base, bool writable) {
    if (!mapping || !resource || !process || !handle || !virtual_geometry(virtual_base, size))
        return false;

    u64 flags = access_irq_save();
    bool created = false;
    if (mapping_live(mapping)) goto done;

    u64 physical = 0ULL;
    u32 pages = 0U;
    if (!mmio_geometry(resource, bar_offset, size, &physical, &pages)) goto done;

    u32 slot = object_storage_empty(g_mmio_mappings,
        PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY);
    if (slot >= PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY) goto done;

    CapabilityRights rights = CAPABILITY_RIGHT_READ;
    if (writable) rights |= CAPABILITY_RIGHT_WRITE;
    if (!pci_bar_resource_access_acquire(resource, process, handle, rights)) goto done;

    AddressSpace *space = process_address_space(process);
    frame_t first = phys_to_frame(physical);
    if (!space || first == FRAME_INVALID) {
        if (!pci_bar_resource_access_release(resource, process->id)) cpu_halt_forever();
        goto done;
    }

    vm_flags_t map_flags = VM_UNCACHED | (writable ? VM_WRITE : 0ULL);
    u32 mapped = 0U;
    for (; mapped < pages; ++mapped) {
        if (!address_space_map_page(space,
                virtual_base + (u64)mapped * VM_PAGE_SIZE,
                first + mapped, map_flags)) break;
    }

    if (mapped != pages) {
        if (!rollback_pages(space, virtual_base, first, mapped) ||
            !pci_bar_resource_access_release(resource, process->id)) cpu_halt_forever();
        goto done;
    }

    k_memset(mapping, 0, sizeof(*mapping));
    mapping->resource = resource;
    mapping->process = process;
    mapping->resource_id = resource->resource.id;
    mapping->process_id = process->id;
    mapping->virtual_base = virtual_base;
    mapping->physical_base = physical;
    mapping->size = size;
    mapping->page_count = pages;
    mapping->mapped_page_count = pages;
    mapping->writable = writable;
    mapping->initialized = true;
    g_mmio_mappings[slot] = mapping;
    created = true;

done:
    access_irq_restore(flags);
    return created;
}

bool pci_bar_mmio_unmap(PciBarMmioMapping *mapping) {
    if (!mapping) return false;
    u64 flags = access_irq_save();
    u32 slot = object_storage_find(g_mmio_mappings,
        PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY, mapping);
    if (slot >= PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY || !mapping_valid_locked(mapping)) {
        access_irq_restore(flags);
        return false;
    }

    AddressSpace *space = process_address_space(mapping->process);
    if (!space) {
        access_irq_restore(flags);
        return false;
    }

    while (mapping->mapped_page_count) {
        u32 index = mapping->mapped_page_count - 1U;
        u64 virtual_address = mapping->virtual_base + (u64)index * VM_PAGE_SIZE;
        frame_t expected = phys_to_frame(mapping->physical_base) + index;
        frame_t mapped = FRAME_INVALID;
        vm_flags_t page_flags = 0;
        vm_flags_t expected_flags = VM_USER | VM_UNCACHED |
            (mapping->writable ? VM_WRITE : 0ULL);
        if (!address_space_query_page(space, virtual_address, &mapped, &page_flags) ||
            mapped != expected || page_flags != expected_flags) {
            access_irq_restore(flags);
            return false;
        }
        frame_t old = FRAME_INVALID;
        if (!address_space_unmap_page(space, virtual_address, &old) || old != expected) {
            access_irq_restore(flags);
            return false;
        }
        mapping->mapped_page_count = index;
    }

    PciBarResource *resource = mapping->resource;
    u64 owner = mapping->process_id;
    g_mmio_mappings[slot] = 0;
    k_memset(mapping, 0, sizeof(*mapping));
    if (!pci_bar_resource_access_release(resource, owner)) cpu_halt_forever();
    access_irq_restore(flags);
    return true;
}

bool pci_bar_mmio_force_unmap_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = access_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY; ++i) {
        PciBarMmioMapping *mapping = (PciBarMmioMapping *)g_mmio_mappings[i];
        if (!mapping) continue;
        if (!mapping_valid_locked(mapping)) { ok = false; break; }
        if (mapping->resource->assignment != assignment) continue;
        if (mapping->process_id != owner_process_id || !pci_bar_mmio_unmap(mapping)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    access_irq_restore(flags);
    return ok;
}

static bool pio_geometry(const PciBarResource *resource, u64 offset, u32 width,
    u16 *out_port) {
    if (out_port) *out_port = 0U;
    if (!resource || (width != 1U && width != 2U && width != 4U) ||
        (offset & (u64)(width - 1U))) return false;

    PciBar bar;
    k_memset(&bar, 0, sizeof(bar));
    if (!pci_bar_resource_descriptor(resource, &bar) ||
        !pci_bar_resource_bounded(resource) || bar.type != PCI_BAR_IO ||
        !pci_bar_resource_contains(resource, offset, width) ||
        bar.base > 0xFFFFULL || offset > 0xFFFFULL - bar.base) return false;

    u64 port = bar.base + offset;
    if (port > 0xFFFFULL || (u64)width - 1ULL > 0xFFFFULL - port) return false;
    if (out_port) *out_port = (u16)port;
    return true;
}

bool pci_bar_pio_range_valid(const PciBarResource *resource, u64 offset, u32 width) {
    u64 flags = access_irq_save();
    bool valid = pio_geometry(resource, offset, width, 0);
    access_irq_restore(flags);
    return valid;
}

static u32 io_read(u16 port, u32 width) {
    if (width == 1U) {
        u8 value;
        __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    if (width == 2U) {
        u16 value;
        __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void io_write(u16 port, u32 width, u32 value) {
    if (width == 1U) {
        __asm__ volatile ("outb %0, %1" : : "a"((u8)value), "Nd"(port) : "memory");
        return;
    }
    if (width == 2U) {
        __asm__ volatile ("outw %0, %1" : : "a"((u16)value), "Nd"(port) : "memory");
        return;
    }
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

bool pci_bar_pio_read(PciBarResource *resource, Process *process,
    CapabilityHandle handle, u64 offset, u32 width, u32 *out_value) {
    if (!out_value) return false;
    *out_value = 0U;
    u64 flags = access_irq_save();
    u16 port = 0U;
    bool valid = pio_geometry(resource, offset, width, &port) &&
        pci_bar_resource_access_acquire(resource, process, handle, CAPABILITY_RIGHT_READ);
    if (valid) {
        *out_value = io_read(port, width);
        if (!pci_bar_resource_access_release(resource, process->id)) cpu_halt_forever();
    }
    access_irq_restore(flags);
    return valid;
}

bool pci_bar_pio_write(PciBarResource *resource, Process *process,
    CapabilityHandle handle, u64 offset, u32 width, u32 value) {
    u64 flags = access_irq_save();
    u16 port = 0U;
    bool valid = pio_geometry(resource, offset, width, &port) &&
        pci_bar_resource_access_acquire(resource, process, handle, CAPABILITY_RIGHT_WRITE);
    if (valid) {
        io_write(port, width, value);
        if (!pci_bar_resource_access_release(resource, process->id)) cpu_halt_forever();
    }
    access_irq_restore(flags);
    return valid;
}
