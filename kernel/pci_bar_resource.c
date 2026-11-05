#include "pci_bar_resource.h"

#include "arch.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"

#define PCI_BAR_RESOURCE_STORAGE_CAPACITY 64U
#define PCI_BAR_RESOURCE_RFLAGS_IF (1ULL << 9)
#define PCI_COMMAND_OFFSET 0x04U
#define PCI_COMMAND_ACTIVE_MASK \
    (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)

static void *g_bar_resources[PCI_BAR_RESOURCE_STORAGE_CAPACITY];

static u64 bar_resource_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void bar_resource_irq_restore(u64 flags) {
    if (flags & PCI_BAR_RESOURCE_RFLAGS_IF) interrupts_enable();
}

static bool bar_resource_live(const PciBarResource *resource) {
    return object_storage_find(g_bar_resources, PCI_BAR_RESOURCE_STORAGE_CAPACITY,
        resource) < PCI_BAR_RESOURCE_STORAGE_CAPACITY;
}

static DeviceResourceKind kind_for_bar(PciBarType type) {
    if (type == PCI_BAR_IO) return DEVICE_RESOURCE_PCI_IO_BAR;
    if (type == PCI_BAR_MMIO32 || type == PCI_BAR_MMIO64)
        return DEVICE_RESOURCE_PCI_MMIO_BAR;
    return DEVICE_RESOURCE_NONE;
}

static bool assignment_matches(const PciBarResource *resource) {
    if (!resource || !resource->assignment || !pci_assignment_valid(resource->assignment))
        return false;
    DeviceResource *parent = pci_assignment_resource(resource->assignment);
    return parent && parent->id == resource->assignment_resource_id &&
        pci_assignment_owner_process_id(resource->assignment) == resource->owner_process_id;
}

static bool bar_resource_valid_locked(const PciBarResource *resource) {
    if (!bar_resource_live(resource) || !resource->initialized ||
        !resource->owner_process_id || !assignment_matches(resource) ||
        !device_resource_valid(&resource->resource)) return false;

    DeviceResourceKind expected = kind_for_bar(resource->bar.type);
    if (expected == DEVICE_RESOURCE_NONE || resource->resource.kind != expected ||
        resource->bar.index >= PCI_STANDARD_BAR_COUNT ||
        resource->active_access_count > PCI_ASSIGNMENT_ACTIVE_ACCESS_LIMIT) return false;

    if (resource->bounded) {
        if (!resource->bar.size || (resource->bar.size & (resource->bar.size - 1ULL)))
            return false;
        if (resource->bar.base && (resource->bar.base & (resource->bar.size - 1ULL)))
            return false;
    } else if (resource->bar.size) {
        return false;
    }
    return true;
}

static bool duplicate_locked(PciAssignment *assignment, u8 bar_index) {
    for (u32 i = 0U; i < PCI_BAR_RESOURCE_STORAGE_CAPACITY; ++i) {
        PciBarResource *resource = (PciBarResource *)g_bar_resources[i];
        if (resource && resource->initialized && resource->assignment == assignment &&
            resource->bar.index == bar_index) return true;
    }
    return false;
}

bool pci_bar_resource_valid(const PciBarResource *resource) {
    u64 flags = bar_resource_irq_save();
    bool valid = resource && bar_resource_valid_locked(resource);
    bar_resource_irq_restore(flags);
    return valid;
}

u32 pci_bar_resource_object_count(void) {
    u64 flags = bar_resource_irq_save();
    u32 count = object_storage_count(g_bar_resources, PCI_BAR_RESOURCE_STORAGE_CAPACITY);
    bar_resource_irq_restore(flags);
    return count;
}

