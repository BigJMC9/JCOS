#include "pci_irq_resource.h"

#include "arch.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "object_storage.h"
#include "../include/pci_irq_protocol.h"

#define PCI_IRQ_RFLAGS_IF (1ULL << 9)

static void *g_irq_resources[PCI_IRQ_RESOURCE_STORAGE_CAPACITY];

static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void irq_restore(u64 flags) {
    if (flags & PCI_IRQ_RFLAGS_IF) interrupts_enable();
}

static bool irq_live(const PciIrqResource *irq) {
    return object_storage_find(g_irq_resources, PCI_IRQ_RESOURCE_STORAGE_CAPACITY,
        irq) < PCI_IRQ_RESOURCE_STORAGE_CAPACITY;
}

static bool state_valid(PciIrqState state) {
    return state == PCI_IRQ_STATE_DISARMED || state == PCI_IRQ_STATE_ARMED ||
        state == PCI_IRQ_STATE_PENDING || state == PCI_IRQ_STATE_STORMED;
}

static bool assignment_matches(const PciIrqResource *irq) {
    if (!irq->assignment || !pci_assignment_valid(irq->assignment)) return false;
    DeviceResource *parent = pci_assignment_resource(irq->assignment);
    return parent && parent->id == irq->assignment_resource_id &&
        pci_assignment_owner_process_id(irq->assignment) == irq->owner_process_id;
}

static bool irq_valid_locked(const PciIrqResource *irq) {
    if (!irq_live(irq) || !irq->initialized || !irq->owner_process_id ||
        !state_valid(irq->state) || !assignment_matches(irq) ||
        !device_resource_valid(&irq->resource) ||
        irq->resource.kind != DEVICE_RESOURCE_PCI_IRQ ||
        !endpoint_storage_in_use(&irq->event_endpoint) ||
        !irq->event_endpoint.initialized || !irq->event_endpoint.id ||
        endpoint_owner_process_id(&irq->event_endpoint) != irq->owner_process_id ||
        irq->kernel_send_handle == CAPABILITY_INVALID_HANDLE) return false;

    if (irq->state == PCI_IRQ_STATE_DISARMED) {
        if (irq->assignment_access_held || !irq->delivery_masked ||
            irq->pending_sequence) return false;
    } else {
        if (!irq->assignment_access_held) return false;
        if (irq->state == PCI_IRQ_STATE_ARMED &&
            (irq->delivery_masked || irq->pending_sequence)) return false;
        if (irq->state == PCI_IRQ_STATE_PENDING &&
            (!irq->delivery_masked || !irq->pending_sequence)) return false;
        if (irq->state == PCI_IRQ_STATE_STORMED && !irq->delivery_masked) return false;
    }
    return true;
}

static bool owner_authorized_locked(PciIrqResource *irq, Process *process,
    CapabilityHandle handle) {
    if (!irq || !process || !handle || !irq_valid_locked(irq) ||
        !process_storage_in_use(process) || !process->initialized || process->kernel ||
        process->id != irq->owner_process_id ||
        pci_assignment_state(irq->assignment) != PCI_ASSIGNMENT_ASSIGNED) return false;
    CapabilityTable *caps = process_capabilities(process);
    void *object = 0;
    return caps && capability_lookup_rights(caps, handle,
        CAPABILITY_TYPE_DEVICE_RESOURCE, PCI_IRQ_RESOURCE_OWNER_RIGHTS, &object) &&
        object == &irq->resource;
}

static void increment_u64(u64 *value) {
    if (value && *value != ~0ULL) ++*value;
}

bool pci_irq_resource_valid(const PciIrqResource *irq) {
    u64 flags = irq_save();
    bool valid = irq && irq_valid_locked(irq);
    irq_restore(flags);
    return valid;
}

u32 pci_irq_resource_object_count(void) {
    u64 flags = irq_save();
    u32 count = object_storage_count(g_irq_resources,
        PCI_IRQ_RESOURCE_STORAGE_CAPACITY);
    irq_restore(flags);
    return count;
}

