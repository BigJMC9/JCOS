#include "pci_bar_access_test.h"

#include "address_space.h"
#include "capability.h"
#include "device_resource.h"
#include "lib.h"
#include "pci.h"
#include "pci_assignment.h"
#include "pci_bar_access.h"
#include "pci_bar_resource.h"
#include "pmm.h"
#include "process.h"
#include "terminal.h"
#include "vmm.h"

#define PCI_BAR_ACCESS_TEST_VA (ADDRESS_SPACE_USER_BASE + 0x02000000ULL)

static PciAssignment g_assignment;
static PciBarResource g_resource;
static PciBarMmioMapping g_mapping;
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
    u32 mmio_mappings;
} PciBarAccessBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciBarAccessBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->capability_tables = capability_table_object_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->bar_resources = pci_bar_resource_object_count();
    out->mmio_mappings = pci_bar_mmio_mapping_object_count();
    return true;
}

static bool baseline_matches(const PciBarAccessBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        capability_table_object_count() == baseline->capability_tables &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_bar_resource_object_count() == baseline->bar_resources &&
        pci_bar_mmio_mapping_object_count() == baseline->mmio_mappings;
}

static bool candidate(const PciDevice **out_device, u8 *out_index) {
    if (!out_device || !out_index) return false;
    *out_device = 0;
    *out_index = 0U;

    const PciDevice *fallback = 0;
    u8 fallback_index = 0U;
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || (device->header_type & 0x7FU) != 0U) continue;
        for (u8 index = 0U; index < PCI_STANDARD_BAR_COUNT; ++index) {
            PciBar bar;
            k_memset(&bar, 0, sizeof(bar));
            if (!pci_read_bar(device, index, &bar) || bar.type == PCI_BAR_NONE) continue;
            if (!fallback) {
                fallback = device;
                fallback_index = index;
            }

            PciBar probed;
            k_memset(&probed, 0, sizeof(probed));
            bool mmio = bar.type == PCI_BAR_MMIO32 || bar.type == PCI_BAR_MMIO64;
            if (mmio && pci_probe_bar(device, index, &probed) &&
                probed.size >= VM_PAGE_SIZE && !(probed.base & (VM_PAGE_SIZE - 1ULL))) {
                *out_device = device;
                *out_index = index;
                return true;
            }
        }
    }

    if (!fallback) return false;
    *out_device = fallback;
    *out_index = fallback_index;
    return true;
}

