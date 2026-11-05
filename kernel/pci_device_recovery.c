#include "pci_device_recovery.h"

#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "pci.h"
#include "pci_bar_access.h"
#include "pci_bar_resource.h"
#include "pci_dma_buffer.h"
#include "pci_dma_submission.h"
#include "pci_irq_resource.h"
#include "pci_irq_router.h"
#include "pci_flr.h"

#define PCI_DEVICE_RECOVERY_RFLAGS_IF (1ULL << 9)
#define PCI_COMMAND_OFFSET 0x04U

static void *g_recoveries[PCI_DEVICE_RECOVERY_STORAGE_CAPACITY];

static u64 recovery_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void recovery_irq_restore(u64 flags) {
    if (flags & PCI_DEVICE_RECOVERY_RFLAGS_IF) interrupts_enable();
}

static bool recovery_live(const PciDeviceRecovery *recovery) {
    return object_storage_find(g_recoveries, PCI_DEVICE_RECOVERY_STORAGE_CAPACITY,
        recovery) < PCI_DEVICE_RECOVERY_STORAGE_CAPACITY;
}

static bool recovery_for_assignment_locked(const PciAssignment *assignment) {
    for (u32 i = 0U; i < PCI_DEVICE_RECOVERY_STORAGE_CAPACITY; ++i) {
        const PciDeviceRecovery *recovery =
            (const PciDeviceRecovery *)g_recoveries[i];
        if (recovery && recovery->assignment == assignment) return true;
    }
    return false;
}

static bool state_valid(PciDeviceRecoveryState state) {
    return state == PCI_DEVICE_RECOVERY_QUIESCED ||
        state == PCI_DEVICE_RECOVERY_DMA_SAFE ||
        state == PCI_DEVICE_RECOVERY_FAILED_QUARANTINED;
}

static bool recovery_valid_locked(const PciDeviceRecovery *recovery) {
    if (!recovery_live(recovery) || !recovery->initialized || !recovery->assignment ||
        !recovery->assignment_resource_id || !recovery->owner_process_id ||
        !state_valid(recovery->state) || !pci_assignment_valid(recovery->assignment) ||
        pci_assignment_owner_process_id(recovery->assignment) != recovery->owner_process_id ||
        recovery->reset_method > PCI_DEVICE_RESET_FLR ||
        (recovery->reset_completed && (!recovery->reset_attempted ||
            recovery->reset_method == PCI_DEVICE_RESET_NONE)))
        return false;
    DeviceResource *resource = pci_assignment_resource(recovery->assignment);
    return resource && resource->id == recovery->assignment_resource_id &&
        pci_assignment_state(recovery->assignment) == PCI_ASSIGNMENT_QUARANTINED;
}

static u16 quiesced_command(u16 command) {
    return (u16)((command | PCI_COMMAND_INT_DISABLE) &
        (u16)~(PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER));
}

bool pci_device_recovery_valid(const PciDeviceRecovery *recovery) {
    u64 flags = recovery_irq_save();
    bool valid = recovery && recovery_valid_locked(recovery);
    recovery_irq_restore(flags);
    return valid;
}

u32 pci_device_recovery_object_count(void) {
    u64 flags = recovery_irq_save();
    u32 count = object_storage_count(g_recoveries,
        PCI_DEVICE_RECOVERY_STORAGE_CAPACITY);
    recovery_irq_restore(flags);
    return count;
}

PciDeviceRecoveryState pci_device_recovery_state(const PciDeviceRecovery *recovery) {
    u64 flags = recovery_irq_save();
    PciDeviceRecoveryState state = recovery && recovery_valid_locked(recovery) ?
        recovery->state : PCI_DEVICE_RECOVERY_INVALID;
    recovery_irq_restore(flags);
    return state;
}

bool pci_device_recovery_reset_required(const PciDeviceRecovery *recovery) {
    u64 flags = recovery_irq_save();
    bool required = recovery && recovery_valid_locked(recovery) &&
        (recovery->bus_master_was_enabled || recovery->bus_master_exposed);
    recovery_irq_restore(flags);
    return required;
}

bool pci_device_recovery_flr_available(const PciDeviceRecovery *recovery) {
    u64 flags = recovery_irq_save();
    const PciDevice *device = recovery && recovery_valid_locked(recovery) &&
        recovery->state == PCI_DEVICE_RECOVERY_QUIESCED ?
        pci_assignment_enumerated_device(recovery->assignment) : 0;
    recovery_irq_restore(flags);
    u8 capability = 0U;
    return device && pci_flr_supported(device, &capability);
}