bool pci_irq_resource_create(PciIrqResource *irq, PciAssignment *assignment,
    Process *process, CapabilityHandle assignment_handle,
    CapabilityHandle *out_resource_handle, CapabilityHandle *out_event_handle) {
    if (!out_resource_handle || !out_event_handle) return false;
    *out_resource_handle = CAPABILITY_INVALID_HANDLE;
    *out_event_handle = CAPABILITY_INVALID_HANDLE;
    if (!irq || !assignment || !process || !assignment_handle) return false;

    u64 flags = irq_save();
    bool child_acquired = false;
    bool resource_created = false;
    bool endpoint_created = false;
    bool published = false;
    CapabilityHandle kernel_send = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle owner_resource = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle owner_event = CAPABILITY_INVALID_HANDLE;
    bool created = false;

    if (irq_live(irq) || !pci_assignment_child_acquire(assignment, process,
            assignment_handle)) goto done;
    child_acquired = true;

    u32 slot = object_storage_empty(g_irq_resources,
        PCI_IRQ_RESOURCE_STORAGE_CAPACITY);
    if (slot >= PCI_IRQ_RESOURCE_STORAGE_CAPACITY) goto rollback;

    k_memset(irq, 0, sizeof(*irq));
    if (!device_resource_create(&irq->resource, DEVICE_RESOURCE_PCI_IRQ)) goto rollback;
    resource_created = true;
    if (!endpoint_create_owned(&irq->event_endpoint, process)) goto rollback;
    endpoint_created = true;

    DeviceResource *parent = pci_assignment_resource(assignment);
    if (!parent || !parent->id) goto rollback;

    irq->assignment = assignment;
    irq->assignment_resource_id = parent->id;
    irq->owner_process_id = process->id;
    irq->next_sequence = 1ULL;
    irq->state = PCI_IRQ_STATE_DISARMED;
    irq->delivery_masked = true;
    irq->initialized = true;
    g_irq_resources[slot] = irq;
    published = true;

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    CapabilityTable *owner_caps = process_capabilities(process);
    if (!kernel_caps || !owner_caps ||
        !capability_insert(kernel_caps, &irq->event_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &kernel_send) ||
        !capability_insert(owner_caps, &irq->resource, CAPABILITY_TYPE_DEVICE_RESOURCE,
            PCI_IRQ_RESOURCE_OWNER_RIGHTS, &owner_resource) ||
        !capability_insert(owner_caps, &irq->event_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &owner_event)) goto rollback;

    irq->kernel_send_handle = kernel_send;
    *out_resource_handle = owner_resource;
    *out_event_handle = owner_event;
    created = true;
    goto done;

rollback: {
    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    CapabilityTable *owner_caps = process_capabilities(process);
    if (owner_event && (!owner_caps || !capability_revoke(owner_caps, owner_event)))
        cpu_halt_forever();
    if (owner_resource && (!owner_caps || !capability_revoke(owner_caps, owner_resource)))
        cpu_halt_forever();
    if (kernel_send && (!kernel_caps || !capability_revoke(kernel_caps, kernel_send)))
        cpu_halt_forever();
    if (published) g_irq_resources[slot] = 0;
    if (endpoint_created && !endpoint_destroy(&irq->event_endpoint)) cpu_halt_forever();
    if (resource_created && !device_resource_destroy(&irq->resource)) cpu_halt_forever();
    k_memset(irq, 0, sizeof(*irq));
    if (child_acquired && !pci_assignment_child_release(assignment, process->id))
        cpu_halt_forever();
}

done:
    irq_restore(flags);
    return created;
}

bool pci_irq_resource_destroy(PciIrqResource *irq) {
    if (!irq) return false;
    u64 flags = irq_save();
    u32 slot = object_storage_find(g_irq_resources,
        PCI_IRQ_RESOURCE_STORAGE_CAPACITY, irq);
    if (slot >= PCI_IRQ_RESOURCE_STORAGE_CAPACITY || !irq_valid_locked(irq) ||
        irq->state != PCI_IRQ_STATE_DISARMED || irq->resource.capability_refs ||
        irq->event_endpoint.capability_refs != 1ULL ||
        endpoint_message_ready(&irq->event_endpoint) ||
        endpoint_receiver_waiting(&irq->event_endpoint) ||
        endpoint_sender_waiting(&irq->event_endpoint)) {
        irq_restore(flags);
        return false;
    }

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    void *resolved = 0;
    if (!kernel_caps || !capability_lookup_rights(kernel_caps, irq->kernel_send_handle,
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &resolved) ||
        resolved != &irq->event_endpoint) {
        irq_restore(flags);
        return false;
    }

    PciAssignment *assignment = irq->assignment;
    u64 owner = irq->owner_process_id;
    if (!capability_revoke(kernel_caps, irq->kernel_send_handle) ||
        !endpoint_destroy(&irq->event_endpoint) ||
        !device_resource_destroy(&irq->resource)) cpu_halt_forever();

    g_irq_resources[slot] = 0;
    k_memset(irq, 0, sizeof(*irq));
    if (!pci_assignment_child_release(assignment, owner)) cpu_halt_forever();
    irq_restore(flags);
    return true;
}

