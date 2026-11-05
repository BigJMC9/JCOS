#include "pci_dma_buffer.h"

#include "arch.h"
#include "address_space.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "physmap.h"
#include "pmm.h"
#include "vmm.h"

#define PCI_DMA_RFLAGS_IF (1ULL << 9)
#define PCI_DMA_OWNER_RIGHTS \
    (CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE | CAPABILITY_RIGHT_MANAGE)

static void *g_dma_buffers[PCI_DMA_BUFFER_STORAGE_CAPACITY];

static u64 dma_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void dma_irq_restore(u64 flags) {
    if (flags & PCI_DMA_RFLAGS_IF) interrupts_enable();
}

static bool buffer_live(const PciDmaBuffer *buffer) {
    return object_storage_find(g_dma_buffers, PCI_DMA_BUFFER_STORAGE_CAPACITY,
        buffer) < PCI_DMA_BUFFER_STORAGE_CAPACITY;
}

static bool state_valid(PciDmaBufferState state) {
    return state == PCI_DMA_BUFFER_READY || state == PCI_DMA_BUFFER_DEVICE_ACTIVE ||
        state == PCI_DMA_BUFFER_QUARANTINED;
}

static bool assignment_matches(const PciDmaBuffer *buffer) {
    if (!buffer || !buffer->assignment || !pci_assignment_valid(buffer->assignment))
        return false;
    DeviceResource *parent = pci_assignment_resource(buffer->assignment);
    return parent && parent->id == buffer->assignment_resource_id &&
        pci_assignment_owner_process_id(buffer->assignment) == buffer->owner_process_id;
}

