#ifndef JA_OS_PCI_DMA_SUBMISSION_H
#define JA_OS_PCI_DMA_SUBMISSION_H

#include "capability.h"
#include "pci_assignment.h"
#include "pci_dma_buffer.h"
#include "process.h"
#include "types.h"

#define PCI_DMA_SUBMISSION_STORAGE_CAPACITY 32U

typedef enum {
    PCI_DMA_DIRECTION_INVALID = 0,
    PCI_DMA_TO_DEVICE,
    PCI_DMA_FROM_DEVICE,
    PCI_DMA_BIDIRECTIONAL
} PciDmaDirection;

typedef enum {
    PCI_DMA_SUBMISSION_INVALID = 0,
    PCI_DMA_SUBMISSION_ACTIVE,
    PCI_DMA_SUBMISSION_QUARANTINED
} PciDmaSubmissionState;

typedef struct {
    PciAssignment *assignment;
    PciDmaBuffer *buffer;
    Process *process;
    u64 assignment_resource_id;
    u64 buffer_resource_id;
    u64 owner_process_id;
    u64 physical_base;
    u64 size;
    PciDmaDirection direction;
    PciDmaSubmissionState state;
    bool assignment_gate_held;
    bool initialized;
} PciDmaSubmission;

/* Prepare one bounded kernel-mediated DMA window. The buffer must be unmapped
 * from Ring3 and no BAR access for this assignment may be active. Preparation
 * holds an assignment-wide gate that blocks new user MMIO and PIO access. It
 * does not enable PCI bus mastering; a device-specific backend may do that only
 * after reset/quiescence rules are established. */
bool pci_dma_submission_begin(PciDmaSubmission *submission,
    PciAssignment *assignment, Process *process, CapabilityHandle assignment_handle,
    PciDmaBuffer *buffer, CapabilityHandle buffer_handle, u64 offset, u64 size,
    PciDmaDirection direction);

/* Normal completion is called only after the device-specific mediator has
 * proven that the device no longer retains the DMA address. */
bool pci_dma_submission_complete(PciDmaSubmission *submission,
    Process *process, CapabilityHandle buffer_handle);

/* Crash/timeout containment retains the DMA buffer, assignment access pin and
 * assignment-wide exclusion gate. reset_complete() is a supervisor path used
 * only after device-specific recovery independently proves DMA impossible. */
bool pci_dma_submission_force_quarantine(PciDmaSubmission *submission);
bool pci_dma_submission_reset_complete(PciDmaSubmission *submission);
bool pci_dma_submission_force_quarantine_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);
bool pci_dma_submission_recovery_complete_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count);
u32 pci_dma_submission_assignment_count(const PciAssignment *assignment);

bool pci_dma_submission_valid(const PciDmaSubmission *submission);
u32 pci_dma_submission_object_count(void);
PciDmaSubmissionState pci_dma_submission_state(const PciDmaSubmission *submission);
PciDmaDirection pci_dma_submission_direction(const PciDmaSubmission *submission);

/* Kernel-only physical span for the device-specific mediator. No userspace ABI
 * exposes this address. */
bool pci_dma_submission_device_span(const PciDmaSubmission *submission,
    u64 *out_physical_base, u64 *out_size);

#endif