PciIrqState pci_irq_resource_state(const PciIrqResource *irq) {
    u64 flags = irq_save();
    PciIrqState state = irq && irq_valid_locked(irq) ? irq->state : PCI_IRQ_STATE_INVALID;
    irq_restore(flags);
    return state;
}

u64 pci_irq_resource_owner_process_id(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 owner = irq && irq_valid_locked(irq) ? irq->owner_process_id : 0ULL;
    irq_restore(flags);
    return owner;
}

u64 pci_irq_resource_pending_sequence(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 sequence = irq && irq_valid_locked(irq) ? irq->pending_sequence : 0ULL;
    irq_restore(flags);
    return sequence;
}

u64 pci_irq_resource_delivered_count(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 count = irq && irq_valid_locked(irq) ? irq->delivered_count : 0ULL;
    irq_restore(flags);
    return count;
}

u64 pci_irq_resource_acknowledged_count(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 count = irq && irq_valid_locked(irq) ? irq->acknowledged_count : 0ULL;
    irq_restore(flags);
    return count;
}

u64 pci_irq_resource_suppressed_count(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 count = irq && irq_valid_locked(irq) ? irq->suppressed_count : 0ULL;
    irq_restore(flags);
    return count;
}

u64 pci_irq_resource_recovery_count(const PciIrqResource *irq) {
    u64 flags = irq_save();
    u64 count = irq && irq_valid_locked(irq) ? irq->recovery_count : 0ULL;
    irq_restore(flags);
    return count;
}

bool pci_irq_resource_delivery_masked(const PciIrqResource *irq) {
    u64 flags = irq_save();
    bool masked = irq && irq_valid_locked(irq) && irq->delivery_masked;
    irq_restore(flags);
    return masked;
}

bool pci_irq_resource_arm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = irq_save();
    bool armed = owner_authorized_locked(irq, process, resource_handle) &&
        irq->state == PCI_IRQ_STATE_DISARMED && !irq->assignment_access_held &&
        pci_assignment_access_acquire(irq->assignment, irq->owner_process_id);
    if (armed) {
        irq->assignment_access_held = true;
        irq->delivery_masked = false;
        irq->suppressed_since_ack = 0U;
        irq->state = PCI_IRQ_STATE_ARMED;
    }
    irq_restore(flags);
    return armed;
}

static bool force_disarm_locked(PciIrqResource *irq, u64 owner_process_id) {
    if (!irq_valid_locked(irq) || irq->owner_process_id != owner_process_id) return false;
    if (irq->state == PCI_IRQ_STATE_DISARMED) return true;

    if (endpoint_message_ready(&irq->event_endpoint)) {
        IpcMessage discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!endpoint_try_receive(&irq->event_endpoint, &discard)) cpu_halt_forever();
    }
    irq->pending_sequence = 0ULL;
    irq->suppressed_since_ack = 0U;
    irq->delivery_masked = true;
    irq->state = PCI_IRQ_STATE_DISARMED;
    if (!irq->assignment_access_held ||
        !pci_assignment_access_release(irq->assignment, owner_process_id))
        cpu_halt_forever();
    irq->assignment_access_held = false;
    return true;
}

bool pci_irq_resource_force_disarm(PciIrqResource *irq, u64 owner_process_id) {
    if (!irq || !owner_process_id) return false;
    u64 flags = irq_save();
    bool disarmed = force_disarm_locked(irq, owner_process_id);
    irq_restore(flags);
    return disarmed;
}

bool pci_irq_resource_disarm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = irq_save();
    bool disarmed = owner_authorized_locked(irq, process, resource_handle) &&
        force_disarm_locked(irq, irq->owner_process_id);
    irq_restore(flags);
    return disarmed;
}

