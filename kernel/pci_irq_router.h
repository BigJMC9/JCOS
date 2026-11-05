#ifndef JA_OS_PCI_IRQ_ROUTER_H
#define JA_OS_PCI_IRQ_ROUTER_H

#include "pci_irq_resource.h"

#define PCI_IRQ_ROUTER_VECTOR_FIRST 0x22U
#define PCI_IRQ_ROUTER_VECTOR_LAST  0x2FU
#define PCI_IRQ_ROUTER_BINDING_CAPACITY \
    (PCI_IRQ_ROUTER_VECTOR_LAST - PCI_IRQ_ROUTER_VECTOR_FIRST + 1U)

/* Initial hardware backend: firmware-programmed legacy PCI INTx line routed
 * through the IOAPIC. MSI/MSI-X and ACPI _PRT discovery remain separate work. */
bool pci_irq_router_bind_legacy_intx(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);
bool pci_irq_router_unbind(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);

/* Supervisor teardown does not depend on retained userspace capabilities. */
bool pci_irq_router_force_disarm(PciIrqResource *irq, u64 owner_process_id);
bool pci_irq_router_force_unbind(PciIrqResource *irq, u64 owner_process_id);

/* Recovery sweep: mask/disarm every routed source owned by one assignment, then
 * optionally release those already-masked leases. These are supervisor-only
 * lifetime operations and do not depend on userspace capability survival. */
bool pci_irq_router_force_quiesce_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);
bool pci_irq_router_force_unbind_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);

/* Hardware-bound resources must use these wrappers so IOAPIC masking follows
 * the resource delivery state. */
bool pci_irq_router_arm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);
bool pci_irq_router_disarm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);
bool pci_irq_router_acknowledge(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle, u64 sequence);
bool pci_irq_router_recover_storm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);

/* Interrupt-dispatch hook. The source is masked before event publication. */
bool pci_irq_router_handle_vector(u8 vector);

bool pci_irq_router_bound(const PciIrqResource *irq);
u8 pci_irq_router_vector(const PciIrqResource *irq);
u32 pci_irq_router_gsi(const PciIrqResource *irq);
bool pci_irq_router_source_masked(const PciIrqResource *irq);
u32 pci_irq_router_binding_count(void);

#endif
