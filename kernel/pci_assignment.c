#include "pci_assignment.h"

#include "capability.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"

#define PCI_ASSIGNMENT_STORAGE_CAPACITY 32U
#define PCI_ASSIGNMENT_RFLAGS_IF (1ULL << 9)

static void *g_assignments[PCI_ASSIGNMENT_STORAGE_CAPACITY];

static u64 assignment_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void assignment_irq_restore(u64 flags) {
    if (flags & PCI_ASSIGNMENT_RFLAGS_IF) interrupts_enable();
}

static bool address_equal(PciAddress a, PciAddress b) {
    return a.bus == b.bus && a.device == b.device && a.function == b.function;
}

static bool state_valid(PciAssignmentState state) {
    return state == PCI_ASSIGNMENT_AVAILABLE ||
        state == PCI_ASSIGNMENT_ASSIGNED ||
        state == PCI_ASSIGNMENT_QUIESCING ||
        state == PCI_ASSIGNMENT_QUARANTINED;
}

static bool assignment_live(const PciAssignment *assignment) {
    return object_storage_find(g_assignments, PCI_ASSIGNMENT_STORAGE_CAPACITY,
        assignment) < PCI_ASSIGNMENT_STORAGE_CAPACITY;
}

static bool assignment_valid_locked(const PciAssignment *assignment) {
    return assignment_live(assignment) && assignment->initialized &&
        state_valid(assignment->state) && assignment->device.vendor_id != 0xFFFFU &&
        assignment->device.address.device < 32U &&
        assignment->device.address.function < 8U &&
        device_resource_valid(&assignment->resource) &&
        assignment->resource.kind == DEVICE_RESOURCE_PCI_FUNCTION &&
        assignment->child_resource_count <= PCI_ASSIGNMENT_CHILD_RESOURCE_LIMIT &&
        assignment->active_access_count <= PCI_ASSIGNMENT_ACTIVE_ACCESS_LIMIT &&
        ((assignment->state == PCI_ASSIGNMENT_AVAILABLE && !assignment->owner_process_id &&
          !assignment->child_resource_count && !assignment->active_access_count &&
          !assignment->dma_gate_active && !assignment->bus_master_exposed) ||
         (assignment->state == PCI_ASSIGNMENT_ASSIGNED && assignment->owner_process_id) ||
         (assignment->state == PCI_ASSIGNMENT_QUIESCING && assignment->owner_process_id &&
          !assignment->dma_gate_active) ||
         (assignment->state == PCI_ASSIGNMENT_QUARANTINED && assignment->owner_process_id));
}

static PciAssignment *assignment_for_address_locked(PciAddress address) {
    for (u32 i = 0U; i < PCI_ASSIGNMENT_STORAGE_CAPACITY; ++i) {
        PciAssignment *assignment = (PciAssignment *)g_assignments[i];
        if (assignment && assignment->initialized &&
            address_equal(assignment->device.address, address)) return assignment;
    }
    return 0;
}

bool pci_assignment_valid(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    bool valid = assignment && assignment_valid_locked(assignment);
    assignment_irq_restore(flags);
    return valid;
}

u32 pci_assignment_object_count(void) {
    u64 flags = assignment_irq_save();
    u32 count = object_storage_count(g_assignments, PCI_ASSIGNMENT_STORAGE_CAPACITY);
    assignment_irq_restore(flags);
    return count;
}

bool pci_assignment_create(PciAssignment *assignment, const PciDevice *device) {
    if (!assignment || !pci_device_is_enumerated(device)) return false;

    u64 flags = assignment_irq_save();
    bool created = false;
    u32 slot = object_storage_empty(g_assignments, PCI_ASSIGNMENT_STORAGE_CAPACITY);
    if (slot >= PCI_ASSIGNMENT_STORAGE_CAPACITY || assignment_live(assignment) ||
        assignment_for_address_locked(device->address)) goto done;

    k_memset(assignment, 0, sizeof(*assignment));
    if (!device_resource_create(&assignment->resource, DEVICE_RESOURCE_PCI_FUNCTION))
        goto done;

    assignment->device = *device;
    assignment->state = PCI_ASSIGNMENT_AVAILABLE;
    assignment->initialized = true;
    g_assignments[slot] = assignment;
    created = true;

done:
    assignment_irq_restore(flags);
    return created;
}

