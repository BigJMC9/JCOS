#include "pci_assignment_test.h"

#include "address_space.h"
#include "capability.h"
#include "device_resource.h"
#include "lib.h"
#include "pci.h"
#include "pci_assignment.h"
#include "pmm.h"
#include "process.h"
#include "terminal.h"

static PciAssignment g_assignment;
static PciAssignment g_duplicate;
static Process g_driver_a;
static Process g_driver_b;
static CapabilityHandle g_driver_a_cap;
static CapabilityHandle g_driver_b_cap;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 address_spaces;
    u32 capability_tables;
    u32 device_resources;
    u32 assignments;
} PciAssignmentBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciAssignmentBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->capability_tables = capability_table_object_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    return true;
}

static bool baseline_matches(const PciAssignmentBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        capability_table_object_count() == baseline->capability_tables &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments;
}

static bool revoke_process_caps(Process *process) {
    if (!process_storage_in_use(process)) return true;
    CapabilityTable *caps = process_capabilities(process);
    return caps && capability_revoke_all(caps);
}

static bool settle_assignment(PciAssignment *assignment) {
    if (!pci_assignment_valid(assignment)) return true;

    u64 owner = pci_assignment_owner_process_id(assignment);
    PciAssignmentState state = pci_assignment_state(assignment);
    if (state == PCI_ASSIGNMENT_ASSIGNED) {
        if (!owner) return false;
        if (pci_assignment_bus_master_exposed(assignment)) {
            /* This fixture only marks software history; no DMA was enabled. */
            if (!pci_assignment_force_quarantine(assignment, owner)) return false;
            state = PCI_ASSIGNMENT_QUARANTINED;
        } else {
            if (!pci_assignment_begin_quiesce(assignment, owner)) return false;
            state = PCI_ASSIGNMENT_QUIESCING;
        }
    }
    if (state == PCI_ASSIGNMENT_QUIESCING) {
        if (!owner || !pci_assignment_complete_release(assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    if (state == PCI_ASSIGNMENT_QUARANTINED) {
        if (!owner || !pci_assignment_reset_complete(assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    return state == PCI_ASSIGNMENT_AVAILABLE && pci_assignment_destroy(assignment);
}

static bool cleanup_all(void) {
    bool ok = true;
    if (!revoke_process_caps(&g_driver_a)) ok = false;
    if (!revoke_process_caps(&g_driver_b)) ok = false;

    if (ok && !settle_assignment(&g_duplicate)) ok = false;
    if (ok && !settle_assignment(&g_assignment)) ok = false;

    if (process_storage_in_use(&g_driver_a) && !process_destroy(&g_driver_a)) ok = false;
    if (process_storage_in_use(&g_driver_b) && !process_destroy(&g_driver_b)) ok = false;

    if (ok) {
        g_driver_a_cap = CAPABILITY_INVALID_HANDLE;
        g_driver_b_cap = CAPABILITY_INVALID_HANDLE;
    }
    return ok;
}

void pci_assignment_test_cleanup_run(void) {
    terminal_writeln("PCI ASSIGNMENT CLEANUP RETRY:");
    report("ASSIGNMENT / DRIVER OBJECTS RELEASED", cleanup_all());
}

void pci_assignment_test_run(void) {
    terminal_writeln("PCI FUNCTION ASSIGNMENT LIFECYCLE TEST:");

    if (!cleanup_all()) {
        report("NO RETAINED ASSIGNMENT STATE", false);
        return;
    }

    PciAssignmentBaseline baseline;
    k_memset(&baseline, 0, sizeof(baseline));
    if (!capture_baseline(&baseline)) {
        report("RESOURCE BASELINE CAPTURED", false);
        return;
    }

    const PciDevice *device = pci_device(0U);
    bool discovered = pci_device_count() != 0U && device &&
        device->vendor_id != 0xFFFFU && device->address.device < 32U &&
        device->address.function < 8U;
    report("ENUMERATED PCI FUNCTION AVAILABLE", discovered);
    if (!discovered) goto fail;

    PciDevice forged = *device;
    bool forged_rejected = !pci_assignment_create(&g_duplicate, &forged) &&
        !pci_assignment_valid(&g_duplicate) &&
        pci_assignment_object_count() == baseline.assignments;
    report("NON-CANONICAL PCI DESCRIPTOR REJECTED", forged_rejected);
    if (!forged_rejected) goto fail;

    bool created = pci_assignment_create(&g_assignment, device) &&
        pci_assignment_valid(&g_assignment) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        !pci_assignment_owner_process_id(&g_assignment) &&
        pci_assignment_device(&g_assignment) &&
        pci_assignment_resource(&g_assignment) &&
        pci_assignment_resource(&g_assignment)->kind == DEVICE_RESOURCE_PCI_FUNCTION &&
        pci_assignment_object_count() == baseline.assignments + 1U &&
        device_resource_object_count() == baseline.device_resources + 1U;
    report("BOUNDED PCI FUNCTION RESOURCE CREATED", created);
    if (!created) goto fail;

    bool duplicate_rejected = !pci_assignment_create(&g_duplicate, device) &&
        !pci_assignment_valid(&g_duplicate) &&
        pci_assignment_object_count() == baseline.assignments + 1U;
    report("DUPLICATE BDF ASSIGNMENT REJECTED", duplicate_rejected);
    if (!duplicate_rejected) goto fail;

    bool processes = process_create(&g_driver_a) && process_create(&g_driver_b) &&
        !g_driver_a.kernel && !g_driver_b.kernel && g_driver_a.id && g_driver_b.id &&
        g_driver_a.id != g_driver_b.id &&
        process_object_count() == baseline.processes + 2U &&
        address_space_object_count() == baseline.address_spaces + 2U &&
        capability_table_object_count() == baseline.capability_tables + 2U;
    report("TWO DRIVER DOMAINS CREATED", processes);
    if (!processes) goto fail;

    CapabilityHandle rejected_handle = 0xFFFFFFFFFFFFFFFFULL;
    bool claimed = pci_assignment_claim(&g_assignment, &g_driver_a, &g_driver_a_cap) &&
        g_driver_a_cap != CAPABILITY_INVALID_HANDLE &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_ASSIGNED &&
        pci_assignment_owner_process_id(&g_assignment) == g_driver_a.id &&
        pci_assignment_authorized(&g_assignment, &g_driver_a, g_driver_a_cap) &&
        !pci_assignment_claim(&g_assignment, &g_driver_b, &rejected_handle) &&
        rejected_handle == CAPABILITY_INVALID_HANDLE &&
        !pci_assignment_begin_quiesce(&g_assignment, g_driver_b.id);
    report("EXCLUSIVE OWNER + MANAGE CAPABILITY", claimed);
    if (!claimed) goto fail;

    DeviceResource *resource = pci_assignment_resource(&g_assignment);
    void *resolved = 0;
    bool authority = resource &&
        capability_lookup_rights(process_capabilities(&g_driver_a), g_driver_a_cap,
            CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_MANAGE, &resolved) &&
        resolved == resource && pci_assignment_from_resource(resource) == &g_assignment &&
        !pci_assignment_authorized(&g_assignment, &g_driver_b, g_driver_a_cap);
    report("CAPABILITY AND OWNER BOTH REQUIRED", authority);
    if (!authority) goto fail;

    bool quiescing = pci_assignment_begin_quiesce(&g_assignment, g_driver_a.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUIESCING &&
        !pci_assignment_authorized(&g_assignment, &g_driver_a, g_driver_a_cap) &&
        !pci_assignment_complete_release(&g_assignment, g_driver_a.id);
    report("QUIESCE DISABLES USE / CAP PINS RELEASE", quiescing);
    if (!quiescing) goto fail;

    CapabilityHandle stale_a = g_driver_a_cap;
    bool revoked = capability_revoke(process_capabilities(&g_driver_a), g_driver_a_cap);
    resolved = (void *)1;
    bool stale_rejected = revoked &&
        !capability_lookup(process_capabilities(&g_driver_a), stale_a,
            CAPABILITY_TYPE_DEVICE_RESOURCE, &resolved) && resolved == 0 &&
        !pci_assignment_authorized(&g_assignment, &g_driver_a, stale_a);
    g_driver_a_cap = CAPABILITY_INVALID_HANDLE;
    report("REVOKED DRIVER HANDLE IS STALE", stale_rejected);
    if (!stale_rejected) goto fail;

    bool released = pci_assignment_complete_release(&g_assignment, g_driver_a.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        !pci_assignment_owner_process_id(&g_assignment);
    report("QUIESCED FUNCTION RETURNS AVAILABLE", released);
    if (!released) goto fail;

    bool replacement = pci_assignment_claim(&g_assignment, &g_driver_b, &g_driver_b_cap) &&
        g_driver_b_cap != CAPABILITY_INVALID_HANDLE && g_driver_b_cap != stale_a &&
        pci_assignment_owner_process_id(&g_assignment) == g_driver_b.id &&
        pci_assignment_authorized(&g_assignment, &g_driver_b, g_driver_b_cap) &&
        !pci_assignment_authorized(&g_assignment, &g_driver_a, stale_a);
    report("REPLACEMENT DRIVER GETS FRESH AUTHORITY", replacement);
    if (!replacement) goto fail;

    bool inherited = !pci_assignment_note_inherited_bus_master(&g_assignment, g_driver_a.id) &&
        !pci_assignment_bus_master_exposed(&g_assignment) &&
        pci_assignment_note_inherited_bus_master(&g_assignment, g_driver_b.id) &&
        pci_assignment_bus_master_exposed(&g_assignment) &&
        !pci_assignment_begin_quiesce(&g_assignment, g_driver_b.id) &&
        !pci_assignment_dma_gate_active(&g_assignment);
    report("INHERITED BUS-MASTER HISTORY / ORDINARY RELEASE REFUSED (MODEL)", inherited);
    if (!inherited) goto fail;

    bool quarantined = pci_assignment_quarantine(&g_assignment, g_driver_b.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        !pci_assignment_authorized(&g_assignment, &g_driver_b, g_driver_b_cap) &&
        !pci_assignment_reset_complete(&g_assignment, g_driver_b.id) &&
        !pci_assignment_reset_complete(&g_assignment, g_driver_a.id);
    report("QUARANTINE RETAINS UNSAFE FUNCTION", quarantined);
    if (!quarantined) goto fail;

    bool replacement_revoked = capability_revoke(process_capabilities(&g_driver_b),
        g_driver_b_cap);
    g_driver_b_cap = CAPABILITY_INVALID_HANDLE;
    bool reset_recovered = replacement_revoked &&
        pci_assignment_reset_complete(&g_assignment, g_driver_b.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        !pci_assignment_owner_process_id(&g_assignment);
    report("RESET COMPLETION RELEASES QUARANTINE", reset_recovered);
    if (!reset_recovered) goto fail;

    bool destroyed = pci_assignment_destroy(&g_assignment) &&
        process_destroy(&g_driver_a) && process_destroy(&g_driver_b) &&
        baseline_matches(&baseline);
    report("FULL ASSIGNMENT BASELINE RESTORED", destroyed);
    if (!destroyed) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI FUNCTION ASSIGNMENT LIFECYCLE TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    {
        bool cleaned = cleanup_all();
        report("FAILURE CLEANUP / ASSIGNMENT BASELINE", cleaned && baseline_matches(&baseline));
        terminal_set_color(terminal_error_color());
        terminal_writeln("PCI FUNCTION ASSIGNMENT LIFECYCLE TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