static bool buffer_valid_locked(const PciDmaBuffer *buffer) {
    if (!buffer_live(buffer) || !buffer->initialized || !buffer->process ||
        !buffer->owner_process_id || !buffer->physical_base || !buffer->page_count ||
        buffer->page_count > PCI_DMA_BUFFER_MAX_PAGES ||
        buffer->mapped_page_count > buffer->page_count || !state_valid(buffer->state) ||
        !assignment_matches(buffer) || !device_resource_valid(&buffer->resource) ||
        buffer->resource.kind != DEVICE_RESOURCE_PCI_DMA_BUFFER) return false;

    if (!process_storage_in_use(buffer->process) || !buffer->process->initialized ||
        buffer->process->kernel || buffer->process->id != buffer->owner_process_id)
        return false;

    if (buffer->mapped_page_count) {
        if (buffer->mapped_page_count != buffer->page_count || !buffer->virtual_base ||
            (buffer->virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
            buffer->virtual_base < ADDRESS_SPACE_USER_BASE ||
            (u64)buffer->page_count * VM_PAGE_SIZE >
                ADDRESS_SPACE_USER_LIMIT - buffer->virtual_base) return false;
    } else if (buffer->virtual_base || buffer->writable_mapping) {
        return false;
    }

    if (buffer->state == PCI_DMA_BUFFER_READY) {
        if (buffer->assignment_access_held) return false;
    } else if (!buffer->assignment_access_held) {
        return false;
    }

    return true;
}

bool pci_dma_buffer_valid(const PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    bool valid = buffer && buffer_valid_locked(buffer);
    dma_irq_restore(flags);
    return valid;
}

u32 pci_dma_buffer_object_count(void) {
    u64 flags = dma_irq_save();
    u32 count = object_storage_count(g_dma_buffers, PCI_DMA_BUFFER_STORAGE_CAPACITY);
    dma_irq_restore(flags);
    return count;
}

static bool owner_authorized_locked(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle, CapabilityRights rights) {
    if (!buffer || !process || !handle || !buffer_valid_locked(buffer) ||
        !process_storage_in_use(process) || process->kernel ||
        process->id != buffer->owner_process_id ||
        pci_assignment_state(buffer->assignment) != PCI_ASSIGNMENT_ASSIGNED)
        return false;

    CapabilityTable *caps = process_capabilities(process);
    void *object = 0;
    return caps && capability_lookup_rights(caps, handle,
        CAPABILITY_TYPE_DEVICE_RESOURCE, rights, &object) && object == &buffer->resource;
}

static bool zero_buffer(PciDmaBuffer *buffer) {
    if (!buffer || !buffer->physical_base || !buffer->page_count) return false;
    void *direct = phys_to_virt(buffer->physical_base);
    if (!direct) return false;
    k_memset(direct, 0, (usize)((u64)buffer->page_count * VM_PAGE_SIZE));
    return true;
}

bool pci_dma_buffer_create(PciDmaBuffer *buffer, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle, u32 page_count,
    CapabilityHandle *out_handle) {
    if (out_handle) *out_handle = CAPABILITY_INVALID_HANDLE;
    if (!buffer || !assignment || !process || !assignment_handle || !out_handle ||
        !page_count || page_count > PCI_DMA_BUFFER_MAX_PAGES) return false;

    u64 flags = dma_irq_save();
    bool child_acquired = false;
    bool resource_created = false;
    bool published = false;
    bool created = false;
    u64 physical = 0ULL;
    CapabilityHandle handle = CAPABILITY_INVALID_HANDLE;

    if (buffer_live(buffer) ||
        !pci_assignment_child_acquire(assignment, process, assignment_handle)) goto done;
    child_acquired = true;

    u32 slot = object_storage_empty(g_dma_buffers, PCI_DMA_BUFFER_STORAGE_CAPACITY);
    if (slot >= PCI_DMA_BUFFER_STORAGE_CAPACITY) goto rollback;

    physical = pmm_alloc_pages(page_count);
    if (!physical) goto rollback;

    k_memset(buffer, 0, sizeof(*buffer));
    if (!device_resource_create(&buffer->resource, DEVICE_RESOURCE_PCI_DMA_BUFFER))
        goto rollback;
    resource_created = true;

    DeviceResource *parent = pci_assignment_resource(assignment);
    if (!parent || !parent->id) goto rollback;

    buffer->assignment = assignment;
    buffer->process = process;
    buffer->assignment_resource_id = parent->id;
    buffer->owner_process_id = process->id;
    buffer->physical_base = physical;
    buffer->page_count = page_count;
    buffer->state = PCI_DMA_BUFFER_READY;
    buffer->initialized = true;
    g_dma_buffers[slot] = buffer;
    published = true;

    if (!zero_buffer(buffer)) goto rollback;

    CapabilityTable *caps = process_capabilities(process);
    if (!caps || !capability_insert(caps, &buffer->resource,
            CAPABILITY_TYPE_DEVICE_RESOURCE, PCI_DMA_OWNER_RIGHTS, &handle)) goto rollback;

    *out_handle = handle;
    created = true;
    goto done;

rollback:
    if (handle != CAPABILITY_INVALID_HANDLE) {
        CapabilityTable *caps = process_capabilities(process);
        if (!caps || !capability_revoke(caps, handle)) cpu_halt_forever();
    }
    if (published) g_dma_buffers[slot] = 0;
    if (resource_created && !device_resource_destroy(&buffer->resource)) cpu_halt_forever();
    if (physical) {
        frame_t first = phys_to_frame(physical);
        if (first == FRAME_INVALID || !frame_free_range(first, page_count)) cpu_halt_forever();
    }
    k_memset(buffer, 0, sizeof(*buffer));
    if (child_acquired && !pci_assignment_child_release(assignment, process->id))
        cpu_halt_forever();

done:
    dma_irq_restore(flags);
    return created;
}

static bool virtual_range_valid(u64 virtual_base, u32 pages) {
    if (!virtual_base || !pages || (virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
        virtual_base < ADDRESS_SPACE_USER_BASE) return false;
    u64 size = (u64)pages * VM_PAGE_SIZE;
    return size <= ADDRESS_SPACE_USER_LIMIT - virtual_base;
}

static bool rollback_mapping(AddressSpace *space, u64 virtual_base, frame_t first,
    u32 mapped) {
    while (mapped) {
        --mapped;
        frame_t old = FRAME_INVALID;
        if (!address_space_unmap_page(space,
                virtual_base + (u64)mapped * VM_PAGE_SIZE, &old) ||
            old != first + mapped) return false;
    }
    return true;
}

bool pci_dma_buffer_map(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle, u64 virtual_base, bool writable) {
    if (!buffer || !process || !handle) return false;
    u64 flags = dma_irq_save();
    bool mapped_ok = false;

    if (!owner_authorized_locked(buffer, process, handle, CAPABILITY_RIGHT_READ) ||
        buffer->state != PCI_DMA_BUFFER_READY || buffer->mapped_page_count ||
        !virtual_range_valid(virtual_base, buffer->page_count)) goto done;

    if (writable && !owner_authorized_locked(buffer, process, handle,
            CAPABILITY_RIGHT_WRITE)) goto done;

    AddressSpace *space = process_address_space(process);
    frame_t first = phys_to_frame(buffer->physical_base);
    if (!space || first == FRAME_INVALID) goto done;

    vm_flags_t map_flags = writable ? VM_WRITE : 0ULL;
    u32 mapped = 0U;
    for (; mapped < buffer->page_count; ++mapped) {
        if (!address_space_map_page(space,
                virtual_base + (u64)mapped * VM_PAGE_SIZE,
                first + mapped, map_flags)) break;
    }
    if (mapped != buffer->page_count) {
        if (!rollback_mapping(space, virtual_base, first, mapped)) cpu_halt_forever();
        goto done;
    }

    buffer->virtual_base = virtual_base;
    buffer->mapped_page_count = buffer->page_count;
    buffer->writable_mapping = writable;
    mapped_ok = true;

done:
    dma_irq_restore(flags);
    return mapped_ok;
}

static bool force_unmap_locked(PciDmaBuffer *buffer, u64 owner_process_id) {
    if (!buffer_valid_locked(buffer) || !owner_process_id ||
        buffer->owner_process_id != owner_process_id) return false;
    if (!buffer->mapped_page_count) return true;

    AddressSpace *space = process_address_space(buffer->process);
    frame_t first = phys_to_frame(buffer->physical_base);
    if (!space || first == FRAME_INVALID) return false;

    while (buffer->mapped_page_count) {
        u32 index = buffer->mapped_page_count - 1U;
        u64 virtual_address = buffer->virtual_base + (u64)index * VM_PAGE_SIZE;
        frame_t expected = first + index;
        frame_t mapped = FRAME_INVALID;
        vm_flags_t page_flags = 0ULL;
        vm_flags_t expected_flags = VM_USER | (buffer->writable_mapping ? VM_WRITE : 0ULL);
        if (!address_space_query_page(space, virtual_address, &mapped, &page_flags) ||
            mapped != expected || page_flags != expected_flags) return false;
        frame_t old = FRAME_INVALID;
        if (!address_space_unmap_page(space, virtual_address, &old) || old != expected)
            return false;
        buffer->mapped_page_count = index;
    }

    buffer->virtual_base = 0ULL;
    buffer->writable_mapping = false;
    return true;
}

bool pci_dma_buffer_force_unmap(PciDmaBuffer *buffer, u64 owner_process_id) {
    if (!buffer || !owner_process_id) return false;
    u64 flags = dma_irq_save();
    bool unmapped = force_unmap_locked(buffer, owner_process_id);
    dma_irq_restore(flags);
    return unmapped;
}

bool pci_dma_buffer_unmap(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle) {
    if (!buffer || !process || !handle) return false;
    u64 flags = dma_irq_save();
    bool unmapped = owner_authorized_locked(buffer, process, handle,
        CAPABILITY_RIGHT_MANAGE) && force_unmap_locked(buffer, process->id);
    dma_irq_restore(flags);
    return unmapped;
}

bool pci_dma_buffer_force_unmap_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = dma_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_DMA_BUFFER_STORAGE_CAPACITY; ++i) {
        PciDmaBuffer *buffer = (PciDmaBuffer *)g_dma_buffers[i];
        if (!buffer) continue;
        if (!buffer_valid_locked(buffer)) { ok = false; break; }
        if (buffer->assignment != assignment) continue;
        if (buffer->owner_process_id != owner_process_id) { ok = false; break; }
        if (!buffer->mapped_page_count) continue;
        if (!force_unmap_locked(buffer, owner_process_id)) { ok = false; break; }
        ++*out_count;
    }
    dma_irq_restore(flags);
    return ok;
}