bool pci_assignment_destroy(PciAssignment *assignment) {
    if (!assignment) return false;

    u64 flags = assignment_irq_save();
    u32 slot = object_storage_find(g_assignments, PCI_ASSIGNMENT_STORAGE_CAPACITY,
        assignment);
    bool destroyed = slot < PCI_ASSIGNMENT_STORAGE_CAPACITY &&
        assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_AVAILABLE &&
        !assignment->owner_process_id && !assignment->child_resource_count &&
        !assignment->active_access_count && !assignment->dma_gate_active &&
        !assignment->resource.capability_refs &&
        device_resource_destroy(&assignment->resource);
    if (destroyed) {
        g_assignments[slot] = 0;
        k_memset(assignment, 0, sizeof(*assignment));
    }
    assignment_irq_restore(flags);
    return destroyed;
}

PciAssignmentState pci_assignment_state(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    PciAssignmentState state = assignment && assignment_valid_locked(assignment) ?
        assignment->state : PCI_ASSIGNMENT_INVALID;
    assignment_irq_restore(flags);
    return state;
}

u64 pci_assignment_owner_process_id(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    u64 owner = assignment && assignment_valid_locked(assignment) ?
        assignment->owner_process_id : 0ULL;
    assignment_irq_restore(flags);
    return owner;
}

const PciDevice *pci_assignment_device(const PciAssignment *assignment) {
    return pci_assignment_valid(assignment) ? &assignment->device : 0;
}

const PciDevice *pci_assignment_enumerated_device(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    const PciDevice *found = 0;
    if (assignment && assignment_valid_locked(assignment)) {
        for (u32 i = 0U; i < pci_device_count(); ++i) {
            const PciDevice *device = pci_device(i);
            if (!device || !address_equal(device->address, assignment->device.address)) continue;
            if (device->vendor_id == assignment->device.vendor_id &&
                device->device_id == assignment->device.device_id &&
                device->revision == assignment->device.revision) {
                found = device;
                break;
            }
        }
    }
    assignment_irq_restore(flags);
    return found;
}

DeviceResource *pci_assignment_resource(PciAssignment *assignment) {
    return pci_assignment_valid(assignment) ? &assignment->resource : 0;
}

u32 pci_assignment_child_count(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    u32 count = assignment && assignment_valid_locked(assignment) ?
        assignment->child_resource_count : 0U;
    assignment_irq_restore(flags);
    return count;
}

u32 pci_assignment_active_access_count(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    u32 count = assignment && assignment_valid_locked(assignment) ?
        assignment->active_access_count : 0U;
    assignment_irq_restore(flags);
    return count;
}

bool pci_assignment_dma_gate_active(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    bool active = assignment && assignment_valid_locked(assignment) &&
        assignment->dma_gate_active;
    assignment_irq_restore(flags);
    return active;
}

bool pci_assignment_bus_master_exposed(const PciAssignment *assignment) {
    u64 flags = assignment_irq_save();
    bool exposed = assignment && assignment_valid_locked(assignment) &&
        assignment->bus_master_exposed;
    assignment_irq_restore(flags);
    return exposed;
}

PciAssignment *pci_assignment_from_resource(DeviceResource *resource) {
    if (!resource) return 0;
    u64 flags = assignment_irq_save();
    PciAssignment *found = 0;
    for (u32 i = 0U; i < PCI_ASSIGNMENT_STORAGE_CAPACITY; ++i) {
        PciAssignment *assignment = (PciAssignment *)g_assignments[i];
        if (assignment && assignment_valid_locked(assignment) &&
            &assignment->resource == resource) {
            found = assignment;
            break;
        }
    }
    assignment_irq_restore(flags);
    return found;
}

