#ifndef JA_OS_PCI_DMA_BUFFER_H
#define JA_OS_PCI_DMA_BUFFER_H

#include "capability.h"
#include "device_resource.h"
#include "pci_assignment.h"
#include "process.h"
#include "types.h"

#define PCI_DMA_BUFFER_STORAGE_CAPACITY 64U
#define PCI_DMA_BUFFER_MAX_PAGES 64U

typedef enum {
    PCI_DMA_BUFFER_INVALID = 0,
    PCI_DMA_BUFFER_READY,
    PCI_DMA_BUFFER_DEVICE_ACTIVE,
    PCI_DMA_BUFFER_QUARANTINED
} PciDmaBufferState;

typedef struct {
    DeviceResource resource;
    PciAssignment *assignment;
    Process *process;
    u64 assignment_resource_id;
    u64 owner_process_id;
    u64 physical_base;
    u64 virtual_base;
    u32 page_count;
    u32 mapped_page_count;
    PciDmaBufferState state;
    bool writable_mapping;
    bool assignment_access_held;
    bool initialized;
} PciDmaBuffer;

/* DMA storage is contiguous, PMM-owned RAM retained by this object. The owner
 * receives non-transferable READ/WRITE/MANAGE authority; no bus-master enable
 * or physical address is exposed through a userspace ABI. */
bool pci_dma_buffer_create(PciDmaBuffer *buffer, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle, u32 page_count,
    CapabilityHandle *out_handle);

/* Destruction is permitted only after CPU mappings are removed, device access
 * is complete, quarantine has been cleared after reset, and all capabilities
 * to this resource have been revoked. */
bool pci_dma_buffer_destroy(PciDmaBuffer *buffer);
bool pci_dma_buffer_valid(const PciDmaBuffer *buffer);
u32 pci_dma_buffer_object_count(void);

PciDmaBufferState pci_dma_buffer_state(const PciDmaBuffer *buffer);
u64 pci_dma_buffer_owner_process_id(const PciDmaBuffer *buffer);
u64 pci_dma_buffer_size(const PciDmaBuffer *buffer);
PciAssignment *pci_dma_buffer_assignment(PciDmaBuffer *buffer);
bool pci_dma_buffer_cpu_mapping_active(const PciDmaBuffer *buffer);
bool pci_dma_buffer_device_span(const PciDmaBuffer *buffer,
    u64 *out_physical_base, u64 *out_size);

/* CPU access is mapped only into the owning process, user-accessible and NX.
 * Supervisor cleanup can remove the mapping after owner authority is revoked. */
bool pci_dma_buffer_map(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle, u64 virtual_base, bool writable);
bool pci_dma_buffer_unmap(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle);
bool pci_dma_buffer_force_unmap(PciDmaBuffer *buffer, u64 owner_process_id);
bool pci_dma_buffer_force_unmap_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);

/* Kernel mediation brackets any interval in which the device may retain the
 * physical address. The assignment active-access pin prevents ordinary
 * quiesce/release while DMA might still be in flight. */
bool pci_dma_buffer_device_begin(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle);
bool pci_dma_buffer_device_complete(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle);

/* Driver death or timeout is not proof that DMA stopped. Force quarantine
 * retains frames and the assignment access pin. reset_complete() is legal only
 * after platform/device recovery has independently proven DMA is impossible. */
bool pci_dma_buffer_force_quarantine(PciDmaBuffer *buffer, u64 owner_process_id);
bool pci_dma_buffer_reset_complete(PciDmaBuffer *buffer, u64 owner_process_id);
bool pci_dma_buffer_force_quarantine_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);
bool pci_dma_buffer_recovery_complete_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);

#endif
