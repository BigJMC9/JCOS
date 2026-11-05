#ifndef JA_OS_PCI_BAR_RESOURCE_H
#define JA_OS_PCI_BAR_RESOURCE_H

#include "capability.h"
#include "device_resource.h"
#include "pci.h"
#include "pci_assignment.h"
#include "process.h"

#define PCI_BAR_RESOURCE_OWNER_RIGHTS \
    (CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE | CAPABILITY_RIGHT_MANAGE)

typedef struct {
    DeviceResource resource;
    PciAssignment *assignment;
    u64 assignment_resource_id;
    u64 owner_process_id;
    PciBar bar;
    u32 active_access_count;
    bool bounded;
    bool initialized;
} PciBarResource;

/* A BAR resource derives only from a live assigned PCI function. Creation does
 * not map MMIO, enable decode, grant I/O privilege, or enable bus mastering. */
bool pci_bar_resource_create(PciBarResource *resource, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle, u8 bar_index,
    CapabilityHandle *out_handle);

/* Destroy after revoking every child capability. The parent assignment remains
 * pinned until this object releases its subordinate-resource reference. */
bool pci_bar_resource_destroy(PciBarResource *resource);
bool pci_bar_resource_valid(const PciBarResource *resource);
u32 pci_bar_resource_object_count(void);

bool pci_bar_resource_descriptor(const PciBarResource *resource, PciBar *out);
bool pci_bar_resource_bounded(const PciBarResource *resource);
u64 pci_bar_resource_owner_process_id(const PciBarResource *resource);
u32 pci_bar_resource_active_access_count(const PciBarResource *resource);
u32 pci_bar_resource_assignment_active_access_count(const PciAssignment *assignment);

/* Authorization is disabled as soon as the parent assignment leaves ASSIGNED.
 * required_rights may contain only READ, WRITE, and MANAGE. */
bool pci_bar_resource_authorized(PciBarResource *resource, Process *process,
    CapabilityHandle handle, CapabilityRights required_rights);

/* Active CPU access pins both this BAR and its parent assignment. Acquire is
 * capability-gated; release uses retained owner identity so crash cleanup does
 * not depend on a still-live userspace capability. */
bool pci_bar_resource_access_acquire(PciBarResource *resource, Process *process,
    CapabilityHandle handle, CapabilityRights required_rights);
bool pci_bar_resource_access_release(PciBarResource *resource, u64 owner_process_id);

/* Byte-range validation only. No CPU mapping or port access is performed here. */
bool pci_bar_resource_contains(const PciBarResource *resource, u64 offset, u64 width);

#endif
