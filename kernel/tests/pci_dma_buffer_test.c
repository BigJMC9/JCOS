#include "pci_dma_buffer_test.h"

#include "address_space.h"
#include "capability.h"
#include "device_resource.h"
#include "lib.h"
#include "pci.h"
#include "pci_assignment.h"
#include "pci_dma_buffer.h"
#include "physmap.h"
#include "pmm.h"
#include "pmm_test.h"
#include "process.h"
#include "terminal.h"
#include "vmm.h"

#define DMA_TEST_PAGES 2U
#define DMA_TEST_VIRTUAL (ADDRESS_SPACE_USER_BASE + 0x24000000ULL)

static PciAssignment g_assignment;
static PciDmaBuffer g_buffer;
static Process g_driver;
static CapabilityHandle g_assignment_cap;
static CapabilityHandle g_buffer_cap;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 address_spaces;
    u32 capability_tables;
    u32 device_resources;
    u32 assignments;
    u32 dma_buffers;
} PciDmaBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciDmaBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->capability_tables = capability_table_object_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->dma_buffers = pci_dma_buffer_object_count();
    return true;
}

static bool baseline_matches(const PciDmaBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        capability_table_object_count() == baseline->capability_tables &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_dma_buffer_object_count() == baseline->dma_buffers;
}

static bool memory_is_zero(u64 physical, u64 size) {
    const u8 *data = (const u8 *)phys_to_virt(physical);
    if (!data || !size) return false;
    for (u64 i = 0ULL; i < size; ++i) if (data[i]) return false;
    return true;
}

static bool cleanup_all(void) {
    bool ok = true;
    u64 owner = pci_dma_buffer_valid(&g_buffer) ?
        pci_dma_buffer_owner_process_id(&g_buffer) :
        (pci_assignment_valid(&g_assignment) ?
            pci_assignment_owner_process_id(&g_assignment) : 0ULL);

    if (pci_dma_buffer_valid(&g_buffer)) {
        if (!pci_dma_buffer_force_unmap(&g_buffer, owner)) ok = false;
        PciDmaBufferState state = pci_dma_buffer_state(&g_buffer);
        if (state == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
            !pci_dma_buffer_force_quarantine(&g_buffer, owner)) ok = false;
        if (pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
            !pci_dma_buffer_reset_complete(&g_buffer, owner)) ok = false;
    }

    if (process_storage_in_use(&g_driver)) {
        CapabilityTable *caps = process_capabilities(&g_driver);
        if (!caps || !capability_revoke_all(caps)) ok = false;
    }
    g_assignment_cap = CAPABILITY_INVALID_HANDLE;
    g_buffer_cap = CAPABILITY_INVALID_HANDLE;

    if (pci_dma_buffer_valid(&g_buffer) && !pci_dma_buffer_destroy(&g_buffer)) ok = false;

    if (pci_assignment_valid(&g_assignment)) {
        PciAssignmentState state = pci_assignment_state(&g_assignment);
        owner = pci_assignment_owner_process_id(&g_assignment);
        if (state == PCI_ASSIGNMENT_ASSIGNED &&
            (!owner || !pci_assignment_begin_quiesce(&g_assignment, owner))) ok = false;
        state = pci_assignment_state(&g_assignment);
        if (state == PCI_ASSIGNMENT_QUIESCING &&
            (!owner || !pci_assignment_complete_release(&g_assignment, owner))) ok = false;
        else if (state == PCI_ASSIGNMENT_QUARANTINED &&
            (!owner || !pci_assignment_reset_complete(&g_assignment, owner))) ok = false;
        if (pci_assignment_valid(&g_assignment) && !pci_assignment_destroy(&g_assignment))
            ok = false;
    }

    if (process_storage_in_use(&g_driver) && !process_destroy(&g_driver)) ok = false;
    if (ok) {
        k_memset(&g_assignment, 0, sizeof(g_assignment));
        k_memset(&g_buffer, 0, sizeof(g_buffer));
        k_memset(&g_driver, 0, sizeof(g_driver));
    }
    return ok;
}

void pci_dma_buffer_test_cleanup_run(void) {
    terminal_writeln("PCI DMA BUFFER CLEANUP RETRY:");
    report("DMA / ASSIGNMENT BASELINE", cleanup_all());
}

