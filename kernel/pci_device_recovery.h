#ifndef JA_OS_PCI_DEVICE_RECOVERY_H
#define JA_OS_PCI_DEVICE_RECOVERY_H

#include "pci_assignment.h"
#include "process.h"
#include "types.h"

#define PCI_DEVICE_RECOVERY_STORAGE_CAPACITY 16U

typedef enum {
    PCI_DEVICE_RECOVERY_INVALID = 0,
    PCI_DEVICE_RECOVERY_QUIESCED,
    PCI_DEVICE_RECOVERY_DMA_SAFE,
    PCI_DEVICE_RECOVERY_FAILED_QUARANTINED
} PciDeviceRecoveryState;

typedef enum {
    PCI_DEVICE_RESET_NONE = 0,
    PCI_DEVICE_RESET_FLR
} PciDeviceResetMethod;

typedef struct {
    PciAssignment *assignment;
    u64 assignment_resource_id;
    u64 owner_process_id;
    u16 command_before;
    u16 command_quiesced;
    u32 irq_bindings_quiesced;
    u32 irq_resources_disarmed;
    u32 mmio_mappings_unmapped;
    u32 dma_mappings_unmapped;
    u32 dma_submissions_quarantined;
    u32 dma_buffers_quarantined;
    bool bus_master_was_enabled;
    bool bus_master_exposed;
    bool reset_attempted;
    bool reset_completed;
    u8 flr_capability_offset;
    PciDeviceResetMethod reset_method;
    PciDeviceRecoveryState state;
    bool initialized;
} PciDeviceRecovery;

/* Recovery first quarantines authority, masks/disarms IRQ delivery, disables
 * PCI decode/bus mastering, removes retained CPU mappings, and quarantines any
 * DMA lifetime. A failed step leaves the assignment quarantined and must never
 * be reclassified as safe by capability loss alone. */
bool pci_device_recovery_begin(PciDeviceRecovery *recovery,
    PciAssignment *assignment, Process *owner_process);

/* If bus mastering was already disabled when recovery began, the generic
 * mediator has a hardware proof that its logical DMA submissions were never
 * issued by this function. That proof can release DMA quarantine without a
 * device reset. If bus mastering had been enabled, this path fails closed and
 * a reset backend must provide the proof instead. */
bool pci_device_recovery_complete_without_reset(PciDeviceRecovery *recovery);

/* Execute PCIe Function Level Reset only for an already-quiesced recovery
 * object whose assignment requires a hardware reset. Unsupported or unsafe
 * functions are refused before reset initiation. Post-initiation failure keeps
 * the assignment quarantined. */
bool pci_device_recovery_complete_flr(PciDeviceRecovery *recovery);

/* Final assignment release is retryable and succeeds only after all child
 * resources/capabilities are gone and recovery has established DMA safety. */
bool pci_device_recovery_complete_assignment(PciDeviceRecovery *recovery);

bool pci_device_recovery_valid(const PciDeviceRecovery *recovery);
u32 pci_device_recovery_object_count(void);
PciDeviceRecoveryState pci_device_recovery_state(const PciDeviceRecovery *recovery);
bool pci_device_recovery_reset_required(const PciDeviceRecovery *recovery);
bool pci_device_recovery_flr_available(const PciDeviceRecovery *recovery);
PciDeviceResetMethod pci_device_recovery_reset_method(const PciDeviceRecovery *recovery);

#endif
