#ifndef JA_OS_PCI_IRQ_RESOURCE_H
#define JA_OS_PCI_IRQ_RESOURCE_H

#include "capability.h"
#include "device_resource.h"
#include "endpoint.h"
#include "pci_assignment.h"
#include "process.h"

#define PCI_IRQ_RESOURCE_STORAGE_CAPACITY 32U
#define PCI_IRQ_STORM_THRESHOLD 8U
#define PCI_IRQ_RESOURCE_OWNER_RIGHTS CAPABILITY_RIGHT_MANAGE

typedef enum {
    PCI_IRQ_STATE_INVALID = 0,
    PCI_IRQ_STATE_DISARMED,
    PCI_IRQ_STATE_ARMED,
    PCI_IRQ_STATE_PENDING,
    PCI_IRQ_STATE_STORMED
} PciIrqState;

typedef struct {
    DeviceResource resource;
    PciAssignment *assignment;
    u64 assignment_resource_id;
    u64 owner_process_id;

    Endpoint event_endpoint;
    CapabilityHandle kernel_send_handle;

    u64 next_sequence;
    u64 pending_sequence;
    u64 delivered_count;
    u64 acknowledged_count;
    u64 suppressed_count;
    u64 recovery_count;
    u32 suppressed_since_ack;

    PciIrqState state;
    bool assignment_access_held;
    bool delivery_masked;
    bool initialized;
} PciIrqResource;

/* This layer owns IRQ authority, delivery backpressure and teardown ordering.
 * A later controller backend maps hardware vectors/sources into signal(). */
bool pci_irq_resource_create(PciIrqResource *irq, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle,
    CapabilityHandle *out_resource_handle, CapabilityHandle *out_event_handle);

/* Destroy only after the owner-side resource/event capabilities are revoked and
 * the resource is disarmed. The kernel SEND capability is revoked internally. */
bool pci_irq_resource_destroy(PciIrqResource *irq);
bool pci_irq_resource_valid(const PciIrqResource *irq);
u32 pci_irq_resource_object_count(void);

PciIrqState pci_irq_resource_state(const PciIrqResource *irq);
u64 pci_irq_resource_owner_process_id(const PciIrqResource *irq);
u64 pci_irq_resource_pending_sequence(const PciIrqResource *irq);
u64 pci_irq_resource_delivered_count(const PciIrqResource *irq);
u64 pci_irq_resource_acknowledged_count(const PciIrqResource *irq);
u64 pci_irq_resource_suppressed_count(const PciIrqResource *irq);
u64 pci_irq_resource_recovery_count(const PciIrqResource *irq);
bool pci_irq_resource_delivery_masked(const PciIrqResource *irq);

/* Arming pins the parent assignment as an active device-access path. Quiesce or
 * quarantine therefore cannot begin while interrupt delivery remains armed. */
bool pci_irq_resource_arm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);
bool pci_irq_resource_disarm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);

/* Supervisor cleanup does not depend on a still-live userspace capability. */
bool pci_irq_resource_force_disarm(PciIrqResource *irq, u64 owner_process_id);
bool pci_irq_resource_force_disarm_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);

/* Called by the hardware routing backend after it has stopped further source
 * delivery. One event is outstanding until the driver consumes it and
 * acknowledges the exact sequence. Repeated signals are bounded as a storm. */
bool pci_irq_resource_signal(PciIrqResource *irq);
bool pci_irq_resource_acknowledge(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle, u64 sequence);
bool pci_irq_resource_recover_storm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle);

#endif