bool pci_bar_resource_create(PciBarResource *resource, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle, u8 bar_index,
    CapabilityHandle *out_handle) {
    if (!out_handle) return false;
    *out_handle = CAPABILITY_INVALID_HANDLE;
    if (!resource || !assignment || !process || bar_index >= PCI_STANDARD_BAR_COUNT)
        return false;

    u64 flags = bar_resource_irq_save();
    bool child_acquired = false;
    bool device_resource_created = false;
    bool created = false;

    if (bar_resource_live(resource) || duplicate_locked(assignment, bar_index)) goto done;
    if (!pci_assignment_child_acquire(assignment, process, assignment_handle)) goto done;
    child_acquired = true;

    const PciDevice *device = pci_assignment_enumerated_device(assignment);
    PciBar descriptor;
    k_memset(&descriptor, 0, sizeof(descriptor));
    if (!device || !pci_read_bar(device, bar_index, &descriptor) ||
        kind_for_bar(descriptor.type) == DEVICE_RESOURCE_NONE) goto rollback;

    PciBar final_bar = descriptor;
    bool bounded = false;
    PciBar probed;
    k_memset(&probed, 0, sizeof(probed));
    if (pci_probe_bar(device, bar_index, &probed)) {
        if (probed.type != descriptor.type || probed.index != descriptor.index ||
            probed.base != descriptor.base || probed.prefetchable != descriptor.prefetchable ||
            !probed.size) goto rollback;
        final_bar = probed;
        bounded = true;
    } else {
        u16 command = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
        if ((command & PCI_COMMAND_ACTIVE_MASK) == 0U) goto rollback;
        final_bar.size = 0ULL;
    }

    u32 slot = object_storage_empty(g_bar_resources, PCI_BAR_RESOURCE_STORAGE_CAPACITY);
    if (slot >= PCI_BAR_RESOURCE_STORAGE_CAPACITY) goto rollback;

    k_memset(resource, 0, sizeof(*resource));
    if (!device_resource_create(&resource->resource, kind_for_bar(final_bar.type))) goto rollback;
    device_resource_created = true;

    DeviceResource *parent = pci_assignment_resource(assignment);
    if (!parent || !parent->id) goto rollback;

    resource->assignment = assignment;
    resource->assignment_resource_id = parent->id;
    resource->owner_process_id = process->id;
    resource->bar = final_bar;
    resource->bounded = bounded;
    resource->initialized = true;
    g_bar_resources[slot] = resource;

    CapabilityTable *caps = process_capabilities(process);
    if (!caps || !capability_insert(caps, &resource->resource,
            CAPABILITY_TYPE_DEVICE_RESOURCE, PCI_BAR_RESOURCE_OWNER_RIGHTS, out_handle)) {
        g_bar_resources[slot] = 0;
        resource->initialized = false;
        goto rollback;
    }

    created = true;
    goto done;

rollback:
    if (device_resource_created && device_resource_valid(&resource->resource)) {
        if (resource->resource.capability_refs || !device_resource_destroy(&resource->resource))
            cpu_halt_forever();
    }
    k_memset(resource, 0, sizeof(*resource));
    if (child_acquired && !pci_assignment_child_release(assignment, process->id))
        cpu_halt_forever();

done:
    bar_resource_irq_restore(flags);
    return created;
}

bool pci_bar_resource_destroy(PciBarResource *resource) {
    if (!resource) return false;
    u64 flags = bar_resource_irq_save();
    u32 slot = object_storage_find(g_bar_resources, PCI_BAR_RESOURCE_STORAGE_CAPACITY,
        resource);
    bool destroyed = slot < PCI_BAR_RESOURCE_STORAGE_CAPACITY &&
        bar_resource_valid_locked(resource) && !resource->active_access_count &&
        !resource->resource.capability_refs;
    if (destroyed) {
        PciAssignment *assignment = resource->assignment;
        u64 owner = resource->owner_process_id;
        if (!device_resource_destroy(&resource->resource)) {
            bar_resource_irq_restore(flags);
            return false;
        }
        g_bar_resources[slot] = 0;
        k_memset(resource, 0, sizeof(*resource));
        if (!pci_assignment_child_release(assignment, owner)) cpu_halt_forever();
    }
    bar_resource_irq_restore(flags);
    return destroyed;
}

bool pci_bar_resource_descriptor(const PciBarResource *resource, PciBar *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    u64 flags = bar_resource_irq_save();
    bool valid = resource && bar_resource_valid_locked(resource);
    if (valid) *out = resource->bar;
    bar_resource_irq_restore(flags);
    return valid;
}

