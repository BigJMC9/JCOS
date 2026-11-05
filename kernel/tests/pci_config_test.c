#include "pci_config_test.h"

#include "lib.h"
#include "pci.h"
#include "terminal.h"

#define PCI_TEST_COMMAND_OFFSET 0x04U
#define PCI_TEST_BAR0_OFFSET    0x10U

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool power_of_two(u64 value) {
    return value && !(value & (value - 1ULL));
}

static bool bar_decode_vectors(void) {
    u64 size = 0ULL;
    if (!pci_bar_decode_size(PCI_BAR_MMIO32, 0xFFFFF000U, 0U, &size) ||
        size != 0x1000ULL) return false;
    if (!pci_bar_decode_size(PCI_BAR_IO, 0xFFFFFFE1U, 0U, &size) ||
        size != 0x20ULL) return false;
    if (!pci_bar_decode_size(PCI_BAR_MMIO64, 0xFFFF000CU, 0xFFFFFFFFU, &size) ||
        size != 0x10000ULL) return false;
    if (pci_bar_decode_size(PCI_BAR_NONE, 0xFFFFFFFFU, 0xFFFFFFFFU, &size) || size)
        return false;
    if (pci_bar_decode_size(PCI_BAR_MMIO32, 0U, 0U, &size) || size)
        return false;
    return true;
}

static bool physical_probe_contract(void) {
    bool inspected = false;

    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || (device->header_type & 0x7FU) != 0U) continue;

        u16 command_before = pci_config_read16(device->address, PCI_TEST_COMMAND_OFFSET);
        for (u8 index = 0U; index < PCI_STANDARD_BAR_COUNT; ++index) {
            PciBar descriptor;
            k_memset(&descriptor, 0, sizeof(descriptor));
            if (!pci_read_bar(device, index, &descriptor)) continue;
            if (descriptor.type == PCI_BAR_NONE) continue;

            inspected = true;
            u16 offset = PCI_TEST_BAR0_OFFSET + (u16)index * 4U;
            u32 low_before = pci_config_read32(device->address, offset);
            u32 high_before = descriptor.type == PCI_BAR_MMIO64 ?
                pci_config_read32(device->address, offset + 4U) : 0U;

            PciBar probed;
            k_memset(&probed, 0, sizeof(probed));
            bool active = (command_before & (PCI_COMMAND_IO_SPACE |
                PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)) != 0U;
            bool probe_result = pci_probe_bar(device, index, &probed);

            u16 command_after = pci_config_read16(device->address, PCI_TEST_COMMAND_OFFSET);
            u32 low_after = pci_config_read32(device->address, offset);
            u32 high_after = descriptor.type == PCI_BAR_MMIO64 ?
                pci_config_read32(device->address, offset + 4U) : 0U;
            if (command_after != command_before || low_after != low_before ||
                high_after != high_before) return false;

            if (active) {
                if (probe_result) return false;
            } else if (probe_result) {
                if (probed.type != descriptor.type || probed.base != descriptor.base ||
                    probed.index != index || !power_of_two(probed.size) ||
                    (probed.base && (probed.base & (probed.size - 1ULL)))) return false;
            }
        }
    }

    /* A platform without a type-zero implemented BAR still exercises the pure
     * decoder and all invalid-access guards in this test. */
    return inspected || pci_device_count() != 0U;
}

void pci_config_test_run(void) {
    terminal_writeln("PCI CONFIG TRANSACTION / BAR BOUNDS TEST:");

    const PciDevice *device = pci_device(0U);
    bool enumerated = pci_device_count() != 0U && device &&
        pci_device_is_enumerated(device) && pci_address_valid(device->address);
    report("CANONICAL ENUMERATED FUNCTION", enumerated);
    if (!enumerated) return;

    PciAddress bad_device = device->address;
    bad_device.device = 32U;
    PciAddress bad_function = device->address;
    bad_function.function = 8U;
    bool bounds = !pci_address_valid(bad_device) && !pci_address_valid(bad_function) &&
        pci_config_read8(bad_device, 0U) == 0xFFU &&
        pci_config_read16(device->address, 0xFFU) == 0xFFFFU &&
        pci_config_read32(device->address, 2U) == 0xFFFFFFFFU;
    report("INVALID BDF / CONFIG RANGES REJECTED", bounds);
    if (!bounds) return;

    PciDevice forged = *device;
    PciBar forged_bar;
    k_memset(&forged_bar, 0, sizeof(forged_bar));
    bool canonical = !pci_device_is_enumerated(&forged) &&
        !pci_read_bar(&forged, 0U, &forged_bar);
    report("FORGED DEVICE DESCRIPTOR REJECTED", canonical);
    if (!canonical) return;

    bool decode = bar_decode_vectors();
    report("BAR SIZE MASK DECODING", decode);
    if (!decode) return;

    bool probe = physical_probe_contract();
    report("ACTIVE BAR PROBE REFUSED / DISABLED BAR RESTORED", probe);
    if (!probe) return;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI CONFIG TRANSACTION / BAR BOUNDS TEST: PASS");
    terminal_set_color(terminal_default_color());
}
