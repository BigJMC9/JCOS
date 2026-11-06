/* Actual probe/decoder/report path; simulated PCI config and local CPU/UART.
 * Link supplies no assignment, map, register-load or PCI-write operation. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/tests/polaris_mmio_fixture.h"
#include "../../kernel/polaris_read_profile.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "read-probe line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static BootInfo boot;
static PciDevice selected, other, duplicate;
static u8 config[PCI_CONFIG_SPACE_SIZE];
static bool cpu = true, uart = true, firmware = true, duplicate_gop;
static u32 reads, wrong_reads;
static char output[4096];
static u32 length;
bool pci_address_valid(PciAddress a) { return a.device < 32U && a.function < 8U; }
u32 pci_device_count(void) { return duplicate_gop ? 3U : 2U; }
const PciDevice *pci_device(u32 i) { return i == 0 ? &other : i == 1 ? &selected : i == 2 ? &duplicate : 0; }
bool pci_device_is_enumerated(const PciDevice *p) { return p == &selected || p == &other || p == &duplicate; }
u8 pci_config_read8(PciAddress a, u16 offset) {
    ++reads;
    if (a.bus != selected.address.bus || a.device != selected.address.device ||
        a.function != selected.address.function || offset >= PCI_CONFIG_SPACE_SIZE) {
        ++wrong_reads; return 0xFFU;
    }
    return config[offset];
}
u16 pci_config_read16(PciAddress a, u16 o) { return (u16)pci_config_read8(a, o) | ((u16)pci_config_read8(a, o+1U) << 8); }
u32 pci_config_read32(PciAddress a, u16 o) { return (u32)pci_config_read16(a, o) | ((u32)pci_config_read16(a, o+2U) << 16); }
bool vmm_device_memory_supported(void) { return cpu; }
bool serial_available(void) { return uart; }
bool framebuffer_firmware_valid(void) { return firmware; }
void terminal_putchar(char c) { if (length+1U < sizeof(output)) { output[length++] = c; output[length] = 0; } }
void terminal_write(const char *s) { while (*s) terminal_putchar(*s++); }
void terminal_writeln(const char *s) { terminal_write(s); terminal_putchar('\n'); }
void terminal_write_u64(u64 value) { char s[32]; snprintf(s, sizeof(s), "%llu", (unsigned long long)value); terminal_write(s); }
void terminal_write_hex(u64 value) { char s[32]; snprintf(s, sizeof(s), "0x%llX", (unsigned long long)value); terminal_write(s); }
int main(void) {
    DisplayProbeInfo info;
    CHECK(polaris_mmio_fixture(&boot, &selected, &info, config));
    other = selected; other.address.bus = 3U; other.vendor_id = 0x10DEU; other.device_id = 0x1C82U;
    duplicate = selected;
    u8 saved[PCI_CONFIG_SPACE_SIZE]; memcpy(saved, config, sizeof(saved));
    CHECK(!polaris_read_profile_inspect(&boot, 0) && !reads);
    PolarisReadReport report;
    CHECK(polaris_read_profile_inspect(&boot, &report) && reads && !wrong_reads &&
        report.blockers == POLARIS_READ_UNQUALIFIED && report.bar_base == 0x63400000ULL &&
        report.bar_size == 0x40000ULL && report.page_count == 4U);
    polaris_read_profile_print(&boot);
    CHECK(strstr(output, "LOCAL COM1 AVAILABLE (NOT END-TO-END CONTROL): OBSERVED") &&
        strstr(output, "LIVE GPU READS: REFUSED; PHYSICAL PROVIDER UNAVAILABLE.") &&
        strstr(output, "30 ORDERED DWORD READS") &&
        strstr(output, "SELECTED PIPE / DISPLAY POWER DOMAIN: UNVERIFIED; PCI D0 ALONE IS INSUFFICIENT.") &&
        strstr(output, "NO ASSIGNMENT, MMIO MAPPING/ACCESS OR SCANOUT CHANGE PERFORMED."));
    CHECK(!memcmp(saved, config, sizeof(saved)));
    config[0x54] = 3U;
    CHECK(polaris_read_profile_inspect(&boot, &report) &&
        (report.blockers & POLARIS_READ_BLOCK_POWER) && (report.blockers & POLARIS_READ_BLOCK_BAR_PLAN));
    config[0x54] = 0U; config[0x52] = 0U;
    CHECK(polaris_read_profile_inspect(&boot, &report) && (report.blockers & POLARIS_READ_BLOCK_POWER));
    config[0x52] = 3U; cpu = uart = firmware = false;
    CHECK(polaris_read_profile_inspect(&boot, &report) &&
        (report.blockers & (POLARIS_READ_BLOCK_CPU | POLARIS_READ_BLOCK_UART | POLARIS_READ_BLOCK_SCANOUT)) ==
            (POLARIS_READ_BLOCK_CPU | POLARIS_READ_BLOCK_UART | POLARIS_READ_BLOCK_SCANOUT));
    duplicate_gop = true; reads = 0;
    CHECK(polaris_read_profile_inspect(&boot, &report) && !reads &&
        (report.blockers & POLARIS_READ_BLOCK_TARGET) && !report.bar_size);
    duplicate_gop = false; boot.framebuffer_pci_valid = 0U;
    CHECK(polaris_read_profile_inspect(&boot, &report) && !reads &&
        (report.blockers & POLARIS_READ_BLOCK_TARGET) && !report.bar_size);
    CHECK(!wrong_reads);
    puts("PASS: production readiness probe/report; selected PCI function only; PM/decode/CPU/UART/scanout refusals;"
         " local UART never qualifies end-to-end control; no config writes or device authority.");
    return 0;
}
