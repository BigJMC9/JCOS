#include "pci_bar_resource_test.h"

#include "address_space.h"
#include "capability.h"
#include "device_resource.h"
#include "lib.h"
#include "pci.h"
#include "pci_assignment.h"
#include "pci_bar_resource.h"
#include "pmm.h"
#include "process.h"
#include "terminal.h"

static PciAssignment g_assignment;
static PciBarResource g_bar_resource;
static PciBarResource g_duplicate;
static Process g_driver;
static CapabilityHandle g_assignment_cap;
static CapabilityHandle g_bar_cap;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 address_spaces;
    u32 capability_tables;
    u32 device_resources;
    u32 assignments;
    u32 bar_resources;
} PciBarResourceBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciBarResourceBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->capability_tables = capability_table_object_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->bar_resources = pci_bar_resource_object_count();
    return true;
}

static bool baseline_matches(const PciBarResourceBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        capability_table_object_count() == baseline->capability_tables &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_bar_resource_object_count() == baseline->bar_resources;
}

static bool find_implemented_bar(const PciDevice **out_device, u8 *out_index,
    PciBar *out_bar) {
    if (!out_device || !out_index || !out_bar) return false;
    *out_device = 0;
    *out_index = 0U;
    k_memset(out_bar, 0, sizeof(*out_bar));

    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || (device->header_type & 0x7FU) != 0U) continue;
        for (u8 index = 0U; index < PCI_STANDARD_BAR_COUNT; ++index) {
            PciBar bar;
            k_memset(&bar, 0, sizeof(bar));
            if (!pci_read_bar(device, index, &bar) || bar.type == PCI_BAR_NONE) continue;
            *out_device = device;
            *out_index = index;
            *out_bar = bar;
            return true;
        }
    }
    return false;
}