bool pci_dma_buffer_device_begin(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle) {
    if (!buffer || !process || !handle) return false;
    u64 flags = dma_irq_save();
    bool begun = owner_authorized_locked(buffer, process, handle,
        CAPABILITY_RIGHT_MANAGE) && buffer->state == PCI_DMA_BUFFER_READY &&
        !buffer->assignment_access_held &&
        pci_assignment_access_acquire(buffer->assignment, buffer->owner_process_id);
    if (begun) {
        buffer->assignment_access_held = true;
        buffer->state = PCI_DMA_BUFFER_DEVICE_ACTIVE;
    }
    dma_irq_restore(flags);
    return begun;
}

bool pci_dma_buffer_device_complete(PciDmaBuffer *buffer, Process *process,
    CapabilityHandle handle) {
    if (!buffer || !process || !handle) return false;
    u64 flags = dma_irq_save();
    bool completed = owner_authorized_locked(buffer, process, handle,
        CAPABILITY_RIGHT_MANAGE) && buffer->state == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
        buffer->assignment_access_held &&
        pci_assignment_access_release(buffer->assignment, buffer->owner_process_id);
    if (completed) {
        buffer->assignment_access_held = false;
        buffer->state = PCI_DMA_BUFFER_READY;
    }
    dma_irq_restore(flags);
    return completed;
}

