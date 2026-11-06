#include "polaris_read_profile_test.h"
#include "polaris_read_profile.h"
#include "polaris_mmio_fixture.h"
#include "terminal.h"
bool polaris_capture_model_test(void) {
    PolarisCaptureBudget b;
    for (u32 pipe = 0; pipe < POLARIS_DISPLAY_PIPE_COUNT; ++pipe) {
        if (!polaris_capture_budget_init(&b, pipe, 100U, 10ULL, 200ULL)) return false;
        for (u32 i = 0; i < POLARIS_CAPTURE_READ_LIMIT; ++i)
            if (!polaris_capture_budget_admit(&b, pipe, i % POLARIS_DISPLAY_REGISTER_COUNT,
                    100U, 10ULL + i) || b.admitted != i + 1U) return false;
        if (!b.closed || polaris_capture_budget_admit(&b, pipe, 0U, 100U, 40ULL)) return false;
    }
    for (u32 boundary = 0; boundary < POLARIS_CAPTURE_READ_LIMIT; ++boundary) {
        if (!polaris_capture_budget_init(&b, 3U, 100U, 50ULL, 100ULL)) return false;
        for (u32 i = 0; i < boundary; ++i)
            if (!polaris_capture_budget_admit(&b, 3U, i % POLARIS_DISPLAY_REGISTER_COUNT, 100U, 50ULL))
                return false;
        if (polaris_capture_budget_admit(&b, 3U, boundary % POLARIS_DISPLAY_REGISTER_COUNT,
                100U, 150ULL) || !b.closed || b.admitted != boundary ||
            polaris_capture_budget_admit(&b, 3U, boundary % POLARIS_DISPLAY_REGISTER_COUNT,
                100U, 50ULL)) return false; /* A backwards tick cannot revive a closed budget. */
    }
    if (!polaris_capture_budget_init(&b, 0U, 100U, ~0ULL - 9ULL, 20ULL) ||
        !polaris_capture_budget_admit(&b, 0U, 0U, 100U, 9ULL) ||
        polaris_capture_budget_admit(&b, 0U, 1U, 100U, 10ULL)) return false;
    const u32 bad_fields[] = {1U, 14U, 15U, 0xFFFFFFFFU};
    for (u32 i = 0; i < ARRAY_COUNT(bad_fields); ++i) {
        if (!polaris_capture_budget_init(&b, 0U, 100U, 0, 200ULL) ||
            polaris_capture_budget_admit(&b, 0U, bad_fields[i], 100U, 0) || !b.closed || b.admitted)
            return false;
    }
    if (!polaris_capture_budget_init(&b, 0U, 100U, 0, 200ULL) ||
        polaris_capture_budget_admit(&b, 1U, 0U, 100U, 0) || !b.closed) return false;
    if (!polaris_capture_budget_init(&b, 0U, 100U, 0, 200ULL) ||
        polaris_capture_budget_admit(&b, 0U, 0U, 101U, 0) || !b.closed) return false;
    if (!polaris_capture_budget_init(&b, 0U, 100U, 0, 200ULL)) return false;
    polaris_capture_budget_close(&b);
    if (polaris_capture_budget_admit(&b, 0U, 0U, 100U, 0)) return false;
    if (polaris_capture_budget_init(&b, 6U, 100U, 0, 200ULL) || !b.closed ||
        polaris_capture_budget_init(&b, 0U, 0U, 0, 1ULL) ||
        polaris_capture_budget_init(&b, 0U, 100U, 0, 0ULL) ||
        polaris_capture_budget_init(&b, 0U, 100U, 0, 201ULL) ||
        polaris_capture_budget_init(0, 0U, 100U, 0, 1ULL) ||
        polaris_capture_budget_admit(0, 0U, 0U, 100U, 0)) return false;
    return true;
}
bool polaris_read_profile_model_test(void) {
    BootInfo boot; PciDevice device; DisplayProbeInfo info; u8 config[PCI_CONFIG_SPACE_SIZE];
    PolarisReadReport report;
    if (!polaris_mmio_fixture(&boot, &device, &info, config) ||
        !polaris_read_profile_evaluate(&boot, &device, &info, 3U, 0U, true, true, true, &report) ||
        report.version != POLARIS_READ_REPORT_VERSION || report.blockers != POLARIS_READ_UNQUALIFIED ||
        report.bar_base != 0x63400000ULL || report.bar_size != 0x40000ULL || report.page_count != 4U)
        return false;
    for (u32 bits = 0; bits < 8U; ++bits) {
        if (!polaris_read_profile_evaluate(&boot, &device, &info, 3U, 0U,
                bits & 1U, bits & 2U, bits & 4U, &report) ||
            (report.blockers & POLARIS_READ_UNQUALIFIED) != POLARIS_READ_UNQUALIFIED ||
            !!(report.blockers & POLARIS_READ_BLOCK_CPU) == !!(bits & 1U) ||
            !!(report.blockers & POLARIS_READ_BLOCK_UART) == !!(bits & 2U) ||
            !!(report.blockers & POLARIS_READ_BLOCK_SCANOUT) == !!(bits & 4U)) return false;
    }
    for (u16 power = 1U; power <= 3U; ++power) {
        if (!polaris_read_profile_evaluate(&boot, &device, &info, 3U, power, true, true, true, &report) ||
            !(report.blockers & POLARIS_READ_BLOCK_POWER) ||
            !(report.blockers & POLARIS_READ_BLOCK_BAR_PLAN)) return false;
    }
    boot.framebuffer_bar_queries[3].prefix[3] = 0x41U;
    if (!polaris_read_profile_evaluate(&boot, &device, &info, 3U, 0U, true, true, true, &report) ||
        !(report.blockers & POLARIS_READ_BLOCK_BAR_PLAN)) return false;
    boot.framebuffer_bar_queries[3].prefix[3] = 0U;
    device.device_id++;
    if (!polaris_read_profile_evaluate(&boot, &device, &info, 3U, 0U, true, true, true, &report) ||
        !(report.blockers & POLARIS_READ_BLOCK_TARGET)) return false;
    return polaris_read_profile_evaluate(0, 0, 0, 0xFFFFU, 0xFFFFU, false, false, false, &report) &&
        (report.blockers & 0x3FU) == 0x3FU &&
        (report.blockers & POLARIS_READ_UNQUALIFIED) == POLARIS_READ_UNQUALIFIED &&
        !polaris_read_profile_evaluate(&boot, &device, &info, 3U, 0U, true, true, true, 0);
}
void polaris_read_profile_test_run(void) {
    bool budget = polaris_capture_model_test();
    bool profile = polaris_read_profile_model_test();
    terminal_writeln("POLARIS READ PROFILE / CAPTURE POLICY TEST:");
    terminal_writeln(budget ? "  THIRTY-READ ORDER / DEADLINES / WRAP / PERMANENT CLOSE: PASS" :
        "  THIRTY-READ ORDER / DEADLINES / WRAP / PERMANENT CLOSE: FAILED");
    terminal_writeln(profile ? "  ALL OBSERVATIONS CANNOT REMOVE PHYSICAL QUALIFICATION BLOCKERS: PASS" :
        "  ALL OBSERVATIONS CANNOT REMOVE PHYSICAL QUALIFICATION BLOCKERS: FAILED");
    terminal_writeln(budget && profile ? "POLARIS READ PROFILE / CAPTURE POLICY TEST: PASS" :
        "POLARIS READ PROFILE / CAPTURE POLICY TEST: FAILED");
}