bool pci_irq_resource_force_disarm_assignment(PciAssignment *assignment,
    u64 owner_process_id, u32 *out_count) {
    if (!assignment || !owner_process_id || !out_count) return false;
    *out_count = 0U;
    u64 flags = irq_save();
    bool ok = true;
    for (u32 i = 0U; i < PCI_IRQ_RESOURCE_STORAGE_CAPACITY; ++i) {
        PciIrqResource *irq = (PciIrqResource *)g_irq_resources[i];
        if (!irq) continue;
        if (!irq_valid_locked(irq)) { ok = false; break; }
        if (irq->assignment != assignment) continue;
        if (irq->owner_process_id != owner_process_id ||
            !force_disarm_locked(irq, owner_process_id)) {
            ok = false;
            break;
        }
        ++*out_count;
    }
    irq_restore(flags);
    return ok;
}

bool pci_irq_resource_signal(PciIrqResource *irq) {
    if (!irq) return false;
    u64 flags = irq_save();
    if (!irq_valid_locked(irq) ||
        pci_assignment_state(irq->assignment) != PCI_ASSIGNMENT_ASSIGNED) {
        irq_restore(flags);
        return false;
    }

    if (irq->state == PCI_IRQ_STATE_PENDING || irq->state == PCI_IRQ_STATE_STORMED) {
        increment_u64(&irq->suppressed_count);
        if (irq->suppressed_since_ack != 0xFFFFFFFFU) ++irq->suppressed_since_ack;
        if (irq->suppressed_since_ack >= PCI_IRQ_STORM_THRESHOLD)
            irq->state = PCI_IRQ_STATE_STORMED;
        irq->delivery_masked = true;
        irq_restore(flags);
        return false;
    }
    if (irq->state != PCI_IRQ_STATE_ARMED || irq->delivery_masked ||
        irq->pending_sequence) {
        irq_restore(flags);
        return false;
    }

    u64 sequence = irq->next_sequence;
    if (!sequence) {
        irq_restore(flags);
        return false;
    }
    irq->next_sequence = sequence == ~0ULL ? 0ULL : sequence + 1ULL;

    irq->pending_sequence = sequence;
    irq->delivery_masked = true;
    irq->state = PCI_IRQ_STATE_PENDING;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = JCOS_PCI_IRQ_EVENT_WORDS;
    message.words[JCOS_PCI_IRQ_EVENT_MAGIC_WORD] = JCOS_PCI_IRQ_EVENT_MAGIC;
    message.words[JCOS_PCI_IRQ_EVENT_RESOURCE_WORD] = irq->resource.id;
    message.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD] = sequence;
    message.words[JCOS_PCI_IRQ_EVENT_SUPPRESSED_WORD] = irq->suppressed_count;

    Process *kernel = process_kernel();
    bool delivered = kernel && ipc_try_send(kernel, irq->kernel_send_handle, &message);
    if (delivered) {
        increment_u64(&irq->delivered_count);
    } else {
        irq->pending_sequence = 0ULL;
        irq->state = PCI_IRQ_STATE_STORMED;
    }
    irq_restore(flags);
    return delivered;
}

bool pci_irq_resource_acknowledge(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle, u64 sequence) {
    if (!sequence) return false;
    u64 flags = irq_save();
    bool acknowledged = owner_authorized_locked(irq, process, resource_handle) &&
        (irq->state == PCI_IRQ_STATE_PENDING || irq->state == PCI_IRQ_STATE_STORMED) &&
        irq->pending_sequence == sequence &&
        !endpoint_message_ready(&irq->event_endpoint);
    if (acknowledged) {
        irq->pending_sequence = 0ULL;
        increment_u64(&irq->acknowledged_count);
        if (irq->state == PCI_IRQ_STATE_PENDING) {
            irq->state = PCI_IRQ_STATE_ARMED;
            irq->delivery_masked = false;
            irq->suppressed_since_ack = 0U;
        }
    }
    irq_restore(flags);
    return acknowledged;
}

bool pci_irq_resource_recover_storm(PciIrqResource *irq, Process *process,
    CapabilityHandle resource_handle) {
    u64 flags = irq_save();
    bool recovered = owner_authorized_locked(irq, process, resource_handle) &&
        irq->state == PCI_IRQ_STATE_STORMED && !irq->pending_sequence &&
        !endpoint_message_ready(&irq->event_endpoint);
    if (recovered) {
        irq->suppressed_since_ack = 0U;
        irq->delivery_masked = false;
        irq->state = PCI_IRQ_STATE_ARMED;
        increment_u64(&irq->recovery_count);
    }
    irq_restore(flags);
    return recovered;
}