PciDeviceResetMethod pci_device_recovery_reset_method(
    const PciDeviceRecovery *recovery) {
    u64 flags = recovery_irq_save();
    PciDeviceResetMethod method = recovery && recovery_valid_locked(recovery) ?
        recovery->reset_method : PCI_DEVICE_RESET_NONE;
    recovery_irq_restore(flags);
    return method;
}

bool pci_device_recovery_begin(PciDeviceRecovery *recovery,
    PciAssignment *assignment, Process *owner_process) {
    if (!recovery || !assignment || !owner_process ||
        !process_storage_in_use(owner_process) || !owner_process->initialized ||
        owner_process->kernel || !owner_process->id ||
        process_thread_count(owner_process) != 0ULL || owner_process->thread_head ||
        owner_process->thread_tail) return false;
    u64 owner_process_id = owner_process->id;
    u64 flags = recovery_irq_save();
    bool result = false;
    if (recovery_live(recovery) || recovery_for_assignment_locked(assignment) ||
        !pci_assignment_valid(assignment) ||
        pci_assignment_owner_process_id(assignment) != owner_process_id ||
        pci_assignment_state(assignment) != PCI_ASSIGNMENT_ASSIGNED) goto done;

    const PciDevice *device = pci_assignment_enumerated_device(assignment);
    DeviceResource *resource = pci_assignment_resource(assignment);
    u32 slot = object_storage_empty(g_recoveries,
        PCI_DEVICE_RECOVERY_STORAGE_CAPACITY);
    if (!device || !resource || !resource->id ||
        slot >= PCI_DEVICE_RECOVERY_STORAGE_CAPACITY) goto done;

    u16 command_before = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    if (command_before == 0xFFFFU) goto done;

    bool bus_master_exposed = pci_assignment_bus_master_exposed(assignment);
    if (!pci_assignment_force_quarantine(assignment, owner_process_id)) goto done;

    k_memset(recovery, 0, sizeof(*recovery));
    recovery->assignment = assignment;
    recovery->assignment_resource_id = resource->id;
    recovery->owner_process_id = owner_process_id;
    recovery->command_before = command_before;
    recovery->command_quiesced = quiesced_command(command_before);
    recovery->bus_master_was_enabled =
        (command_before & PCI_COMMAND_BUS_MASTER) != 0U;
    recovery->bus_master_exposed = bus_master_exposed;
    recovery->state = PCI_DEVICE_RECOVERY_FAILED_QUARANTINED;
    recovery->initialized = true;
    g_recoveries[slot] = recovery;

    u32 ignored = 0U;
    if (!pci_irq_router_force_quiesce_assignment(assignment, owner_process_id,
            &recovery->irq_bindings_quiesced) ||
        !pci_irq_resource_force_disarm_assignment(assignment, owner_process_id,
            &recovery->irq_resources_disarmed)) goto done;

    pci_config_write16(device->address, PCI_COMMAND_OFFSET,
        recovery->command_quiesced);
    u16 command_after = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    if ((command_after & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER)) != 0U ||
        !(command_after & PCI_COMMAND_INT_DISABLE)) goto done;
    recovery->command_quiesced = command_after;

    if (!pci_bar_mmio_force_unmap_assignment(assignment, owner_process_id,
            &recovery->mmio_mappings_unmapped) ||
        !pci_dma_buffer_force_unmap_assignment(assignment, owner_process_id,
            &recovery->dma_mappings_unmapped) ||
        !pci_dma_submission_force_quarantine_assignment(assignment,
            owner_process_id, &recovery->dma_submissions_quarantined) ||
        !pci_dma_buffer_force_quarantine_assignment(assignment,
            owner_process_id, &recovery->dma_buffers_quarantined) ||
        pci_bar_resource_assignment_active_access_count(assignment) != 0U)
        goto done;

    if (!pci_irq_router_force_unbind_assignment(assignment, owner_process_id,
            &ignored)) goto done;

    recovery->state = PCI_DEVICE_RECOVERY_QUIESCED;
    result = true;

done:
    recovery_irq_restore(flags);
    return result;
}

