#ifndef JA_OS_PCI_BAR_ACCESS_H
#define JA_OS_PCI_BAR_ACCESS_H

#include "capability.h"
#include "pci_bar_resource.h"
#include "process.h"
#include "types.h"

#define PCI_BAR_MMIO_MAPPING_STORAGE_CAPACITY 64U

typedef struct {
    PciBarResource *resource;
    Process *process;
    u64 resource_id;
    u64 process_id;
    u64 virtual_base;
    u64 physical_base;
    u64 size;
    u32 page_count;
    u32 mapped_page_count;
    bool writable;
    bool initialized;
} PciBarMmioMapping;

/* Direct MMIO mappings are page-exact, user-accessible, NX and uncached. No
 * containing page outside the BAR may be exposed to the driver. */
bool pci_bar_mmio_map(PciBarMmioMapping *mapping, PciBarResource *resource,
    Process *process, CapabilityHandle handle, u64 bar_offset, u64 size,
    u64 virtual_base, bool writable);

/* Cleanup uses retained object/process identity rather than a live capability,
 * so a crashed driver can be unmapped after its authority has been revoked. */
bool pci_bar_mmio_unmap(PciBarMmioMapping *mapping);
bool pci_bar_mmio_mapping_valid(const PciBarMmioMapping *mapping);
u32 pci_bar_mmio_mapping_object_count(void);
bool pci_bar_mmio_force_unmap_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);

/* Validate the page-exact subset accepted by direct mapping without modifying
 * an address space. */
bool pci_bar_mmio_range_mappable(const PciBarResource *resource,
    u64 bar_offset, u64 size);

/* Port I/O remains kernel-mediated. Ring3 receives no IOPL or I/O bitmap grant.
 * Width is one, two, or four bytes and the access must remain wholly in BAR. */
bool pci_bar_pio_range_valid(const PciBarResource *resource, u64 offset, u32 width);
bool pci_bar_pio_read(PciBarResource *resource, Process *process,
    CapabilityHandle handle, u64 offset, u32 width, u32 *out_value);
bool pci_bar_pio_write(PciBarResource *resource, Process *process,
    CapabilityHandle handle, u64 offset, u32 width, u32 value);

#endif