static bool settle_assignment(void) {
    if (!pci_assignment_valid(&g_assignment)) return true;
    u64 owner = pci_assignment_owner_process_id(&g_assignment);
    PciAssignmentState state = pci_assignment_state(&g_assignment);
    if (state == PCI_ASSIGNMENT_ASSIGNED) {
        if (pci_assignment_active_access_count(&g_assignment)) return false;
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
    if (pci_bar_mmio_mapping_valid(&g_mapping) && !pci_bar_mmio_unmap(&g_mapping)) ok = false;
    if (pci_bar_resource_valid(&g_resource)) {
        u64 owner = pci_bar_resource_owner_process_id(&g_resource);
        while (owner && pci_bar_resource_active_access_count(&g_resource)) {
            if (!pci_bar_resource_access_release(&g_resource, owner)) { ok = false; break; }
        }
    }
    if (process_storage_in_use(&g_driver)) {
        CapabilityTable *caps = process_capabilities(&g_driver);
        if (!caps || !capability_revoke_all(caps)) ok = false;
    }
    if (pci_bar_resource_valid(&g_resource) && !pci_bar_resource_destroy(&g_resource)) ok = false;
    if (ok && !settle_assignment()) ok = false;
    if (process_storage_in_use(&g_driver) && !process_destroy(&g_driver)) ok = false;
    if (ok) {
        g_assignment_cap = CAPABILITY_INVALID_HANDLE;
        g_bar_cap = CAPABILITY_INVALID_HANDLE;
        k_memset(&g_assignment, 0, sizeof(g_assignment));
        k_memset(&g_resource, 0, sizeof(g_resource));
        k_memset(&g_mapping, 0, sizeof(g_mapping));
        k_memset(&g_driver, 0, sizeof(g_driver));
    }
    return ok;
}

void pci_bar_access_test_cleanup_run(void) {
    terminal_writeln("PCI BAR ACCESS CLEANUP RETRY:");
    report("ACCESS / RESOURCE / ASSIGNMENT BASELINE", cleanup_all());
}

void pci_bar_access_test_run(void) {
    terminal_writeln("PCI BAR MMIO / MEDIATED PIO AUTHORITY TEST:");

    bool cleaned = cleanup_all();
    report("CLEAN START", cleaned);
    if (!cleaned) return;

    PciBarAccessBaseline baseline;
    if (!capture_baseline(&baseline)) return;

    const PciDevice *device = 0;
    u8 bar_index = 0U;
    bool found = candidate(&device, &bar_index);
    report("CANONICAL PCI BAR AVAILABLE", found);
    if (!found) goto fail;

    bool setup = pci_assignment_create(&g_assignment, device) &&
        process_create(&g_driver) &&
        pci_assignment_claim(&g_assignment, &g_driver, &g_assignment_cap) &&
        pci_bar_resource_create(&g_resource, &g_assignment, &g_driver,
            g_assignment_cap, bar_index, &g_bar_cap);
    report("ASSIGNMENT / BAR ACCESS AUTHORITY", setup);
    if (!setup) goto fail;

    PciBar descriptor;
    k_memset(&descriptor, 0, sizeof(descriptor));
    if (!pci_bar_resource_descriptor(&g_resource, &descriptor)) goto fail;
    bool bounded = pci_bar_resource_bounded(&g_resource);

    bool pin_contract = false;
    if (bounded) {
        u64 owner = g_driver.id;
        pin_contract = pci_bar_resource_access_acquire(&g_resource, &g_driver, g_bar_cap,
                CAPABILITY_RIGHT_READ) &&
            pci_bar_resource_active_access_count(&g_resource) == 1U &&
            pci_assignment_active_access_count(&g_assignment) == 1U &&
            !pci_assignment_begin_quiesce(&g_assignment, owner) &&
            !pci_assignment_quarantine(&g_assignment, owner) &&
            pci_bar_resource_access_release(&g_resource, owner) &&
            !pci_bar_resource_active_access_count(&g_resource) &&
            !pci_assignment_active_access_count(&g_assignment);
    } else {
        pin_contract = !pci_bar_resource_access_acquire(&g_resource, &g_driver, g_bar_cap,
                CAPABILITY_RIGHT_READ) &&
            !pci_bar_resource_active_access_count(&g_resource) &&
            !pci_assignment_active_access_count(&g_assignment);
    }
    report("ACTIVE ACCESS PINS ASSIGNMENT STATE", pin_contract);
    if (!pin_contract) goto fail;

    bool mapping_contract = true;
    bool mappable = pci_bar_mmio_range_mappable(&g_resource, 0ULL, VM_PAGE_SIZE);
    if (mappable) {
        bool mapped = pci_bar_mmio_map(&g_mapping, &g_resource, &g_driver, g_bar_cap,
            0ULL, VM_PAGE_SIZE, PCI_BAR_ACCESS_TEST_VA, true);
        AddressSpace *space = process_address_space(&g_driver);
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        bool page = mapped && space && address_space_query_page(space,
            PCI_BAR_ACCESS_TEST_VA, &frame, &flags) &&
            frame == phys_to_frame(descriptor.base) &&
            flags == (VM_USER | VM_WRITE | VM_UNCACHED);
        mapping_contract = page && pci_bar_mmio_mapping_object_count() ==
                baseline.mmio_mappings + 1U &&
            pci_bar_resource_active_access_count(&g_resource) == 1U &&
            pci_assignment_active_access_count(&g_assignment) == 1U &&
            !pci_assignment_begin_quiesce(&g_assignment, g_driver.id) &&
            !pci_bar_resource_destroy(&g_resource) &&
            pci_bar_mmio_unmap(&g_mapping) &&
            !address_space_query_page(space, PCI_BAR_ACCESS_TEST_VA, 0, 0) &&
            pci_bar_mmio_mapping_object_count() == baseline.mmio_mappings &&
            !pci_bar_resource_active_access_count(&g_resource) &&
            !pci_assignment_active_access_count(&g_assignment);
    } else {
        mapping_contract = !pci_bar_mmio_map(&g_mapping, &g_resource, &g_driver,
            g_bar_cap, 0ULL, VM_PAGE_SIZE, PCI_BAR_ACCESS_TEST_VA, true) &&
            !pci_bar_mmio_mapping_valid(&g_mapping) &&
            pci_bar_mmio_mapping_object_count() == baseline.mmio_mappings;
    }
    report("PAGE-EXACT MMIO MAP / UNMAP OR SAFE REFUSAL", mapping_contract);
    if (!mapping_contract) goto fail;

    bool pio_contract = true;
    u32 value = 0xA5A5A5A5U;
    if (descriptor.type == PCI_BAR_IO && bounded) {
        bool first_byte = descriptor.size && pci_bar_pio_range_valid(&g_resource, 0ULL, 1U);
        bool outside = !pci_bar_pio_range_valid(&g_resource, descriptor.size, 1U) &&
            !pci_bar_pio_read(&g_resource, &g_driver, g_bar_cap,
                descriptor.size, 1U, &value) && value == 0U &&
            !pci_bar_pio_write(&g_resource, &g_driver, g_bar_cap,
                descriptor.size, 1U, 0U);
        pio_contract = first_byte && outside;
    } else {
        pio_contract = !pci_bar_pio_range_valid(&g_resource, 0ULL, 1U) &&
            !pci_bar_pio_read(&g_resource, &g_driver, g_bar_cap, 0ULL, 1U, &value) &&
            value == 0U &&
            !pci_bar_pio_write(&g_resource, &g_driver, g_bar_cap, 0ULL, 1U, 0U);
    }
    report("PIO RANGE MEDIATION / NO DIRECT USER I/O", pio_contract);
    if (!pio_contract) goto fail;

    bool quiesced = !pci_assignment_active_access_count(&g_assignment) &&
        pci_assignment_begin_quiesce(&g_assignment, g_driver.id) &&
        !pci_bar_resource_authorized(&g_resource, &g_driver, g_bar_cap,
            CAPABILITY_RIGHT_READ) &&
        !pci_bar_mmio_map(&g_mapping, &g_resource, &g_driver, g_bar_cap,
            0ULL, VM_PAGE_SIZE, PCI_BAR_ACCESS_TEST_VA, false);
    report("QUIESCE BLOCKS NEW CPU DEVICE ACCESS", quiesced);
    if (!quiesced) goto fail;

    CapabilityTable *caps = process_capabilities(&g_driver);
    bool released = caps && capability_revoke(caps, g_bar_cap) &&
        capability_revoke(caps, g_assignment_cap) &&
        pci_bar_resource_destroy(&g_resource) &&
        pci_assignment_complete_release(&g_assignment, g_driver.id) &&
        pci_assignment_destroy(&g_assignment) && process_destroy(&g_driver) &&
        baseline_matches(&baseline);
    report("FULL ACCESS / RESOURCE BASELINE RESTORED", released);
    if (!released) goto fail;
    g_bar_cap = CAPABILITY_INVALID_HANDLE;
    g_assignment_cap = CAPABILITY_INVALID_HANDLE;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI BAR MMIO / MEDIATED PIO AUTHORITY TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup_all() && baseline_matches(&baseline));
    terminal_set_color(terminal_error_color());
    terminal_writeln("PCI BAR MMIO / MEDIATED PIO AUTHORITY TEST: FAILED");
    terminal_set_color(terminal_default_color());
}