bool pci_dma_buffer_force_quarantine(PciDmaBuffer *buffer, u64 owner_process_id) {
    if (!buffer || !owner_process_id) return false;
    u64 flags = dma_irq_save();
    bool quarantined = false;

    if (!buffer_valid_locked(buffer) || buffer->owner_process_id != owner_process_id)
        goto done;
    if (buffer->state == PCI_DMA_BUFFER_QUARANTINED) {
        quarantined = pci_assignment_state(buffer->assignment) == PCI_ASSIGNMENT_QUARANTINED;
        goto done;
    }

    PciAssignmentState parent = pci_assignment_state(buffer->assignment);
    if (buffer->state == PCI_DMA_BUFFER_READY) {
        if (parent != PCI_ASSIGNMENT_ASSIGNED || buffer->assignment_access_held ||
            !pci_assignment_access_acquire(buffer->assignment, owner_process_id)) goto done;
        buffer->assignment_access_held = true;
    } else if (buffer->state != PCI_DMA_BUFFER_DEVICE_ACTIVE ||
        !buffer->assignment_access_held ||
        (parent != PCI_ASSIGNMENT_ASSIGNED && parent != PCI_ASSIGNMENT_QUARANTINED)) {
        goto done;
    }

    if (parent != PCI_ASSIGNMENT_QUARANTINED &&
        !pci_assignment_force_quarantine(buffer->assignment, owner_process_id)) {
        if (buffer->state == PCI_DMA_BUFFER_READY) {
            if (!pci_assignment_access_release(buffer->assignment, owner_process_id))
                cpu_halt_forever();
            buffer->assignment_access_held = false;
        }
        goto done;
    }

    buffer->state = PCI_DMA_BUFFER_QUARANTINED;
    quarantined = true;

done:
    dma_irq_restore(flags);
    return quarantined;
}