bool pci_bar_resource_bounded(const PciBarResource *resource) {
    u64 flags = bar_resource_irq_save();
    bool bounded = resource && bar_resource_valid_locked(resource) && resource->bounded;
    bar_resource_irq_restore(flags);
    return bounded;
}

u64 pci_bar_resource_owner_process_id(const PciBarResource *resource) {
    u64 flags = bar_resource_irq_save();
    u64 owner = resource && bar_resource_valid_locked(resource) ?
        resource->owner_process_id : 0ULL;
    bar_resource_irq_restore(flags);
    return owner;
}

u32 pci_bar_resource_active_access_count(const PciBarResource *resource) {
    u64 flags = bar_resource_irq_save();
    u32 count = resource && bar_resource_valid_locked(resource) ?
        resource->active_access_count : 0U;
    bar_resource_irq_restore(flags);
    return count;
}

u32 pci_bar_resource_assignment_active_access_count(const PciAssignment *assignment) {
    if (!assignment) return 0U;
    u64 flags = bar_resource_irq_save();
    u32 count = 0U;
    for (u32 i = 0U; i < PCI_BAR_RESOURCE_STORAGE_CAPACITY; ++i) {
        PciBarResource *resource = (PciBarResource *)g_bar_resources[i];
        if (!resource || !bar_resource_valid_locked(resource) ||
            resource->assignment != assignment) continue;
        if (count > ~0U - resource->active_access_count) {
            count = ~0U;
            break;
        }
        count += resource->active_access_count;
    }
    bar_resource_irq_restore(flags);
    return count;
}

bool pci_bar_resource_authorized(PciBarResource *resource, Process *process,
    CapabilityHandle handle, CapabilityRights required_rights) {
    const CapabilityRights allowed = PCI_BAR_RESOURCE_OWNER_RIGHTS;
    if (!resource || !process || !handle || !required_rights ||
        (required_rights & ~allowed)) return false;

    u64 flags = bar_resource_irq_save();
    void *resolved = 0;
    CapabilityTable *caps = process_storage_in_use(process) && process->initialized &&
        !process->kernel && process->id == resource->owner_process_id ?
        process_capabilities(process) : 0;
    bool authorized = caps && bar_resource_valid_locked(resource) &&
        pci_assignment_state(resource->assignment) == PCI_ASSIGNMENT_ASSIGNED &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_DEVICE_RESOURCE,
            required_rights, &resolved) && resolved == &resource->resource;
    bar_resource_irq_restore(flags);
    return authorized;
}

bool pci_bar_resource_access_acquire(PciBarResource *resource, Process *process,
    CapabilityHandle handle, CapabilityRights required_rights) {
    if (!resource || !process || !handle || !required_rights) return false;
    u64 flags = bar_resource_irq_save();
    bool acquired = bar_resource_valid_locked(resource) && resource->bounded &&
        resource->active_access_count < PCI_ASSIGNMENT_ACTIVE_ACCESS_LIMIT &&
        !pci_assignment_dma_gate_active(resource->assignment) &&
        pci_bar_resource_authorized(resource, process, handle, required_rights) &&
        pci_assignment_access_acquire(resource->assignment, resource->owner_process_id);
    if (acquired) ++resource->active_access_count;
    bar_resource_irq_restore(flags);
    return acquired;
}

bool pci_bar_resource_access_release(PciBarResource *resource, u64 owner_process_id) {
    if (!resource || !owner_process_id) return false;
    u64 flags = bar_resource_irq_save();
    bool released = bar_resource_valid_locked(resource) &&
        resource->owner_process_id == owner_process_id &&
        resource->active_access_count != 0U &&
        pci_assignment_access_release(resource->assignment, owner_process_id);
    if (released) --resource->active_access_count;
    bar_resource_irq_restore(flags);
    return released;
}

bool pci_bar_resource_contains(const PciBarResource *resource, u64 offset, u64 width) {
    if (!width) return false;
    u64 flags = bar_resource_irq_save();
    bool contains = false;
    if (resource && bar_resource_valid_locked(resource) && resource->bounded &&
        offset < resource->bar.size && width <= resource->bar.size - offset)
        contains = true;
    bar_resource_irq_restore(flags);
    return contains;
}
