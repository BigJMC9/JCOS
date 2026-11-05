#include "pci_irq_router_test.h"

#include "../address_space.h"
#include "../capability.h"
#include "../device_resource.h"
#include "../endpoint.h"
#include "../interrupt_controller.h"
#include "../lib.h"
#include "../pci.h"
#include "../pci_assignment.h"
#include "../pci_irq_resource.h"
#include "../pci_irq_router.h"
#include "../pmm.h"
#include "../process.h"
#include "../terminal.h"

static Process g_owner;
static PciAssignment g_assignment;
static PciIrqResource g_irq;
static CapabilityHandle g_assignment_handle;
static CapabilityHandle g_irq_handle;
static CapabilityHandle g_event_handle;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 tables;
    u32 endpoints;
    u32 resources;
    u32 assignments;
    u32 irqs;
    u32 bindings;
} PciIrqRouterBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciIrqRouterBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->spaces = address_space_object_count();
    out->tables = capability_table_object_count();
    out->endpoints = endpoint_object_count();
    out->resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->irqs = pci_irq_resource_object_count();
    out->bindings = pci_irq_router_binding_count();
    return true;
}

static bool baseline_matches(const PciIrqRouterBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        capability_table_object_count() == baseline->tables &&
        endpoint_object_count() == baseline->endpoints &&
        device_resource_object_count() == baseline->resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_irq_resource_object_count() == baseline->irqs &&
        pci_irq_router_binding_count() == baseline->bindings;
}

static const PciDevice *legacy_intx_candidate(void) {
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || !pci_device_is_enumerated(device)) continue;
        if (device->interrupt_pin < 1U || device->interrupt_pin > 4U) continue;
        if (device->interrupt_line < 3U ||
            device->interrupt_line >= ACPI_LEGACY_IRQ_COUNT) continue;
        return device;
    }
    return 0;
}

static bool cleanup(void) {
    bool ok = true;
    CapabilityTable *caps = process_storage_in_use(&g_owner) ?
        process_capabilities(&g_owner) : 0;

    if (pci_irq_router_bound(&g_irq) &&
        !pci_irq_router_force_unbind(&g_irq, g_owner.id)) ok = false;
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

void pci_irq_router_test_cleanup_run(void) {
    (void)cleanup();
}

void pci_irq_router_test_run(void) {
    terminal_writeln("PCI LEGACY INTX ROUTER / IOAPIC LEASE TEST:");
    (void)cleanup();

    PciIrqRouterBaseline baseline;
    k_memset(&baseline, 0, sizeof(baseline));
    bool baseline_ok = capture_baseline(&baseline);
    report("RESOURCE BASELINE CAPTURED", baseline_ok);
    if (!baseline_ok) return;

    InterruptControllerInfo controller = interrupt_controller_info();
    const PciDevice *device = legacy_intx_candidate();
    bool platform_supported = controller.mode == INTERRUPT_CONTROLLER_APIC && device;
    report("APIC / LEGACY INTX CANDIDATE", platform_supported ||
        controller.mode != INTERRUPT_CONTROLLER_APIC || !device);

    /* Lack of a firmware-programmed legacy line is a supported platform limit.
     * MSI/MSI-X and ACPI _PRT routing are intentionally not inferred here. */
    if (!platform_supported) {
        bool safe = pci_irq_router_binding_count() == baseline.bindings &&
            baseline_matches(&baseline);
        report("UNSUPPORTED ROUTE FAILS CLOSED", safe);
        terminal_set_color(safe ? terminal_accent_color() : terminal_error_color());
        terminal_write("PCI LEGACY INTX ROUTER / IOAPIC LEASE TEST: ");
        terminal_writeln(safe ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    bool completed = false;
    bool created = process_create(&g_owner) &&
        pci_assignment_create(&g_assignment, device) &&
        pci_assignment_claim(&g_assignment, &g_owner, &g_assignment_handle) &&
        pci_irq_resource_create(&g_irq, &g_assignment, &g_owner,
            g_assignment_handle, &g_irq_handle, &g_event_handle);
    report("ASSIGNMENT + IRQ RESOURCE CREATED", created);
    if (!created) goto done;

    bool bound = pci_irq_router_bind_legacy_intx(&g_irq, &g_owner, g_irq_handle);
    if (!bound) {
        /* An already-live IOAPIC source is never stolen. */
        bool safe_refusal = !pci_irq_router_bound(&g_irq) &&
            pci_irq_router_binding_count() == baseline.bindings &&
            pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED;
        report("LIVE / UNAVAILABLE SOURCE REFUSED WITHOUT STEALING", safe_refusal);
        completed = safe_refusal;
        goto done;
    }

    u8 vector = pci_irq_router_vector(&g_irq);
    u32 gsi = pci_irq_router_gsi(&g_irq);
    bool lease = vector >= PCI_IRQ_ROUTER_VECTOR_FIRST &&
        vector <= PCI_IRQ_ROUTER_VECTOR_LAST &&
        pci_irq_router_gsi(&g_irq) == gsi && pci_irq_router_source_masked(&g_irq) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED &&
        pci_irq_router_binding_count() == baseline.bindings + 1U;
    report("MASKED IOAPIC LEASE + DYNAMIC VECTOR", lease);
    if (!lease) goto done;

    /* The source stays hardware-masked. Dispatch ownership can therefore be
     * proven without exposing a live device interrupt during the test. */
    u64 delivered_before = pci_irq_resource_delivered_count(&g_irq);
    bool dispatch_owned = pci_irq_router_handle_vector(vector) &&
        pci_irq_router_source_masked(&g_irq) &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED &&
        pci_irq_resource_delivered_count(&g_irq) == delivered_before;
    report("BOUND VECTOR DISPATCH / DISARMED DELIVERY SUPPRESSED", dispatch_owned);
    if (!dispatch_owned) goto done;

    bool unbound = pci_irq_router_unbind(&g_irq, &g_owner, g_irq_handle) &&
        !pci_irq_router_bound(&g_irq) &&
        pci_irq_router_binding_count() == baseline.bindings &&
        !pci_irq_router_handle_vector(vector);
    report("UNBIND RESTORES PRIOR IOAPIC ENTRY", unbound);
    completed = unbound;

done: {
    bool cleaned = cleanup();
    bool baseline_restored = cleaned && baseline_matches(&baseline);
    report("FULL ROUTER / RESOURCE BASELINE RESTORED", baseline_restored);
    bool pass = completed && baseline_restored;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PCI LEGACY INTX ROUTER / IOAPIC LEASE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
}