static bool assignment_authorized_locked(PciAssignment *assignment, Process *process,
    CapabilityHandle handle) {
    void *resolved = 0;
    if (!assignment_valid_locked(assignment)) return false;
    bool process_valid = process_storage_in_use(process) && process->initialized &&
        (!process->kernel || (assignment && assignment->supervisor_owned && process==process_kernel())) && process->id;
    CapabilityTable *caps = process_valid ? process_capabilities(process) : 0;
    return caps && assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == process->id &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_MANAGE, &resolved) && resolved == &assignment->resource;
}

static bool claim(PciAssignment *assignment, Process *process,CapabilityHandle *out_handle,bool supervised) {
    if (!out_handle) return false;
    *out_handle = CAPABILITY_INVALID_HANDLE;
    if (!assignment || !process) return false;

    u64 flags = assignment_irq_save();
    bool claimed = false;
    bool process_valid = process_storage_in_use(process) && process->initialized &&
        (supervised ? (process->kernel && process==process_kernel()) : !process->kernel) && process->id;
    CapabilityTable *caps = process_valid ? process_capabilities(process) : 0;
    if (!caps || !assignment_valid_locked(assignment) ||
        assignment->state != PCI_ASSIGNMENT_AVAILABLE ||
        assignment->owner_process_id || assignment->child_resource_count ||
        assignment->resource.capability_refs) goto done;

    if (!capability_insert(caps, &assignment->resource,
            CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_MANAGE, out_handle))
        goto done;

    assignment->owner_process_id = process->id;
    assignment->supervisor_owned=supervised;
    assignment->state = PCI_ASSIGNMENT_ASSIGNED;
    claimed = true;

done:
    assignment_irq_restore(flags);
    return claimed;
}

bool pci_assignment_claim(PciAssignment *assignment,Process *process,CapabilityHandle *out) {
    return claim(assignment,process,out,false);
}
bool pci_assignment_claim_supervised(PciAssignment *assignment,CapabilityHandle *out) {
    return claim(assignment,process_kernel(),out,true);
}

bool pci_assignment_authorized(PciAssignment *assignment, Process *process,
    CapabilityHandle handle) {
    if (!assignment || !process || !handle) return false;
    u64 flags = assignment_irq_save();
    bool authorized = assignment_authorized_locked(assignment, process, handle);
    assignment_irq_restore(flags);
    return authorized;
}

bool pci_assignment_child_acquire(PciAssignment *assignment, Process *process,
    CapabilityHandle assignment_handle) {
    if (!assignment || !process || !assignment_handle) return false;
    u64 flags = assignment_irq_save();
    bool acquired = assignment_authorized_locked(assignment, process, assignment_handle) &&
        assignment->child_resource_count < PCI_ASSIGNMENT_CHILD_RESOURCE_LIMIT;
    if (acquired) ++assignment->child_resource_count;
    assignment_irq_restore(flags);
    return acquired;
}

bool pci_assignment_child_release(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool released = assignment_valid_locked(assignment) &&
        assignment->state != PCI_ASSIGNMENT_AVAILABLE &&
        assignment->owner_process_id == owner_process_id &&
        assignment->child_resource_count != 0U;
    if (released) --assignment->child_resource_count;
    assignment_irq_restore(flags);
    return released;
}

bool pci_assignment_access_acquire(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool acquired = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == owner_process_id &&
        assignment->active_access_count < PCI_ASSIGNMENT_ACTIVE_ACCESS_LIMIT;
    if (acquired) ++assignment->active_access_count;
    assignment_irq_restore(flags);
    return acquired;
}

bool pci_assignment_access_release(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool released = assignment_valid_locked(assignment) &&
        assignment->state != PCI_ASSIGNMENT_AVAILABLE &&
        assignment->owner_process_id == owner_process_id &&
        assignment->active_access_count != 0U;
    if (released) --assignment->active_access_count;
    assignment_irq_restore(flags);
    return released;
}

bool pci_assignment_dma_gate_acquire(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool acquired = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == owner_process_id &&
        !assignment->dma_gate_active;
    if (acquired) assignment->dma_gate_active = true;
    assignment_irq_restore(flags);
    return acquired;
}

