#include "display_lease_test.h"

#include "display.h"
#include "framebuffer.h"
#include "lib.h"
#include "process.h"
#include "terminal.h"
#include "vmm.h"

#define DISPLAY_TEST_USER_BASE 0x0000022000000000ULL

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool cleanup_lease(void) {
    DisplayLeaseState state = display_direct_state();
    if (state == DISPLAY_LEASE_KERNEL) return true;
    if (state == DISPLAY_LEASE_HANDOFF) return display_direct_abort();
    return false;
}

void display_lease_test_cleanup_run(void) {
    terminal_writeln("DISPLAY LEASE CLEANUP RETRY:");
    report("KERNEL FRAMEBUFFER OWNERSHIP RESTORED", cleanup_lease());
}

void display_lease_test_run(void) {
    terminal_writeln("DISPLAY FRAMEBUFFER LEASE TEST:");

    bool render_before = terminal_render_enabled();
    bool preflight = cleanup_lease() && display_available() &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL;
    report("KERNEL OWNERSHIP BASELINE", preflight);
    if (!preflight) goto fail;

    FramebufferInfo before;
    k_memset(&before, 0, sizeof(before));
    bool descriptor = framebuffer_info(&before) && before.physical_base && before.size &&
        before.mapping_physical_base && before.mapping_size &&
        before.page_offset == (before.physical_base & (VM_PAGE_SIZE - 1ULL)) &&
        before.mapping_physical_base == before.physical_base - before.page_offset &&
        before.mapping_size >= before.size + before.page_offset &&
        !(before.mapping_size & (VM_PAGE_SIZE - 1ULL)) &&
        before.width == framebuffer_width() && before.height == framebuffer_height() &&
        before.pixels_per_scanline >= before.width;
    report("VALIDATED GOP RESOURCE DESCRIPTOR", descriptor);
    if (!descriptor) goto fail;

    if (!before.direct_map_safe) {
        ProgramBorrowedMappingSpec rejected_mapping;
        FramebufferInfo rejected_info;
        u64 rejected_pointer = 0ULL;
        k_memset(&rejected_mapping, 0, sizeof(rejected_mapping));
        k_memset(&rejected_info, 0, sizeof(rejected_info));
        bool fallback = !display_direct_prepare(DISPLAY_TEST_USER_BASE,
            &rejected_mapping, &rejected_info, &rejected_pointer) &&
            display_direct_state() == DISPLAY_LEASE_KERNEL &&
            terminal_render_enabled() == render_before;
        report("NON-MMIO GOP RANGE / SAFE KERNEL FALLBACK", fallback);
        if (!fallback) goto fail;

        terminal_set_color(terminal_accent_color());
        terminal_writeln("DISPLAY FRAMEBUFFER LEASE TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

    ProgramBorrowedMappingSpec mapping;
    FramebufferInfo prepared_info;
    u64 user_framebuffer = 0ULL;
    k_memset(&mapping, 0, sizeof(mapping));
    k_memset(&prepared_info, 0, sizeof(prepared_info));

    bool prepared = display_direct_prepare(DISPLAY_TEST_USER_BASE,
        &mapping, &prepared_info, &user_framebuffer);
    bool handoff = prepared && display_direct_state() == DISPLAY_LEASE_HANDOFF &&
        display_direct_owner_process_id() == 0ULL && !terminal_render_enabled();
    report("KERNEL -> HANDOFF / FRAMEBUFFER QUIET", handoff);
    if (!handoff) goto fail;

    bool map_contract = mapping.physical_base == before.physical_base &&
        mapping.size == before.size && mapping.virtual_base == DISPLAY_TEST_USER_BASE &&
        mapping.flags == (VM_WRITE | VM_UNCACHED) &&
        user_framebuffer == DISPLAY_TEST_USER_BASE + before.page_offset &&
        prepared_info.physical_base == before.physical_base &&
        prepared_info.mapping_physical_base == before.mapping_physical_base &&
        prepared_info.mapping_size == before.mapping_size;
    report("BORROWED GOP MAP / RW-NX / UNCACHED", map_contract);
    if (!map_contract) goto fail;

    /* A kernel Process is never a valid Ring3 display owner. Failed commit must
     * retain HANDOFF so the caller can explicitly abort the transaction. */
    bool invalid_owner = !display_direct_commit(process_kernel()) &&
        display_direct_state() == DISPLAY_LEASE_HANDOFF;
    report("INVALID DIRECT OWNER REJECTED", invalid_owner);
    if (!invalid_owner) goto fail;

    bool aborted = display_direct_abort() &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL &&
        terminal_render_enabled() == render_before;
    report("HANDOFF ABORT / TERMINAL RESTORED", aborted);
    if (!aborted) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("DISPLAY FRAMEBUFFER LEASE TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    {
        bool cleaned = cleanup_lease();
        report("FAILURE CLEANUP / KERNEL OWNERSHIP", cleaned);
        terminal_set_color(terminal_error_color());
        terminal_writeln("DISPLAY FRAMEBUFFER LEASE TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}