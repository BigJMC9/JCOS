#include "pci_irq_router.h"

#include "arch.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "lib.h"
#include "pci.h"

#define PCI_IRQ_ROUTER_RFLAGS_IF (1ULL << 9)

typedef struct {
    PciIrqResource *irq;
    u64 resource_id;
    u64 owner_process_id;
    InterruptControllerGsiLease lease;
    u8 legacy_irq;
    u8 vector;
    bool source_masked;
    bool occupied;
} PciIrqBinding;

static PciIrqBinding g_bindings[PCI_IRQ_ROUTER_BINDING_CAPACITY];

static u64 router_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void router_irq_restore(u64 flags) {
    if (flags & PCI_IRQ_ROUTER_RFLAGS_IF) interrupts_enable();
}

static bool vector_valid(u8 vector) {
    return vector >= PCI_IRQ_ROUTER_VECTOR_FIRST &&
        vector <= PCI_IRQ_ROUTER_VECTOR_LAST;
}

static PciIrqBinding *binding_for_irq_locked(const PciIrqResource *irq) {
    if (!irq) return 0;
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i) {
        PciIrqBinding *binding = &g_bindings[i];
        if (binding->occupied && binding->irq == irq) return binding;
    }
    return 0;
}

static PciIrqBinding *binding_for_vector_locked(u8 vector) {
    if (!vector_valid(vector)) return 0;
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i) {
        PciIrqBinding *binding = &g_bindings[i];
        if (binding->occupied && binding->vector == vector) return binding;
    }
    return 0;
}

static bool gsi_in_use_locked(u32 gsi) {
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i) {
        const PciIrqBinding *binding = &g_bindings[i];
        if (binding->occupied && binding->lease.valid && binding->lease.gsi == gsi)
            return true;
    }
    return false;
}

static bool binding_valid_locked(const PciIrqBinding *binding) {
    if (!binding || !binding->occupied || !binding->irq || !binding->lease.valid ||
        !vector_valid(binding->vector) || binding->lease.vector != binding->vector ||
        !pci_irq_resource_valid(binding->irq) ||
        binding->resource_id != binding->irq->resource.id ||
        binding->owner_process_id != pci_irq_resource_owner_process_id(binding->irq))
        return false;
    bool hardware_masked = true;
    return interrupt_controller_gsi_lease_query(&binding->lease, &hardware_masked) &&
        hardware_masked == binding->source_masked;
}

static bool owner_authorized_locked(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    if (!irq || !process || !resource_handle || !pci_irq_resource_valid(irq) ||
        !process_storage_in_use(process) || !process->initialized || process->kernel ||
        process->id != pci_irq_resource_owner_process_id(irq) ||
        pci_assignment_state(irq->assignment) != PCI_ASSIGNMENT_ASSIGNED) return false;
    CapabilityTable *caps = process_capabilities(process);
    void *resolved = 0;
    return caps && capability_lookup_rights(caps, resource_handle,
        CAPABILITY_TYPE_DEVICE_RESOURCE, PCI_IRQ_RESOURCE_OWNER_RIGHTS, &resolved) &&
        resolved == &irq->resource;
}

static u8 allocate_vector_locked(void) {
    for (u32 vector = PCI_IRQ_ROUTER_VECTOR_FIRST;
         vector <= PCI_IRQ_ROUTER_VECTOR_LAST; ++vector) {
        if (!binding_for_vector_locked((u8)vector)) return (u8)vector;
    }
    return 0U;
}

static u32 empty_binding_locked(void) {
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i)
        if (!g_bindings[i].occupied) return i;
    return PCI_IRQ_ROUTER_BINDING_CAPACITY;
}

