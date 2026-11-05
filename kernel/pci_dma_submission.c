#include "pci_dma_submission.h"

#include "arch.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "pci_bar_resource.h"

#define PCI_DMA_SUBMISSION_RFLAGS_IF (1ULL << 9)

static void *g_submissions[PCI_DMA_SUBMISSION_STORAGE_CAPACITY];

static u64 submission_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void submission_irq_restore(u64 flags) {
    if (flags & PCI_DMA_SUBMISSION_RFLAGS_IF) interrupts_enable();
}

static bool direction_valid(PciDmaDirection direction) {
    return direction == PCI_DMA_TO_DEVICE || direction == PCI_DMA_FROM_DEVICE ||
        direction == PCI_DMA_BIDIRECTIONAL;
}

static bool submission_live(const PciDmaSubmission *submission) {
    return object_storage_find(g_submissions, PCI_DMA_SUBMISSION_STORAGE_CAPACITY,
        submission) < PCI_DMA_SUBMISSION_STORAGE_CAPACITY;
}

static bool submission_valid_locked(const PciDmaSubmission *submission) {
    if (!submission_live(submission) || !submission->initialized ||
        !submission->assignment || !submission->buffer || !submission->process ||
        !submission->owner_process_id || !submission->physical_base || !submission->size ||
        !direction_valid(submission->direction) || !submission->assignment_gate_held ||
        (submission->state != PCI_DMA_SUBMISSION_ACTIVE &&
         submission->state != PCI_DMA_SUBMISSION_QUARANTINED)) return false;

    if (!pci_assignment_valid(submission->assignment) ||
        !pci_dma_buffer_valid(submission->buffer) ||
        pci_dma_buffer_assignment(submission->buffer) != submission->assignment ||
        pci_dma_buffer_owner_process_id(submission->buffer) != submission->owner_process_id ||
        submission->process->id != submission->owner_process_id ||
        !process_storage_in_use(submission->process) || !submission->process->initialized ||
        submission->process->kernel ||
        !pci_assignment_dma_gate_active(submission->assignment)) return false;

    DeviceResource *assignment_resource = pci_assignment_resource(submission->assignment);
    if (!assignment_resource || assignment_resource->id != submission->assignment_resource_id ||
        submission->buffer->resource.id != submission->buffer_resource_id) return false;

    u64 buffer_base = 0ULL;
    u64 buffer_size = 0ULL;
    if (!pci_dma_buffer_device_span(submission->buffer, &buffer_base, &buffer_size) ||
        submission->physical_base < buffer_base ||
        submission->physical_base - buffer_base > buffer_size ||
        submission->size > buffer_size - (submission->physical_base - buffer_base)) return false;

    if (submission->state == PCI_DMA_SUBMISSION_ACTIVE) {
        PciAssignmentState assignment_state = pci_assignment_state(submission->assignment);
        return (assignment_state == PCI_ASSIGNMENT_ASSIGNED ||
                assignment_state == PCI_ASSIGNMENT_QUARANTINED) &&
            pci_dma_buffer_state(submission->buffer) == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
            !pci_dma_buffer_cpu_mapping_active(submission->buffer) &&
            pci_bar_resource_assignment_active_access_count(submission->assignment) == 0U;
    }

    return pci_assignment_state(submission->assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_dma_buffer_state(submission->buffer) == PCI_DMA_BUFFER_QUARANTINED;
}

bool pci_dma_submission_valid(const PciDmaSubmission *submission) {
    u64 flags = submission_irq_save();
    bool valid = submission && submission_valid_locked(submission);
    submission_irq_restore(flags);
    return valid;
}

u32 pci_dma_submission_object_count(void) {
    u64 flags = submission_irq_save();
    u32 count = object_storage_count(g_submissions, PCI_DMA_SUBMISSION_STORAGE_CAPACITY);
    submission_irq_restore(flags);
    return count;
}

PciDmaSubmissionState pci_dma_submission_state(const PciDmaSubmission *submission) {
    u64 flags = submission_irq_save();
    PciDmaSubmissionState state = submission && submission_valid_locked(submission) ?
        submission->state : PCI_DMA_SUBMISSION_INVALID;
    submission_irq_restore(flags);
    return state;
}

PciDmaDirection pci_dma_submission_direction(const PciDmaSubmission *submission) {
    u64 flags = submission_irq_save();
    PciDmaDirection direction = submission && submission_valid_locked(submission) ?
        submission->direction : PCI_DMA_DIRECTION_INVALID;
    submission_irq_restore(flags);
    return direction;
}

bool pci_dma_submission_begin(PciDmaSubmission *submission,
    PciAssignment *assignment, Process *process, CapabilityHandle assignment_handle,
    PciDmaBuffer *buffer, CapabilityHandle buffer_handle, u64 offset, u64 size,
    PciDmaDirection direction) {
    if (!submission || !assignment || !process || !assignment_handle || !buffer ||
        !buffer_handle || !size || !direction_valid(direction)) return false;

    u64 flags = submission_irq_save();
    bool gate_held = false;
    bool buffer_active = false;
    bool begun = false;

    if (submission_live(submission) || !pci_assignment_authorized(assignment, process,
            assignment_handle) || pci_dma_buffer_assignment(buffer) != assignment ||
        pci_dma_buffer_owner_process_id(buffer) != process->id ||
        pci_dma_buffer_state(buffer) != PCI_DMA_BUFFER_READY ||
        pci_dma_buffer_cpu_mapping_active(buffer) ||
        pci_bar_resource_assignment_active_access_count(assignment) != 0U)
        goto done;

    u64 buffer_base = 0ULL;
    u64 buffer_size = 0ULL;
    if (!pci_dma_buffer_device_span(buffer, &buffer_base, &buffer_size) ||
        offset >= buffer_size || size > buffer_size - offset ||
        buffer_base > ~0ULL - offset) goto done;

    u32 slot = object_storage_empty(g_submissions, PCI_DMA_SUBMISSION_STORAGE_CAPACITY);
    if (slot >= PCI_DMA_SUBMISSION_STORAGE_CAPACITY) goto done;

    if (!pci_assignment_dma_gate_acquire(assignment, process->id)) goto done;
    gate_held = true;

    if (!pci_dma_buffer_device_begin(buffer, process, buffer_handle)) goto rollback;
    buffer_active = true;

    if (pci_dma_buffer_cpu_mapping_active(buffer) ||
        pci_bar_resource_assignment_active_access_count(assignment) != 0U) goto rollback;

    DeviceResource *assignment_resource = pci_assignment_resource(assignment);
    if (!assignment_resource || !assignment_resource->id || !buffer->resource.id) goto rollback;

    k_memset(submission, 0, sizeof(*submission));
    submission->assignment = assignment;
    submission->buffer = buffer;
    submission->process = process;
    submission->assignment_resource_id = assignment_resource->id;
    submission->buffer_resource_id = buffer->resource.id;
    submission->owner_process_id = process->id;
    submission->physical_base = buffer_base + offset;
    submission->size = size;
    submission->direction = direction;
    submission->state = PCI_DMA_SUBMISSION_ACTIVE;
    submission->assignment_gate_held = true;
    submission->initialized = true;
    g_submissions[slot] = submission;
    begun = true;
    goto done;

rollback:
    if (buffer_active && !pci_dma_buffer_device_complete(buffer, process, buffer_handle))
        cpu_halt_forever();
    if (gate_held && !pci_assignment_dma_gate_release(assignment, process->id))
        cpu_halt_forever();

done:
    submission_irq_restore(flags);
    return begun;
}

static void submission_clear_locked(PciDmaSubmission *submission) {
    u32 slot = object_storage_find(g_submissions, PCI_DMA_SUBMISSION_STORAGE_CAPACITY,
        submission);
    if (slot >= PCI_DMA_SUBMISSION_STORAGE_CAPACITY) cpu_halt_forever();
    g_submissions[slot] = 0;
    k_memset(submission, 0, sizeof(*submission));
}

bool pci_dma_submission_complete(PciDmaSubmission *submission,
    Process *process, CapabilityHandle buffer_handle) {
    if (!submission || !process || !buffer_handle) return false;
    u64 flags = submission_irq_save();
    if (!submission_valid_locked(submission) ||
        submission->state != PCI_DMA_SUBMISSION_ACTIVE ||
        process != submission->process || process->id != submission->owner_process_id ||
        !pci_dma_buffer_device_complete(submission->buffer, process, buffer_handle)) {
        submission_irq_restore(flags);
        return false;
    }

    PciAssignment *assignment = submission->assignment;
    u64 owner = submission->owner_process_id;
    if (!pci_assignment_dma_gate_release(assignment, owner)) cpu_halt_forever();
    submission_clear_locked(submission);
    submission_irq_restore(flags);
    return true;
}

bool pci_dma_submission_force_quarantine(PciDmaSubmission *submission) {
    if (!submission) return false;
    u64 flags = submission_irq_save();
    bool quarantined = false;
    if (!submission_valid_locked(submission)) goto done;
    if (submission->state == PCI_DMA_SUBMISSION_QUARANTINED) {
        quarantined = true;
        goto done;
    }
    if (submission->state != PCI_DMA_SUBMISSION_ACTIVE ||
        !pci_dma_buffer_force_quarantine(submission->buffer,
            submission->owner_process_id)) goto done;
    submission->state = PCI_DMA_SUBMISSION_QUARANTINED;
    quarantined = submission_valid_locked(submission);

done:
    submission_irq_restore(flags);
    return quarantined;
}

bool pci_dma_submission_reset_complete(PciDmaSubmission *submission) {
    if (!submission) return false;
    u64 flags = submission_irq_save();
    if (!submission_valid_locked(submission) ||
        submission->state != PCI_DMA_SUBMISSION_QUARANTINED ||
        !pci_dma_buffer_reset_complete(submission->buffer, submission->owner_process_id)) {
        submission_irq_restore(flags);
        return false;
    }

    PciAssignment *assignment = submission->assignment;
    u64 owner = submission->owner_process_id;
    if (!pci_assignment_dma_gate_release(assignment, owner)) cpu_halt_forever();
    submission_clear_locked(submission);
    submission_irq_restore(flags);
    return true;
}

bool pci_dma_submission_device_span(const PciDmaSubmission *submission,
    u64 *out_physical_base, u64 *out_size) {
    if (!out_physical_base || !out_size) return false;
    *out_physical_base = 0ULL;
    *out_size = 0ULL;
    u64 flags = submission_irq_save();
    bool valid = submission && submission_valid_locked(submission);
    if (valid) {
        *out_physical_base = submission->physical_base;
        *out_size = submission->size;
    }
    submission_irq_restore(flags);
    return valid;
}

u32 pci_dma_submission_assignment_count(const PciAssignment *assignment) {
    if (!assignment) return 0U;
    u64 flags = submission_irq_save();
    u32 count = 0U;
    for (u32 i = 0U; i < PCI_DMA_SUBMISSION_STORAGE_CAPACITY; ++i) {
        PciDmaSubmission *submission = (PciDmaSubmission *)g_submissions[i];
        if (submission && submission_valid_locked(submission) &&
            submission->assignment == assignment) ++count;
    }
    submission_irq_restore(flags);
    return count;
}

bool pci_dma_submission_force_quarantine_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = submission_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_DMA_SUBMISSION_STORAGE_CAPACITY; ++i) {
        PciDmaSubmission *submission = (PciDmaSubmission *)g_submissions[i];
        if (!submission) continue;
        if (!submission_valid_locked(submission)) { ok = false; break; }
        if (submission->assignment != assignment) continue;
        if (submission->owner_process_id != owner_process_id ||
            !pci_dma_submission_force_quarantine(submission)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    submission_irq_restore(flags);
    return ok;
}

bool pci_dma_submission_recovery_complete_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = submission_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_DMA_SUBMISSION_STORAGE_CAPACITY; ++i) {
        PciDmaSubmission *submission = (PciDmaSubmission *)g_submissions[i];
        if (!submission) continue;
        if (!submission_valid_locked(submission)) { ok = false; break; }
        if (submission->assignment != assignment) continue;
        if (submission->owner_process_id != owner_process_id ||
            submission->state != PCI_DMA_SUBMISSION_QUARANTINED ||
            !pci_dma_submission_reset_complete(submission)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    submission_irq_restore(flags);
    return ok;
}
