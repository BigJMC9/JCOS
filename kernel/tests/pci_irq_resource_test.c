#include "pci_irq_resource_test.h"

#include "../address_space.h"
#include "../capability.h"
#include "../device_resource.h"
#include "../endpoint.h"
#include "../ipc.h"
#include "../lib.h"
#include "../pci.h"
#include "../pci_assignment.h"
#include "../pci_irq_resource.h"
#include "../pmm.h"
#include "../process.h"
#include "../terminal.h"
#include "../../include/pci_irq_protocol.h"

static Process g_owner;
static PciAssignment g_assignment;
static PciIrqResource g_irq;
static CapabilityHandle g_assignment_handle;
static CapabilityHandle g_irq_handle;
static CapabilityHandle g_event_handle;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool cleanup(void) {
    bool ok = true;
    CapabilityTable *caps = process_storage_in_use(&g_owner) ?
        process_capabilities(&g_owner) : 0;

    if (pci_irq_resource_valid(&g_irq) &&
        pci_irq_resource_state(&g_irq) != PCI_IRQ_STATE_DISARMED &&
        !pci_irq_resource_force_disarm(&g_irq, g_owner.id)) ok = false;

    if (caps && g_event_handle && !capability_revoke(caps, g_event_handle)) ok = false;
    g_event_handle = CAPABILITY_INVALID_HANDLE;
    if (caps && g_irq_handle && !capability_revoke(caps, g_irq_handle)) ok = false;
    g_irq_handle = CAPABILITY_INVALID_HANDLE;

    if (pci_irq_resource_valid(&g_irq) && !pci_irq_resource_destroy(&g_irq)) ok = false;

    if (pci_assignment_valid(&g_assignment)) {
        PciAssignmentState state = pci_assignment_state(&g_assignment);
        if (state == PCI_ASSIGNMENT_ASSIGNED &&
            !pci_assignment_begin_quiesce(&g_assignment, g_owner.id)) ok = false;
        if (caps && g_assignment_handle &&
            !capability_revoke(caps, g_assignment_handle)) ok = false;
        g_assignment_handle = CAPABILITY_INVALID_HANDLE;
        state = pci_assignment_state(&g_assignment);
        if (state == PCI_ASSIGNMENT_QUIESCING &&
            !pci_assignment_complete_release(&g_assignment, g_owner.id)) ok = false;
        else if (state == PCI_ASSIGNMENT_QUARANTINED &&
            !pci_assignment_reset_complete(&g_assignment, g_owner.id)) ok = false;
        if (pci_assignment_valid(&g_assignment) &&
            !pci_assignment_destroy(&g_assignment)) ok = false;
    }

    if (caps && !capability_table_empty(caps) && !capability_revoke_all(caps)) ok = false;
    if (process_storage_in_use(&g_owner) && !process_destroy(&g_owner)) ok = false;

    k_memset(&g_owner, 0, sizeof(g_owner));
    k_memset(&g_assignment, 0, sizeof(g_assignment));
    k_memset(&g_irq, 0, sizeof(g_irq));
    g_assignment_handle = CAPABILITY_INVALID_HANDLE;
    g_irq_handle = CAPABILITY_INVALID_HANDLE;
    g_event_handle = CAPABILITY_INVALID_HANDLE;
    return ok;
}

void pci_irq_resource_test_cleanup_run(void) {
    (void)cleanup();
}