bool pci_irq_router_bind_legacy_intx(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    if (!irq || !process || !resource_handle) return false;
    u64 flags = router_irq_save();
    bool bound = false;
    if (!owner_authorized_locked(irq, process, resource_handle) ||
        pci_irq_resource_state(irq) != PCI_IRQ_STATE_DISARMED ||
        binding_for_irq_locked(irq)) goto done;

    const PciDevice *device = pci_assignment_enumerated_device(irq->assignment);
    if (!device || device->interrupt_pin < 1U || device->interrupt_pin > 4U ||
        device->interrupt_line < 3U || device->interrupt_line >= ACPI_LEGACY_IRQ_COUNT)
        goto done;

    u32 gsi = 0U;
    u16 route_flags = 0U;
    if (!interrupt_controller_legacy_irq_route(device->interrupt_line, &gsi,
            &route_flags) || gsi_in_use_locked(gsi)) goto done;

    u8 vector = allocate_vector_locked();
    u32 slot = empty_binding_locked();
    if (!vector || slot >= PCI_IRQ_ROUTER_BINDING_CAPACITY) goto done;

    InterruptControllerGsiLease lease;
    k_memset(&lease, 0, sizeof(lease));
    if (!interrupt_controller_gsi_lease_acquire(gsi, route_flags, vector, &lease))
        goto done;

    PciIrqBinding *binding = &g_bindings[slot];
    k_memset(binding, 0, sizeof(*binding));
    binding->irq = irq;
    binding->resource_id = irq->resource.id;
    binding->owner_process_id = process->id;
    binding->lease = lease;
    binding->legacy_irq = device->interrupt_line;
    binding->vector = vector;
    binding->source_masked = true;
    binding->occupied = true;
    bound = true;

done:
    router_irq_restore(flags);
    return bound;
}

static bool release_binding_locked(PciIrqBinding *binding) {
    if (!binding_valid_locked(binding) || !binding->source_masked ||
        pci_irq_resource_state(binding->irq) != PCI_IRQ_STATE_DISARMED) return false;
    if (!interrupt_controller_gsi_lease_release(&binding->lease)) return false;
    k_memset(binding, 0, sizeof(*binding));
    return true;
}

bool pci_irq_router_unbind(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool unbound = binding && owner_authorized_locked(irq, process, resource_handle) &&
        release_binding_locked(binding);
    router_irq_restore(flags);
    return unbound;
}

bool pci_irq_router_arm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool armed = binding && binding_valid_locked(binding) && binding->source_masked &&
        owner_authorized_locked(irq, process, resource_handle) &&
        pci_irq_resource_arm(irq, process, resource_handle);
    if (armed) {
        if (!interrupt_controller_gsi_lease_unmask(&binding->lease)) {
            if (!pci_irq_resource_force_disarm(irq, binding->owner_process_id))
                cpu_halt_forever();
            armed = false;
        } else {
            binding->source_masked = false;
        }
    }
    router_irq_restore(flags);
    return armed;
}

static bool force_disarm_locked(PciIrqBinding *binding, u64 owner_process_id) {
    if (!binding_valid_locked(binding) || binding->owner_process_id != owner_process_id)
        return false;
    if (!binding->source_masked) {
        if (!interrupt_controller_gsi_lease_mask(&binding->lease)) return false;
        binding->source_masked = true;
    }
    return pci_irq_resource_force_disarm(binding->irq, owner_process_id);
}

bool pci_irq_router_force_disarm(PciIrqResource *irq, u64 owner_process_id) {
    if (!irq || !owner_process_id) return false;
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool disarmed = binding && force_disarm_locked(binding, owner_process_id);
    router_irq_restore(flags);
    return disarmed;
}

bool pci_irq_router_disarm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool disarmed = false;
    if (binding && binding_valid_locked(binding) &&
        owner_authorized_locked(irq, process, resource_handle)) {
        if (!binding->source_masked) {
            if (!interrupt_controller_gsi_lease_mask(&binding->lease)) goto done;
            binding->source_masked = true;
        }
        disarmed = pci_irq_resource_disarm(irq, process, resource_handle);
    }
done:
    router_irq_restore(flags);
    return disarmed;
}

bool pci_irq_router_acknowledge(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle, u64 sequence) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool acknowledged = binding && binding_valid_locked(binding) &&
        binding->source_masked && owner_authorized_locked(irq, process, resource_handle) &&
        pci_irq_resource_acknowledge(irq, process, resource_handle, sequence);
    if (acknowledged && pci_irq_resource_state(irq) == PCI_IRQ_STATE_ARMED) {
        if (!interrupt_controller_gsi_lease_unmask(&binding->lease)) {
            if (!pci_irq_resource_force_disarm(irq, binding->owner_process_id))
                cpu_halt_forever();
            acknowledged = false;
        } else {
            binding->source_masked = false;
        }
    }
    router_irq_restore(flags);
    return acknowledged;
}