bool pci_assignment_dma_gate_release(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool released = assignment_valid_locked(assignment) &&
        (assignment->state == PCI_ASSIGNMENT_ASSIGNED ||
         assignment->state == PCI_ASSIGNMENT_QUARANTINED) &&
        assignment->owner_process_id == owner_process_id &&
        assignment->dma_gate_active;
    if (released) assignment->dma_gate_active = false;
    assignment_irq_restore(flags);
    return released;
}

bool pci_assignment_mark_bus_master_exposed(PciAssignment *assignment,
    u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool marked = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == owner_process_id &&
        assignment->dma_gate_active && assignment->active_access_count != 0U;
    if (marked) assignment->bus_master_exposed = true;
    assignment_irq_restore(flags);
    return marked;
}

bool pci_assignment_note_inherited_bus_master(PciAssignment *assignment,
    u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool marked = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == owner_process_id;
    if (marked) assignment->bus_master_exposed = true;
    assignment_irq_restore(flags);
    return marked;
}

bool pci_assignment_begin_quiesce(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool begun = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_ASSIGNED &&
        assignment->owner_process_id == owner_process_id &&
        !assignment->active_access_count && !assignment->dma_gate_active &&
        !assignment->bus_master_exposed;
    if (begun) assignment->state = PCI_ASSIGNMENT_QUIESCING;
    assignment_irq_restore(flags);
    return begun;
}

bool pci_assignment_complete_release(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool released = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_QUIESCING &&
        assignment->owner_process_id == owner_process_id &&
        !assignment->child_resource_count && !assignment->active_access_count &&
        !assignment->dma_gate_active && !assignment->bus_master_exposed &&
        !assignment->resource.capability_refs;
    if (released) {
        assignment->owner_process_id = 0ULL;
        assignment->state = PCI_ASSIGNMENT_AVAILABLE;
    }
    assignment_irq_restore(flags);
    return released;
}

bool pci_assignment_quarantine(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool quarantined = assignment_valid_locked(assignment) &&
        (assignment->state == PCI_ASSIGNMENT_ASSIGNED ||
         assignment->state == PCI_ASSIGNMENT_QUIESCING) &&
        assignment->owner_process_id == owner_process_id &&
        !assignment->active_access_count && !assignment->dma_gate_active;
    if (quarantined) assignment->state = PCI_ASSIGNMENT_QUARANTINED;
    assignment_irq_restore(flags);
    return quarantined;
}

bool pci_assignment_force_quarantine(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool quarantined = assignment_valid_locked(assignment) &&
        (assignment->state == PCI_ASSIGNMENT_ASSIGNED ||
         assignment->state == PCI_ASSIGNMENT_QUIESCING ||
         assignment->state == PCI_ASSIGNMENT_QUARANTINED) &&
        assignment->owner_process_id == owner_process_id;
    if (quarantined) assignment->state = PCI_ASSIGNMENT_QUARANTINED;
    assignment_irq_restore(flags);
    return quarantined;
}

bool pci_assignment_complete_quarantine(PciAssignment *assignment, u64 owner_process_id) {
    if (!assignment || !owner_process_id) return false;
    u64 flags = assignment_irq_save();
    bool recovered = assignment_valid_locked(assignment) &&
        assignment->state == PCI_ASSIGNMENT_QUARANTINED &&
        assignment->owner_process_id == owner_process_id &&
        !assignment->child_resource_count && !assignment->active_access_count &&
        !assignment->dma_gate_active && !assignment->resource.capability_refs;
    if (recovered) {
        assignment->owner_process_id = 0ULL;
        assignment->bus_master_exposed = false;
        assignment->state = PCI_ASSIGNMENT_AVAILABLE;
    }
    assignment_irq_restore(flags);
    return recovered;
}

bool pci_assignment_reset_complete(PciAssignment *assignment, u64 owner_process_id) {
    return pci_assignment_complete_quarantine(assignment, owner_process_id);
}