static bool complete_dma_recovery_locked(PciDeviceRecovery *recovery) {
    u32 submissions = 0U;
    u32 buffers = 0U;
    if (!pci_dma_submission_recovery_complete_assignment(recovery->assignment,
            recovery->owner_process_id, &submissions) ||
        !pci_dma_buffer_recovery_complete_assignment(recovery->assignment,
            recovery->owner_process_id, &buffers) ||
        pci_dma_submission_assignment_count(recovery->assignment) != 0U ||
        pci_assignment_dma_gate_active(recovery->assignment) ||
        pci_assignment_active_access_count(recovery->assignment) != 0U)
        return false;
    (void)submissions;
    (void)buffers;
    recovery->state = PCI_DEVICE_RECOVERY_DMA_SAFE;
    return true;
}

bool pci_device_recovery_complete_without_reset(PciDeviceRecovery *recovery) {
    if (!recovery) return false;
    u64 flags = recovery_irq_save();
    bool completed = false;
    if (!recovery_valid_locked(recovery) ||
        recovery->state != PCI_DEVICE_RECOVERY_QUIESCED ||
        recovery->bus_master_was_enabled || recovery->bus_master_exposed) goto done;

    const PciDevice *device = pci_assignment_enumerated_device(recovery->assignment);
    if (!device || pci_config_read16(device->address, PCI_COMMAND_OFFSET) !=
            recovery->command_quiesced) goto done;

    if (!complete_dma_recovery_locked(recovery)) goto done;
    completed = true;

done:
    recovery_irq_restore(flags);
    return completed;
}

bool pci_device_recovery_complete_flr(PciDeviceRecovery *recovery) {
    if (!recovery) return false;

    const PciDevice *device = 0;
    u8 capability = 0U;
    u64 flags = recovery_irq_save();
    bool required = recovery_valid_locked(recovery) &&
        recovery->state == PCI_DEVICE_RECOVERY_QUIESCED &&
        (recovery->bus_master_was_enabled || recovery->bus_master_exposed);
    if (required) device = pci_assignment_enumerated_device(recovery->assignment);
    recovery_irq_restore(flags);
    if (!required || !device || !pci_flr_supported(device, &capability)) return false;

    u16 command_after = 0U;
    PciFlrResult result = pci_flr_execute_quiesced(device, &command_after);

    flags = recovery_irq_save();
    if (!recovery_valid_locked(recovery) ||
        recovery->state != PCI_DEVICE_RECOVERY_QUIESCED) {
        recovery_irq_restore(flags);
        return false;
    }
    if (result == PCI_FLR_RESULT_NOT_ATTEMPTED) {
        recovery_irq_restore(flags);
        return false;
    }

    recovery->reset_attempted = true;
    recovery->reset_method = PCI_DEVICE_RESET_FLR;
    recovery->flr_capability_offset = capability;
    if (result != PCI_FLR_RESULT_COMPLETE || !command_after) {
        recovery->state = PCI_DEVICE_RECOVERY_FAILED_QUARANTINED;
        recovery_irq_restore(flags);
        return false;
    }

    recovery->command_quiesced = command_after;
    recovery->reset_completed = true;
    if (!complete_dma_recovery_locked(recovery)) {
        recovery->state = PCI_DEVICE_RECOVERY_FAILED_QUARANTINED;
        recovery_irq_restore(flags);
        return false;
    }

    recovery_irq_restore(flags);
    return true;
}

bool pci_device_recovery_complete_assignment(PciDeviceRecovery *recovery) {
    if (!recovery) return false;
    u64 flags = recovery_irq_save();
    if (!recovery_valid_locked(recovery) ||
        recovery->state != PCI_DEVICE_RECOVERY_DMA_SAFE ||
        !pci_assignment_complete_quarantine(recovery->assignment,
            recovery->owner_process_id)) {
        recovery_irq_restore(flags);
        return false;
    }

    u32 slot = object_storage_find(g_recoveries,
        PCI_DEVICE_RECOVERY_STORAGE_CAPACITY, recovery);
    if (slot >= PCI_DEVICE_RECOVERY_STORAGE_CAPACITY) {
        recovery_irq_restore(flags);
        return false;
    }
    g_recoveries[slot] = 0;
    k_memset(recovery, 0, sizeof(*recovery));
    recovery_irq_restore(flags);
    return true;
}
