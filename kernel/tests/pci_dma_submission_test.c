#include "pci_dma_submission_test.h"

#include "address_space.h"
#include "capability.h"
#include "device_resource.h"
#include "lib.h"
#include "pci.h"
#include "pci_assignment.h"
#include "pci_bar_resource.h"
#include "pci_dma_buffer.h"
#include "pci_dma_submission.h"
#include "physmap.h"
#include "pmm.h"
#include "process.h"
#include "terminal.h"
#include "vmm.h"

#define DMA_SUBMISSION_TEST_PAGES 2U
#define DMA_SUBMISSION_TEST_VIRTUAL (ADDRESS_SPACE_USER_BASE + 0x26000000ULL)
#define PCI_COMMAND_OFFSET 0x04U

static PciAssignment g_assignment;
static PciDmaBuffer g_buffer;
static PciDmaSubmission g_submission;
static PciDmaSubmission g_second_submission;
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
    u32 dma_submissions;
} PciDmaSubmissionBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciDmaSubmissionBaseline *out) {
    if (!out) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->capability_tables = capability_table_object_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->dma_buffers = pci_dma_buffer_object_count();
    out->dma_submissions = pci_dma_submission_object_count();
    return true;
}

static bool baseline_matches(const PciDmaSubmissionBaseline *baseline) {
    return baseline && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        capability_table_object_count() == baseline->capability_tables &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_dma_buffer_object_count() == baseline->dma_buffers &&
        pci_dma_submission_object_count() == baseline->dma_submissions;
}

static bool revoke_if_live(CapabilityHandle *handle) {
    if (!handle || *handle == CAPABILITY_INVALID_HANDLE) return true;
    CapabilityTable *caps = process_storage_in_use(&g_driver) ?
        process_capabilities(&g_driver) : 0;
    if (!caps || !capability_revoke(caps, *handle)) return false;
    *handle = CAPABILITY_INVALID_HANDLE;
    return true;
}

static bool settle_submission(PciDmaSubmission *submission) {
    if (!pci_dma_submission_valid(submission)) return true;
    PciDmaSubmissionState state = pci_dma_submission_state(submission);
    if (state == PCI_DMA_SUBMISSION_ACTIVE) {
        if (!pci_dma_submission_force_quarantine(submission)) return false;
        state = PCI_DMA_SUBMISSION_QUARANTINED;
    }
    return state == PCI_DMA_SUBMISSION_QUARANTINED &&
        pci_dma_submission_reset_complete(submission);
}