void pci_dma_buffer_test_run(void) {
    terminal_writeln("PCI MEDIATED DMA BUFFER / QUARANTINE TEST:");

    bool cleaned = cleanup_all();
    report("CLEAN START", cleaned);
    if (!cleaned) return;

    PciDmaBaseline baseline;
    if (!capture_baseline(&baseline)) return;

    const PciDevice *device = pci_device_count() ? pci_device(0U) : 0;
    bool preflight = device && pci_device_is_enumerated(device);
    report("CANONICAL PCI FUNCTION AVAILABLE", preflight);
    if (!preflight) goto fail;

    bool setup = pci_assignment_create(&g_assignment, device) &&
        process_create(&g_driver) &&
        pci_assignment_claim(&g_assignment, &g_driver, &g_assignment_cap);
    report("DRIVER DOMAIN / FUNCTION ASSIGNMENT", setup);
    if (!setup) goto fail;

    u64 free_before_buffer = pmm_stats().free_pages;
    bool created = pci_dma_buffer_create(&g_buffer, &g_assignment, &g_driver,
        g_assignment_cap, DMA_TEST_PAGES, &g_buffer_cap) &&
        g_buffer_cap != CAPABILITY_INVALID_HANDLE &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_READY &&
        pci_assignment_child_count(&g_assignment) == 1U &&
        pmm_stats().free_pages + DMA_TEST_PAGES == free_before_buffer;
    report("CONTIGUOUS ZEROED DMA STORAGE / PARENT PIN", created);
    if (!created) goto fail;

    u64 physical = 0ULL;
    u64 size = 0ULL;
    frame_t first = FRAME_INVALID;
    bool span = pci_dma_buffer_device_span(&g_buffer, &physical, &size) &&
        size == (u64)DMA_TEST_PAGES * VM_PAGE_SIZE &&
        (physical & (VM_PAGE_SIZE - 1ULL)) == 0ULL &&
        (first = phys_to_frame(physical)) != FRAME_INVALID &&
        memory_is_zero(physical, size) &&
        pmm_test_frame_releasable(first) &&
        pmm_test_frame_releasable(first + DMA_TEST_PAGES - 1U);
    report("PINNED CONTIGUOUS PHYSICAL SPAN / ZERO BASELINE", span);
    if (!span) goto fail;

    CapabilityTable *caps = process_capabilities(&g_driver);
    void *object = 0;
    bool authority = caps &&
        capability_lookup_rights(caps, g_buffer_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE | CAPABILITY_RIGHT_MANAGE,
            &object) && object == &g_buffer.resource &&
        !capability_lookup_rights(caps, g_buffer_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_TRANSFER, &object);
    report("NON-TRANSFERABLE DMA BUFFER AUTHORITY", authority);
    if (!authority) goto fail;

    bool mapped = pci_dma_buffer_map(&g_buffer, &g_driver, g_buffer_cap,
        DMA_TEST_VIRTUAL, true);
    AddressSpace *space = process_address_space(&g_driver);
    bool mapping_exact = mapped && space;
    for (u32 i = 0U; mapping_exact && i < DMA_TEST_PAGES; ++i) {
        frame_t mapped_frame = FRAME_INVALID;
        vm_flags_t flags = 0ULL;
        mapping_exact = address_space_query_page(space,
            DMA_TEST_VIRTUAL + (u64)i * VM_PAGE_SIZE, &mapped_frame, &flags) &&
            mapped_frame == first + i && flags == (VM_USER | VM_WRITE);
    }
    report("OWNER-ONLY CACHED USER NX MAPPING", mapping_exact);
    if (!mapping_exact) goto fail;

    u8 *direct = (u8 *)phys_to_virt(physical);
    if (!direct) goto fail;
    direct[0] = 0xA5U;
    direct[size - 1ULL] = 0x5AU;

    bool active = pci_dma_buffer_device_begin(&g_buffer, &g_driver, g_buffer_cap) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
        pci_assignment_active_access_count(&g_assignment) == 1U &&
        !pci_assignment_begin_quiesce(&g_assignment, g_driver.id) &&
        !pci_assignment_quarantine(&g_assignment, g_driver.id);
    report("DEVICE ACCESS PIN BLOCKS ORDINARY QUIESCE / QUARANTINE", active);
    if (!active) goto fail;

    bool quarantined = pci_dma_buffer_force_quarantine(&g_buffer, g_driver.id) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_assignment_active_access_count(&g_assignment) == 1U &&
        !pci_dma_buffer_device_complete(&g_buffer, &g_driver, g_buffer_cap);
    report("TIMEOUT / DRIVER-DEATH PATH RETAINS DMA QUARANTINE", quarantined);
    if (!quarantined) goto fail;

    bool revoked = capability_revoke(caps, g_buffer_cap) &&
        capability_revoke(caps, g_assignment_cap);
    g_buffer_cap = CAPABILITY_INVALID_HANDLE;
    g_assignment_cap = CAPABILITY_INVALID_HANDLE;
    bool crash_cleanup = revoked && pci_dma_buffer_force_unmap(&g_buffer, g_driver.id) &&
        !pci_dma_buffer_destroy(&g_buffer) &&
        !pci_assignment_reset_complete(&g_assignment, g_driver.id) &&
        pmm_test_frame_releasable(first) &&
        pmm_test_frame_releasable(first + DMA_TEST_PAGES - 1U);
    report("CAPABILITY LOSS DOES NOT FREE POSSIBLY DMA-REACHABLE PAGES", crash_cleanup);
    if (!crash_cleanup) goto fail;

    bool reset = pci_dma_buffer_reset_complete(&g_buffer, g_driver.id) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_READY &&
        pci_assignment_active_access_count(&g_assignment) == 0U &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        memory_is_zero(physical, size);
    report("PROVEN RESET CLEARS BUFFER / RELEASES ACTIVE PIN", reset);
    if (!reset) goto fail;

    bool release = pci_dma_buffer_destroy(&g_buffer) &&
        pci_assignment_child_count(&g_assignment) == 0U &&
        !pmm_test_frame_releasable(first) &&
        !pmm_test_frame_releasable(first + DMA_TEST_PAGES - 1U) &&
        pci_assignment_reset_complete(&g_assignment, g_driver.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        pci_assignment_destroy(&g_assignment) && process_destroy(&g_driver) &&
        baseline_matches(&baseline);
    report("RESET -> BUFFER REUSE GATE -> FULL BASELINE", release);
    if (!release) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI MEDIATED DMA BUFFER / QUARANTINE TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup_all() && baseline_matches(&baseline));
    terminal_set_color(terminal_error_color());
    terminal_writeln("PCI MEDIATED DMA BUFFER / QUARANTINE TEST: FAILED");
    terminal_set_color(terminal_default_color());
}
