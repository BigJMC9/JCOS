#include "polaris_read_profile.h"
#include "framebuffer.h"
#include "lib.h"
#include "serial.h"
#include "terminal.h"
bool polaris_read_profile_evaluate(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u16 pmc, u16 pmcsr, bool cpu_ready,
    bool uart_available, bool firmware_valid, PolarisReadReport *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    out->version = POLARIS_READ_REPORT_VERSION;
    out->blockers = POLARIS_READ_UNQUALIFIED;
    bool target = device && info && display_probe_matches_gop(boot, device) &&
        device->vendor_id == 0x1002U && device->device_id == 0x67DFU &&
        device->revision == 0xEFU && info->vendor_id == device->vendor_id &&
        info->device_id == device->device_id && info->revision == device->revision &&
        info->subsystem_vendor_id == 0x1458U && info->subsystem_device_id == 0x2310U;
    if (!target) out->blockers |= POLARIS_READ_BLOCK_TARGET;
    if (device) out->address = device->address;
    out->pmc = pmc; out->pmcsr = pmcsr;
    if (info) out->command = info->command;
    bool power = target && info->pm_offset >= 0x40U && info->pm_offset <= 0xF8U &&
        !(info->pm_offset & 3U) && pmc != 0xFFFFU && (pmc & 7U) >= 1U && (pmc & 7U) <= 3U &&
        pmcsr != 0xFFFFU && !(pmcsr & 3U) && (info->command & PCI_COMMAND_MEMORY_SPACE);
    if (!power) out->blockers |= POLARIS_READ_BLOCK_POWER;
    PolarisMmioPlan plan;
    if (target && polaris_mmio_plan(boot, device, info, pmcsr, &plan)) {
        out->bar_base = plan.bar.base; out->bar_size = plan.bar.size; out->page_count = plan.page_count;
    } else out->blockers |= POLARIS_READ_BLOCK_BAR_PLAN;
    if (!cpu_ready) out->blockers |= POLARIS_READ_BLOCK_CPU;
    if (!uart_available) out->blockers |= POLARIS_READ_BLOCK_UART;
    if (!firmware_valid) out->blockers |= POLARIS_READ_BLOCK_SCANOUT;
    return true; /* Report available; blockers, never true, express readiness. */
}
bool polaris_read_profile_inspect(const BootInfo *boot, PolarisReadReport *out) {
    if (!out) return false;
    const PciDevice *selected = 0;
    for (u32 i = 0; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!display_probe_matches_gop(boot, device)) continue;
        if (selected) return polaris_read_profile_evaluate(boot, 0, 0, 0xFFFFU, 0xFFFFU,
            vmm_device_memory_supported(), serial_available(), framebuffer_firmware_valid(), out);
        selected = device;
    }
    DisplayProbeInfo info;
    bool read = selected && display_probe_read(selected, &info);
    u16 pmc = 0xFFFFU, pmcsr = 0xFFFFU;
    if (read && info.pm_offset >= 0x40U && info.pm_offset <= 0xF8U && !(info.pm_offset & 3U)) {
        pmc = pci_config_read16(selected->address, (u16)(info.pm_offset + 2U));
        pmcsr = pci_config_read16(selected->address, (u16)(info.pm_offset + 4U));
    }
    return polaris_read_profile_evaluate(boot, selected, read ? &info : 0, pmc, pmcsr,
        vmm_device_memory_supported(), serial_available(), framebuffer_firmware_valid(), out);
}
static void condition(const char *label, bool blocked) {
    terminal_write("  "); terminal_write(label);
    terminal_writeln(blocked ? ": UNVERIFIED / BLOCKED" : ": OBSERVED");
}
void polaris_read_profile_print(const BootInfo *boot) {
    PolarisReadReport report;
    if (!polaris_read_profile_inspect(boot, &report)) return;
    terminal_writeln("POLARIS READ PROFILE V2: QUALIFICATION REPORT ONLY");
    terminal_writeln("CANDIDATE: 1002:67DF REV EF / SUBSYSTEM 1458:2310 / DCE11.2");
    terminal_writeln("PROPOSED CAPTURE: ONE PIPE, TWO SAMPLES, 30 ORDERED DWORD READS, NO RETRY.");
    terminal_writeln("SOFTWARE ADMISSION: AT MOST TWO SECONDS; NOT A BUS-TRANSACTION TIMEOUT.");
    condition("EXACT SELECTED GOP / BOARD CANDIDATE", report.blockers & POLARIS_READ_BLOCK_TARGET);
    condition("COMPLETE BAR5 EXTENT / FOUR-PAGE PLAN", report.blockers & POLARIS_READ_BLOCK_BAR_PLAN);
    condition("STANDARD PM / D0 / MEMORY DECODE", report.blockers & POLARIS_READ_BLOCK_POWER);
    condition("KERNEL RO/NX/UC CPU PRECONDITIONS", report.blockers & POLARIS_READ_BLOCK_CPU);
    condition("LOCAL COM1 AVAILABLE (NOT END-TO-END CONTROL)", report.blockers & POLARIS_READ_BLOCK_UART);
    condition("FIRMWARE SCANOUT ELIGIBLE", report.blockers & POLARIS_READ_BLOCK_SCANOUT);
    terminal_write("  PCI COMMAND="); terminal_write_hex(report.command);
    terminal_write(" PMC="); terminal_write_hex(report.pmc);
    terminal_write(" PMCSR="); terminal_write_hex(report.pmcsr); terminal_putchar('\n');
    terminal_write("  BAR5 BASE="); terminal_write_hex(report.bar_base);
    terminal_write(" BYTES="); terminal_write_hex(report.bar_size);
    terminal_write(" PAGES="); terminal_write_u64(report.page_count); terminal_putchar('\n');
    terminal_writeln("SELECTED PIPE / DISPLAY POWER DOMAIN: UNVERIFIED; PCI D0 ALONE IS INSUFFICIENT.");
    terminal_writeln("REQUIRED: REGISTER-READ SAFETY REVIEW, INDEPENDENT SERIAL CONTROL, PHYSICAL EVIDENCE.");
    terminal_writeln("LIVE GPU READS: REFUSED; PHYSICAL PROVIDER UNAVAILABLE.");
    terminal_writeln("NO ASSIGNMENT, MMIO MAPPING/ACCESS OR SCANOUT CHANGE PERFORMED.");
}