static bool settle_assignment(void) {
    if (!pci_assignment_valid(&g_assignment)) return true;
    u64 owner = pci_assignment_owner_process_id(&g_assignment);
    if (!owner) return false;

    PciAssignmentState state = pci_assignment_state(&g_assignment);
    if (state == PCI_ASSIGNMENT_ASSIGNED) {
        if (pci_assignment_active_access_count(&g_assignment) ||
            pci_assignment_dma_gate_active(&g_assignment) ||
            !pci_assignment_begin_quiesce(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_QUIESCING;
    }
    if (state == PCI_ASSIGNMENT_QUIESCING) {
        if (!pci_assignment_complete_release(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    if (state == PCI_ASSIGNMENT_QUARANTINED) {
        if (pci_assignment_active_access_count(&g_assignment) ||
            pci_assignment_dma_gate_active(&g_assignment) ||
            !pci_assignment_reset_complete(&g_assignment, owner)) return false;
        state = PCI_ASSIGNMENT_AVAILABLE;
    }
    return state == PCI_ASSIGNMENT_AVAILABLE && pci_assignment_destroy(&g_assignment);
}

static bool cleanup_all(void) {
    bool ok = settle_submission(&g_second_submission) && settle_submission(&g_submission);

    if (pci_dma_buffer_valid(&g_buffer)) {
        u64 owner = pci_dma_buffer_owner_process_id(&g_buffer);
        if (!pci_dma_buffer_force_unmap(&g_buffer, owner)) ok = false;
        PciDmaBufferState state = pci_dma_buffer_state(&g_buffer);
        if (state == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
            !pci_dma_buffer_force_quarantine(&g_buffer, owner)) ok = false;
        if (pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
            !pci_dma_buffer_reset_complete(&g_buffer, owner)) ok = false;
    }

    if (!revoke_if_live(&g_buffer_cap)) ok = false;
    if (pci_dma_buffer_valid(&g_buffer) && !pci_dma_buffer_destroy(&g_buffer)) ok = false;
    if (!revoke_if_live(&g_assignment_cap)) ok = false;
    if (pci_assignment_valid(&g_assignment) && !settle_assignment()) ok = false;
    if (process_storage_in_use(&g_driver) && !process_destroy(&g_driver)) ok = false;
    return ok;
}

void pci_dma_submission_test_cleanup_run(void) {
    terminal_writeln("PCI DMA SUBMISSION / EXCLUSION CLEANUP:");
    report("SUBMISSION / BUFFER / ASSIGNMENT BASELINE", cleanup_all());
}

void pci_dma_submission_test_run(void) {
    terminal_writeln("PCI MEDIATED DMA SUBMISSION / EXCLUSION TEST:");

    bool clean = cleanup_all();
    report("CLEAN START", clean);
    if (!clean) return;

    PciDmaSubmissionBaseline baseline;
    if (!capture_baseline(&baseline)) return;

    const PciDevice *device = pci_device_count() ? pci_device(0U) : 0;
    bool preflight = device && pci_device_is_enumerated(device);
    report("CANONICAL PCI FUNCTION AVAILABLE", preflight);
    if (!preflight) goto fail;

    u16 command_before = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    bool setup = pci_assignment_create(&g_assignment, device) &&
        process_create(&g_driver) &&
        pci_assignment_claim(&g_assignment, &g_driver, &g_assignment_cap) &&
        pci_dma_buffer_create(&g_buffer, &g_assignment, &g_driver,
            g_assignment_cap, DMA_SUBMISSION_TEST_PAGES, &g_buffer_cap);
    report("DRIVER / FUNCTION / DMA BUFFER SETUP", setup);
    if (!setup) goto fail;

    bool mapped = pci_dma_buffer_map(&g_buffer, &g_driver, g_buffer_cap,
        DMA_SUBMISSION_TEST_VIRTUAL, true);
    bool mapped_denied = mapped &&
        !pci_dma_submission_begin(&g_submission, &g_assignment, &g_driver,
            g_assignment_cap, &g_buffer, g_buffer_cap, 0ULL, 128ULL,
            PCI_DMA_TO_DEVICE) &&
        !pci_assignment_dma_gate_active(&g_assignment) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_READY &&
        pci_dma_buffer_unmap(&g_buffer, &g_driver, g_buffer_cap);
    report("CPU-MAPPED BUFFER CANNOT ENTER DMA SUBMISSION", mapped_denied);
    if (!mapped_denied) goto fail;

    u64 buffer_base = 0ULL;
    u64 buffer_size = 0ULL;
    bool span = pci_dma_buffer_device_span(&g_buffer, &buffer_base, &buffer_size) &&
        buffer_size == (u64)DMA_SUBMISSION_TEST_PAGES * VM_PAGE_SIZE;
    report("BOUNDED DMA BUFFER SPAN", span);
    if (!span) goto fail;

    bool invalid_range = !pci_dma_submission_begin(&g_submission, &g_assignment,
        &g_driver, g_assignment_cap, &g_buffer, g_buffer_cap, buffer_size, 1ULL,
        PCI_DMA_TO_DEVICE) &&
        !pci_dma_submission_begin(&g_submission, &g_assignment, &g_driver,
            g_assignment_cap, &g_buffer, g_buffer_cap, 0ULL, buffer_size + 1ULL,
            PCI_DMA_FROM_DEVICE) &&
        !pci_dma_submission_begin(&g_submission, &g_assignment, &g_driver,
            g_assignment_cap, &g_buffer, g_buffer_cap, 0ULL, 64ULL,
            PCI_DMA_DIRECTION_INVALID) &&
        !pci_assignment_dma_gate_active(&g_assignment);
    report("INVALID DMA WINDOW / DIRECTION REJECTED", invalid_range);
    if (!invalid_range) goto fail;

    const u64 offset = 128ULL;
    const u64 length = 512ULL;
    bool active = pci_dma_submission_begin(&g_submission, &g_assignment, &g_driver,
        g_assignment_cap, &g_buffer, g_buffer_cap, offset, length,
        PCI_DMA_TO_DEVICE) &&
        pci_dma_submission_state(&g_submission) == PCI_DMA_SUBMISSION_ACTIVE &&
        pci_dma_submission_direction(&g_submission) == PCI_DMA_TO_DEVICE &&
        pci_assignment_dma_gate_active(&g_assignment) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
        pci_assignment_active_access_count(&g_assignment) == 1U &&
        pci_bar_resource_assignment_active_access_count(&g_assignment) == 0U &&
        !pci_assignment_begin_quiesce(&g_assignment, g_driver.id) &&
        !pci_dma_buffer_map(&g_buffer, &g_driver, g_buffer_cap,
            DMA_SUBMISSION_TEST_VIRTUAL, true);
    report("DMA SUBMISSION HOLDS BUFFER + ASSIGNMENT EXCLUSION", active);
    if (!active) goto fail;

    u64 device_base = 0ULL;
    u64 device_size = 0ULL;
    bool exact_span = pci_dma_submission_device_span(&g_submission,
            &device_base, &device_size) &&
        device_base == buffer_base + offset && device_size == length &&
        !pci_dma_submission_begin(&g_second_submission, &g_assignment, &g_driver,
            g_assignment_cap, &g_buffer, g_buffer_cap, 0ULL, 64ULL,
            PCI_DMA_FROM_DEVICE);
    report("EXACT KERNEL DMA WINDOW / SINGLE SUBMISSION GATE", exact_span);
    if (!exact_span) goto fail;

    bool completed = pci_dma_submission_complete(&g_submission, &g_driver,
            g_buffer_cap) &&
        !pci_dma_submission_valid(&g_submission) &&
        !pci_assignment_dma_gate_active(&g_assignment) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_READY &&
        pci_assignment_active_access_count(&g_assignment) == 0U;
    report("NORMAL COMPLETION RELEASES DMA EXCLUSION", completed);
    if (!completed) goto fail;

    u8 *direct = (u8 *)phys_to_virt(buffer_base);
    if (!direct) goto fail;
    direct[0] = 0xA5U;

    bool quarantined = pci_dma_submission_begin(&g_submission, &g_assignment,
            &g_driver, g_assignment_cap, &g_buffer, g_buffer_cap, 0ULL,
            buffer_size, PCI_DMA_BIDIRECTIONAL) &&
        pci_dma_submission_force_quarantine(&g_submission) &&
        pci_dma_submission_state(&g_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_assignment_dma_gate_active(&g_assignment) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
        pci_assignment_active_access_count(&g_assignment) == 1U &&
        !pci_dma_submission_complete(&g_submission, &g_driver, g_buffer_cap);
    report("CRASH PATH RETAINS SUBMISSION / BUFFER QUARANTINE", quarantined);
    if (!quarantined) goto fail;

    CapabilityTable *caps = process_capabilities(&g_driver);
    bool revoked = caps && capability_revoke(caps, g_buffer_cap) &&
        capability_revoke(caps, g_assignment_cap);
    if (revoked) {
        g_buffer_cap = CAPABILITY_INVALID_HANDLE;
        g_assignment_cap = CAPABILITY_INVALID_HANDLE;
    }
    bool reset = revoked && pci_dma_submission_reset_complete(&g_submission) &&
        !pci_dma_submission_valid(&g_submission) &&
        !pci_assignment_dma_gate_active(&g_assignment) &&
        pci_dma_buffer_state(&g_buffer) == PCI_DMA_BUFFER_READY &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_assignment_active_access_count(&g_assignment) == 0U && direct[0] == 0U;
    report("CAPABILITY LOSS -> RESET PROOF -> DMA GATE RELEASE", reset);
    if (!reset) goto fail;

    u16 command_after = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    bool command_untouched = command_after == command_before;
    report("GENERIC MEDIATOR DOES NOT MODIFY PCI COMMAND / BUS MASTER", command_untouched);
    if (!command_untouched) goto fail;

    bool release = pci_dma_buffer_destroy(&g_buffer) &&
        pci_assignment_child_count(&g_assignment) == 0U &&
        pci_assignment_reset_complete(&g_assignment, g_driver.id) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        pci_assignment_destroy(&g_assignment) && process_destroy(&g_driver) &&
        baseline_matches(&baseline);
    report("DMA SUBMISSION / QUARANTINE FULL BASELINE", release);
    if (!release) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI MEDIATED DMA SUBMISSION / EXCLUSION TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup_all() && baseline_matches(&baseline));
    terminal_set_color(terminal_error_color());
    terminal_writeln("PCI MEDIATED DMA SUBMISSION / EXCLUSION TEST: FAILED");
    terminal_set_color(terminal_default_color());
}