void pci_irq_resource_test_run(void) {
    terminal_writeln("PCI IRQ AUTHORITY / DELIVERY / STORM TEST:");
    (void)cleanup();

    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    const PciDevice *device = pci_device_count() ? pci_device(0U) : 0;
    bool preflight = kernel && kernel_caps && device && pci_device_is_enumerated(device);
    report("ENUMERATED PCI FUNCTION / KERNEL AUTHORITY BASELINE", preflight);
    if (!preflight) return;

    u64 free_before = pmm_stats().free_pages;
    u32 process_before = process_object_count();
    u32 space_before = address_space_object_count();
    u32 tables_before = capability_table_object_count();
    u32 endpoints_before = endpoint_object_count();
    u32 resources_before = device_resource_object_count();
    u32 assignments_before = pci_assignment_object_count();
    u32 irqs_before = pci_irq_resource_object_count();
    u32 kernel_caps_before = capability_table_count(kernel_caps);
    bool completed = false;

    bool owner_created = process_create(&g_owner);
    bool assignment_created = owner_created &&
        pci_assignment_create(&g_assignment, device);
    bool claimed = assignment_created && pci_assignment_claim(&g_assignment,
        &g_owner, &g_assignment_handle);
    report("ASSIGNED FUNCTION / OWNER CAPABILITY", claimed);
    if (!claimed) goto done;

    bool irq_created = pci_irq_resource_create(&g_irq, &g_assignment, &g_owner,
        g_assignment_handle, &g_irq_handle, &g_event_handle);
    CapabilityTable *owner_caps = process_capabilities(&g_owner);
    void *resolved = 0;
    bool exact_authority = irq_created && owner_caps &&
        capability_lookup_rights(owner_caps, g_irq_handle,
            CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_MANAGE, &resolved) &&
        resolved == &g_irq.resource;
    resolved = 0;
    exact_authority = exact_authority &&
        !capability_lookup_rights(owner_caps, g_irq_handle,
            CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_TRANSFER, &resolved) &&
        capability_lookup_rights(owner_caps, g_event_handle,
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &resolved) &&
        resolved == &g_irq.event_endpoint;
    resolved = 0;
    exact_authority = exact_authority &&
        !capability_lookup_rights(owner_caps, g_event_handle,
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &resolved) &&
        pci_assignment_child_count(&g_assignment) == 1U &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED &&
        pci_irq_resource_delivery_masked(&g_irq);
    report("NON-TRANSFERABLE MANAGE + RECEIVE AUTHORITY", exact_authority);
    if (!exact_authority) goto done;

    bool armed = pci_irq_resource_arm(&g_irq, &g_owner, g_irq_handle) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_ARMED &&
        !pci_irq_resource_delivery_masked(&g_irq) &&
        pci_assignment_active_access_count(&g_assignment) == 1U;
    report("ARM PINS ASSIGNMENT ACTIVE ACCESS", armed);
    if (!armed) goto done;

    bool delivered = pci_irq_resource_signal(&g_irq);
    u64 sequence = pci_irq_resource_pending_sequence(&g_irq);
    bool pending = delivered && sequence &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_PENDING &&
        pci_irq_resource_delivery_masked(&g_irq) &&
        pci_irq_resource_delivered_count(&g_irq) == 1ULL &&
        !pci_assignment_begin_quiesce(&g_assignment, g_owner.id);
    report("ONE IRQ -> MASKED PENDING EVENT / QUIESCE BLOCKED", pending);
    if (!pending) goto done;

    IpcMessage event;
    k_memset(&event, 0, sizeof(event));
    bool received = ipc_try_receive(&g_owner, g_event_handle, &event) &&
        event.word_count == JCOS_PCI_IRQ_EVENT_WORDS &&
        event.words[JCOS_PCI_IRQ_EVENT_MAGIC_WORD] == JCOS_PCI_IRQ_EVENT_MAGIC &&
        event.words[JCOS_PCI_IRQ_EVENT_RESOURCE_WORD] == g_irq.resource.id &&
        event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD] == sequence;
    bool acknowledged = received && pci_irq_resource_acknowledge(&g_irq,
        &g_owner, g_irq_handle, sequence) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_ARMED &&
        !pci_irq_resource_delivery_masked(&g_irq) &&
        pci_irq_resource_acknowledged_count(&g_irq) == 1ULL;
    report("EXACT SEQUENCE RECEIVE / ACK REARMS DELIVERY", acknowledged);
    if (!acknowledged) goto done;

    bool storm_start = pci_irq_resource_signal(&g_irq);
    u64 storm_sequence = pci_irq_resource_pending_sequence(&g_irq);
    bool suppressed = storm_start && storm_sequence;
    for (u32 i = 0U; i < PCI_IRQ_STORM_THRESHOLD; ++i) {
        if (pci_irq_resource_signal(&g_irq)) suppressed = false;
    }
    bool stormed = suppressed &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_STORMED &&
        pci_irq_resource_delivery_masked(&g_irq) &&
        pci_irq_resource_suppressed_count(&g_irq) >= PCI_IRQ_STORM_THRESHOLD;
    report("REPEATED UNACKED IRQ -> STORM MASK", stormed);
    if (!stormed) goto done;

    k_memset(&event, 0, sizeof(event));
    bool storm_received = ipc_try_receive(&g_owner, g_event_handle, &event) &&
        event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD] == storm_sequence;
    bool storm_ack = storm_received && pci_irq_resource_acknowledge(&g_irq,
        &g_owner, g_irq_handle, storm_sequence) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_STORMED &&
        pci_irq_resource_delivery_masked(&g_irq);
    bool recovered = storm_ack && pci_irq_resource_recover_storm(&g_irq,
        &g_owner, g_irq_handle) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_ARMED &&
        !pci_irq_resource_delivery_masked(&g_irq) &&
        pci_irq_resource_recovery_count(&g_irq) == 1ULL;
    report("STORM ACK + EXPLICIT RECOVERY", recovered);
    if (!recovered) goto done;

    bool disarmed = pci_irq_resource_disarm(&g_irq, &g_owner, g_irq_handle) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED &&
        pci_irq_resource_delivery_masked(&g_irq) &&
        pci_assignment_active_access_count(&g_assignment) == 0U &&
        !pci_irq_resource_signal(&g_irq);
    report("DISARM REMOVES ACTIVE ACCESS / DELIVERY", disarmed);
    if (!disarmed) goto done;

    bool quiescing = pci_assignment_begin_quiesce(&g_assignment, g_owner.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUIESCING &&
        !pci_irq_resource_arm(&g_irq, &g_owner, g_irq_handle);
    report("QUIESCING DENIES IRQ REARM", quiescing);
    completed = quiescing;

done: {
    bool cleaned = cleanup();
    bool baseline = cleaned &&
        pmm_stats().free_pages == free_before &&
        process_object_count() == process_before &&
        address_space_object_count() == space_before &&
        capability_table_object_count() == tables_before &&
        endpoint_object_count() == endpoints_before &&
        device_resource_object_count() == resources_before &&
        pci_assignment_object_count() == assignments_before &&
        pci_irq_resource_object_count() == irqs_before &&
        capability_table_count(kernel_caps) == kernel_caps_before;
    report("FULL RESOURCE / CAPABILITY BASELINE RESTORED", baseline);

    bool pass = completed && baseline;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PCI IRQ AUTHORITY / DELIVERY / STORM TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
}