static bool settle_assignment(void) {
    if (!pci_assignment_valid(&g_assignment)) return true;
    u64 owner = pci_assignment_owner_process_id(&g_assignment);
    PciAssignmentState state = pci_assignment_state(&g_assignment);
    if (state == PCI_ASSIGNMENT_ASSIGNED) {
        if (!owner || !pci_assignment_begin_quiesce(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_QUIESCING;
    }
    if (state == PCI_ASSIGNMENT_QUIESCING) {
        if (!owner || !pci_assignment_complete_release(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    if (state == PCI_ASSIGNMENT_QUARANTINED) {
        if (!owner || !pci_assignment_reset_complete(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    return state == PCI_ASSIGNMENT_AVAILABLE && pci_assignment_destroy(&g_assignment);
}

static bool cleanup_all(void) {
    bool ok = true;
    if (process_storage_in_use(&g_driver)) {
        CapabilityTable *caps = process_capabilities(&g_driver);
        if (!caps || !capability_revoke_all(caps)) ok = false;
    }
    if (pci_bar_resource_valid(&g_duplicate) && !pci_bar_resource_destroy(&g_duplicate))
        ok = false;
    if (pci_bar_resource_valid(&g_bar_resource) && !pci_bar_resource_destroy(&g_bar_resource))
        ok = false;
    if (ok && !settle_assignment()) ok = false;
    if (process_storage_in_use(&g_driver) && !process_destroy(&g_driver)) ok = false;
    if (ok) {
        g_assignment_cap = CAPABILITY_INVALID_HANDLE;
        g_bar_cap = CAPABILITY_INVALID_HANDLE;
        k_memset(&g_assignment, 0, sizeof(g_assignment));
        k_memset(&g_bar_resource, 0, sizeof(g_bar_resource));
        k_memset(&g_duplicate, 0, sizeof(g_duplicate));
        k_memset(&g_driver, 0, sizeof(g_driver));
    }
    return ok;
}

void pci_bar_resource_test_cleanup_run(void) {
    terminal_writeln("PCI BAR RESOURCE CLEANUP RETRY:");
    report("RESOURCE / ASSIGNMENT BASELINE", cleanup_all());
}

void pci_bar_resource_test_run(void) {
    terminal_writeln("PCI BAR RESOURCE AUTHORITY / LIFETIME TEST:");

    bool cleaned = cleanup_all();
    report("CLEAN START", cleaned);
    if (!cleaned) return;

    PciBarResourceBaseline baseline;
    if (!capture_baseline(&baseline)) return;

    const PciDevice *device = 0;
    u8 bar_index = 0U;
    PciBar source_bar;
    bool found = find_implemented_bar(&device, &bar_index, &source_bar);
    report("CANONICAL IMPLEMENTED BAR AVAILABLE", found);
    if (!found) goto fail;

    bool setup = pci_assignment_create(&g_assignment, device) &&
        process_create(&g_driver) &&
        pci_assignment_claim(&g_assignment, &g_driver, &g_assignment_cap) &&
        pci_bar_resource_create(&g_bar_resource, &g_assignment, &g_driver,
            g_assignment_cap, bar_index, &g_bar_cap);
    report("ASSIGNMENT -> BAR RESOURCE DERIVATION", setup);
    if (!setup) goto fail;

    PciBar descriptor;
    k_memset(&descriptor, 0, sizeof(descriptor));
    bool identity = pci_bar_resource_descriptor(&g_bar_resource, &descriptor) &&
        descriptor.type == source_bar.type && descriptor.index == source_bar.index &&
        descriptor.base == source_bar.base &&
        descriptor.prefetchable == source_bar.prefetchable &&
        pci_bar_resource_owner_process_id(&g_bar_resource) == g_driver.id &&
        pci_assignment_child_count(&g_assignment) == 1U;
    report("BAR IDENTITY / PARENT PIN", identity);
    if (!identity) goto fail;

    CapabilityTable *caps = process_capabilities(&g_driver);
    void *forbidden = 0;
    bool authority = caps &&
        pci_bar_resource_authorized(&g_bar_resource, &g_driver, g_bar_cap,
            CAPABILITY_RIGHT_READ) &&
        pci_bar_resource_authorized(&g_bar_resource, &g_driver, g_bar_cap,
            CAPABILITY_RIGHT_WRITE | CAPABILITY_RIGHT_MANAGE) &&
        !capability_lookup_rights(caps, g_bar_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_TRANSFER, &forbidden);
    report("NON-TRANSFERABLE READ / WRITE / MANAGE AUTHORITY", authority);
    if (!authority) goto fail;

    CapabilityHandle duplicate_cap = CAPABILITY_INVALID_HANDLE;
    bool duplicate_rejected = !pci_bar_resource_create(&g_duplicate, &g_assignment,
        &g_driver, g_assignment_cap, bar_index, &duplicate_cap) &&
        duplicate_cap == CAPABILITY_INVALID_HANDLE &&
        !pci_bar_resource_valid(&g_duplicate) &&
        pci_assignment_child_count(&g_assignment) == 1U;
    report("DUPLICATE BAR AUTHORITY REJECTED", duplicate_rejected);
    if (!duplicate_rejected) goto fail;

    bool bounded = pci_bar_resource_bounded(&g_bar_resource);
    bool geometry = false;
    if (bounded) {
        geometry = descriptor.size && !(descriptor.size & (descriptor.size - 1ULL)) &&
            (!descriptor.base || !(descriptor.base & (descriptor.size - 1ULL))) &&
            pci_bar_resource_contains(&g_bar_resource, 0ULL, 1ULL) &&
            pci_bar_resource_contains(&g_bar_resource, descriptor.size - 1ULL, 1ULL) &&
            !pci_bar_resource_contains(&g_bar_resource, descriptor.size, 1ULL) &&
            !pci_bar_resource_contains(&g_bar_resource, descriptor.size - 1ULL, 2ULL) &&
            !pci_bar_resource_contains(&g_bar_resource, 0ULL, 0ULL);
    } else {
        geometry = descriptor.size == 0ULL &&
            !pci_bar_resource_contains(&g_bar_resource, 0ULL, 1ULL);
    }
    report("EXACT BOUNDS OR ACCESS-DISABLED DESCRIPTOR", geometry);
    if (!geometry) goto fail;

    u64 owner = g_driver.id;
    bool quiesced = pci_assignment_begin_quiesce(&g_assignment, owner) &&
        !pci_bar_resource_authorized(&g_bar_resource, &g_driver, g_bar_cap,
            CAPABILITY_RIGHT_READ);
    report("QUIESCE IMMEDIATELY REVOKES BAR USE", quiesced);
    if (!quiesced) goto fail;

    bool child_pins_release = capability_revoke(caps, g_bar_cap) &&
        capability_revoke(caps, g_assignment_cap) &&
        !pci_assignment_complete_release(&g_assignment, owner) &&
        pci_assignment_child_count(&g_assignment) == 1U;
    report("CHILD OBJECT PINS FUNCTION RELEASE", child_pins_release);
    if (!child_pins_release) goto fail;
    g_bar_cap = CAPABILITY_INVALID_HANDLE;
    g_assignment_cap = CAPABILITY_INVALID_HANDLE;

    bool child_destroyed = pci_bar_resource_destroy(&g_bar_resource) &&
        pci_assignment_child_count(&g_assignment) == 0U &&
        pci_assignment_complete_release(&g_assignment, owner) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE;
    report("CHILD DESTROY -> FUNCTION RELEASE", child_destroyed);
    if (!child_destroyed) goto fail;

    bool restored = pci_assignment_destroy(&g_assignment) && process_destroy(&g_driver) &&
        baseline_matches(&baseline);
    report("FULL RESOURCE BASELINE RESTORED", restored);
    if (!restored) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI BAR RESOURCE AUTHORITY / LIFETIME TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup_all() && baseline_matches(&baseline));
    terminal_set_color(terminal_error_color());
    terminal_writeln("PCI BAR RESOURCE AUTHORITY / LIFETIME TEST: FAILED");
    terminal_set_color(terminal_default_color());
}
