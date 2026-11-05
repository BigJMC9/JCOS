#ifndef JA_OS_PCI_ASSIGNMENT_H
#define JA_OS_PCI_ASSIGNMENT_H

#include "device_resource.h"
#include "pci.h"
#include "process.h"

#define PCI_ASSIGNMENT_CHILD_RESOURCE_LIMIT 32U
#define PCI_ASSIGNMENT_ACTIVE_ACCESS_LIMIT 64U

typedef enum {
    PCI_ASSIGNMENT_INVALID = 0,
    PCI_ASSIGNMENT_AVAILABLE,
    PCI_ASSIGNMENT_ASSIGNED,
    PCI_ASSIGNMENT_QUIESCING,
    PCI_ASSIGNMENT_QUARANTINED
} PciAssignmentState;

typedef struct {
    DeviceResource resource;
    PciDevice device;
    u64 owner_process_id;
    u32 child_resource_count;
    u32 active_access_count;
    PciAssignmentState state;
    bool dma_gate_active;
    bool bus_master_exposed;
    bool initialized;
} PciAssignment;

/*
 * Assignment is an authority and lifetime contract only. Creation and claim do
 * not enable PCI decoding, bus mastering, BAR mappings, interrupt delivery or
 * DMA. Those resources are introduced by later, separately gated layers.
 */
bool pci_assignment_create(PciAssignment *assignment, const PciDevice *device);
bool pci_assignment_destroy(PciAssignment *assignment);
bool pci_assignment_valid(const PciAssignment *assignment);
u32 pci_assignment_object_count(void);

PciAssignmentState pci_assignment_state(const PciAssignment *assignment);
u64 pci_assignment_owner_process_id(const PciAssignment *assignment);
const PciDevice *pci_assignment_device(const PciAssignment *assignment);
const PciDevice *pci_assignment_enumerated_device(const PciAssignment *assignment);
DeviceResource *pci_assignment_resource(PciAssignment *assignment);
u32 pci_assignment_child_count(const PciAssignment *assignment);
u32 pci_assignment_active_access_count(const PciAssignment *assignment);
bool pci_assignment_dma_gate_active(const PciAssignment *assignment);
bool pci_assignment_bus_master_exposed(const PciAssignment *assignment);
PciAssignment *pci_assignment_from_resource(DeviceResource *resource);

/* Claim is atomic with issuance of one non-transferable MANAGE capability to
 * the owner. A stale capability therefore prevents a new incarnation from
 * claiming the same function until explicit revocation and release complete. */
bool pci_assignment_claim(PciAssignment *assignment, Process *process,
    CapabilityHandle *out_handle);

/* Authorization requires both current ownership and the exact live capability
 * issued to that process. QUIESCING and QUARANTINED assignments are unusable. */
bool pci_assignment_authorized(PciAssignment *assignment, Process *process,
    CapabilityHandle handle);

/* Subordinate BAR/IRQ/DMA objects pin the assignment independently of its
 * owner capability. New children require a live ASSIGNED owner authority;
 * release is allowed while quiescing or quarantined so teardown can finish. */
bool pci_assignment_child_acquire(PciAssignment *assignment, Process *process,
    CapabilityHandle assignment_handle);
bool pci_assignment_child_release(PciAssignment *assignment, u64 owner_process_id);

/* Long-lived CPU mappings and other active access paths pin the ASSIGNED
 * state. Quiesce/quarantine is refused until every such path is removed. */
bool pci_assignment_access_acquire(PciAssignment *assignment, u64 owner_process_id);
bool pci_assignment_access_release(PciAssignment *assignment, u64 owner_process_id);

/* A kernel-mediated DMA submission owns one assignment-wide exclusion gate.
 * While held, user BAR mappings and mediated PIO are refused. The gate may
 * survive assignment quarantine and is released only after completion or an
 * independently proven reset. */
bool pci_assignment_dma_gate_acquire(PciAssignment *assignment, u64 owner_process_id);
bool pci_assignment_dma_gate_release(PciAssignment *assignment, u64 owner_process_id);
/* Sticky per-incarnation taint set immediately before a qualified backend
 * enables PCI bus mastering. Ordinary release is forbidden after this point;
 * only quarantine recovery may clear the taint. */
bool pci_assignment_mark_bus_master_exposed(PciAssignment *assignment,
    u64 owner_process_id);

/* Callers must stop new submissions before entering QUIESCING. Completion is
 * refused while any capability still pins the assignment resource. */
bool pci_assignment_begin_quiesce(PciAssignment *assignment, u64 owner_process_id);
bool pci_assignment_complete_release(PciAssignment *assignment, u64 owner_process_id);

/* Quarantine preserves ownership and identity when safe quiescence cannot be
 * established. reset_complete() is reserved for a later device-specific reset
 * path after interrupts and DMA are proven stopped and authority is revoked. */
bool pci_assignment_quarantine(PciAssignment *assignment, u64 owner_process_id);

/* Supervisor recovery may quarantine an assignment even while active-access
 * pins remain. Those pins deliberately survive quarantine and must be released
 * only after the corresponding device access has been proven stopped. */
bool pci_assignment_force_quarantine(PciAssignment *assignment, u64 owner_process_id);

/* Complete quarantine only after an independent recovery proof establishes
 * that the device can no longer access host memory. reset_complete() remains a
 * compatibility wrapper for reset-backed callers. */
bool pci_assignment_complete_quarantine(PciAssignment *assignment, u64 owner_process_id);
bool pci_assignment_reset_complete(PciAssignment *assignment, u64 owner_process_id);

#endif