bool pci_dma_buffer_reset_complete(PciDmaBuffer *buffer, u64 owner_process_id) {
    if (!buffer || !owner_process_id) return false;
    u64 flags = dma_irq_save();
    bool completed = buffer_valid_locked(buffer) &&
        buffer->owner_process_id == owner_process_id &&
        buffer->state == PCI_DMA_BUFFER_QUARANTINED &&
        buffer->assignment_access_held && !buffer->mapped_page_count &&
        pci_assignment_state(buffer->assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        zero_buffer(buffer) &&
        pci_assignment_access_release(buffer->assignment, owner_process_id);
    if (completed) {
        buffer->assignment_access_held = false;
        buffer->state = PCI_DMA_BUFFER_READY;
    }
    dma_irq_restore(flags);
    return completed;
}

bool pci_dma_buffer_destroy(PciDmaBuffer *buffer) {
    if (!buffer) return false;
    u64 flags = dma_irq_save();
    u32 slot = object_storage_find(g_dma_buffers, PCI_DMA_BUFFER_STORAGE_CAPACITY,
        buffer);
    if (slot >= PCI_DMA_BUFFER_STORAGE_CAPACITY || !buffer_valid_locked(buffer) ||
        buffer->state != PCI_DMA_BUFFER_READY || buffer->assignment_access_held ||
        buffer->mapped_page_count || buffer->resource.capability_refs) {
        dma_irq_restore(flags);
        return false;
    }

    frame_t first = phys_to_frame(buffer->physical_base);
    if (first == FRAME_INVALID || !frame_free_range(first, buffer->page_count)) {
        dma_irq_restore(flags);
        return false;
    }

    PciAssignment *assignment = buffer->assignment;
    u64 owner = buffer->owner_process_id;
    if (!device_resource_destroy(&buffer->resource)) cpu_halt_forever();
    g_dma_buffers[slot] = 0;
    k_memset(buffer, 0, sizeof(*buffer));
    if (!pci_assignment_child_release(assignment, owner)) cpu_halt_forever();
    dma_irq_restore(flags);
    return true;
}

PciDmaBufferState pci_dma_buffer_state(const PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    PciDmaBufferState state = buffer && buffer_valid_locked(buffer) ?
        buffer->state : PCI_DMA_BUFFER_INVALID;
    dma_irq_restore(flags);
    return state;
}

u64 pci_dma_buffer_owner_process_id(const PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    u64 owner = buffer && buffer_valid_locked(buffer) ? buffer->owner_process_id : 0ULL;
    dma_irq_restore(flags);
    return owner;
}

u64 pci_dma_buffer_size(const PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    u64 size = buffer && buffer_valid_locked(buffer) ?
        (u64)buffer->page_count * VM_PAGE_SIZE : 0ULL;
    dma_irq_restore(flags);
    return size;
}

PciAssignment *pci_dma_buffer_assignment(PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    PciAssignment *assignment = buffer && buffer_valid_locked(buffer) ?
        buffer->assignment : 0;
    dma_irq_restore(flags);
    return assignment;
}

bool pci_dma_buffer_cpu_mapping_active(const PciDmaBuffer *buffer) {
    u64 flags = dma_irq_save();
    bool mapped = buffer && buffer_valid_locked(buffer) &&
        buffer->mapped_page_count != 0U;
    dma_irq_restore(flags);
    return mapped;
}

bool pci_dma_buffer_device_span(const PciDmaBuffer *buffer,
    u64 *out_physical_base, u64 *out_size) {
    if (!out_physical_base || !out_size) return false;
    *out_physical_base = 0ULL;
    *out_size = 0ULL;
    u64 flags = dma_irq_save();
    bool valid = buffer && buffer_valid_locked(buffer);
    if (valid) {
        *out_physical_base = buffer->physical_base;
        *out_size = (u64)buffer->page_count * VM_PAGE_SIZE;
    }
    dma_irq_restore(flags);
    return valid;
}

bool pci_dma_buffer_force_quarantine_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = dma_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_DMA_BUFFER_STORAGE_CAPACITY; ++i) {
        PciDmaBuffer *buffer = (PciDmaBuffer *)g_dma_buffers[i];
        if (!buffer) continue;
        if (!buffer_valid_locked(buffer)) { ok = false; break; }
        if (buffer->assignment != assignment) continue;
        if (buffer->owner_process_id != owner_process_id) { ok = false; break; }
        if (buffer->state == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
            !pci_dma_buffer_force_quarantine(buffer, owner_process_id)) {
            ok = false;
            break;
        }
        if (buffer->state == PCI_DMA_BUFFER_QUARANTINED) ++*out_count;
    }
    dma_irq_restore(flags);
    return ok;
}

bool pci_dma_buffer_recovery_complete_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = dma_irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_DMA_BUFFER_STORAGE_CAPACITY; ++i) {
        PciDmaBuffer *buffer = (PciDmaBuffer *)g_dma_buffers[i];
        if (!buffer) continue;
        if (!buffer_valid_locked(buffer)) { ok = false; break; }
        if (buffer->assignment != assignment) continue;
        if (buffer->owner_process_id != owner_process_id) { ok = false; break; }
        if (buffer->state != PCI_DMA_BUFFER_QUARANTINED) continue;
        if (!pci_dma_buffer_reset_complete(buffer, owner_process_id)) { ok = false; break; }
        ++*out_count;
    }
    dma_irq_restore(flags);
    return ok;
}