bool pci_irq_router_recover_storm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool recovered = binding && binding_valid_locked(binding) && binding->source_masked &&
        owner_authorized_locked(irq, process, resource_handle) &&
        pci_irq_resource_recover_storm(irq, process, resource_handle);
    if (recovered) {
        if (!interrupt_controller_gsi_lease_unmask(&binding->lease)) {
            if (!pci_irq_resource_force_disarm(irq, binding->owner_process_id))
                cpu_halt_forever();
            recovered = false;
        } else {
            binding->source_masked = false;
        }
    }
    router_irq_restore(flags);
    return recovered;
}

bool pci_irq_router_force_unbind(PciIrqResource *irq, u64 owner_process_id) {
    if (!irq || !owner_process_id) return false;
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool unbound = false;
    if (binding && binding_valid_locked(binding) &&
        binding->owner_process_id == owner_process_id) {
        if (pci_irq_resource_state(irq) != PCI_IRQ_STATE_DISARMED &&
            !force_disarm_locked(binding, owner_process_id)) goto done;
        unbound = release_binding_locked(binding);
    }
done:
    router_irq_restore(flags);
    return unbound;
}

bool pci_irq_router_force_quiesce_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = router_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i) {
        PciIrqBinding *binding = &g_bindings[i];
        if (!binding->occupied) continue;
        if (!binding_valid_locked(binding)) { ok = false; break; }
        if (binding->irq->assignment != assignment) continue;
        if (binding->owner_process_id != owner_process_id ||
            !force_disarm_locked(binding, owner_process_id)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    router_irq_restore(flags);
    return ok;
}

bool pci_irq_router_force_unbind_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = router_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i) {
        PciIrqBinding *binding = &g_bindings[i];
        if (!binding->occupied) continue;
        if (!binding_valid_locked(binding)) { ok = false; break; }
        if (binding->irq->assignment != assignment) continue;
        if (binding->owner_process_id != owner_process_id ||
            !binding->source_masked || !release_binding_locked(binding)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    router_irq_restore(flags);
    return ok;
}

bool pci_irq_router_handle_vector(u8 vector) {
    if (!vector_valid(vector)) return false;
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_vector_locked(vector);
    if (!binding || !binding_valid_locked(binding)) {
        router_irq_restore(flags);
        return false;
    }

    if (!binding->source_masked) {
        if (!interrupt_controller_gsi_lease_mask(&binding->lease)) cpu_halt_forever();
        binding->source_masked = true;
    }
    (void)pci_irq_resource_signal(binding->irq);
    router_irq_restore(flags);
    return true;
}

bool pci_irq_router_bound(const PciIrqResource *irq) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool bound = binding && binding_valid_locked(binding);
    router_irq_restore(flags);
    return bound;
}

u8 pci_irq_router_vector(const PciIrqResource *irq) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    u8 vector = binding && binding_valid_locked(binding) ? binding->vector : 0U;
    router_irq_restore(flags);
    return vector;
}

u32 pci_irq_router_gsi(const PciIrqResource *irq) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    u32 gsi = binding && binding_valid_locked(binding) ? binding->lease.gsi : 0U;
    router_irq_restore(flags);
    return gsi;
}

bool pci_irq_router_source_masked(const PciIrqResource *irq) {
    u64 flags = router_irq_save();
    PciIrqBinding *binding = binding_for_irq_locked(irq);
    bool masked = binding && binding_valid_locked(binding) && binding->source_masked;
    router_irq_restore(flags);
    return masked;
}

u32 pci_irq_router_binding_count(void) {
    u64 flags = router_irq_save();
    u32 count = 0U;
    for (u32 i = 0U; i < PCI_IRQ_ROUTER_BINDING_CAPACITY; ++i)
        if (g_bindings[i].occupied) ++count;
    router_irq_restore(flags);
    return count;
}
