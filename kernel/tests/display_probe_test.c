#include "display_probe_test.h"
#include "polaris_mmio_plan_test.h"

#include "display.h"
#include "display_probe.h"
#include "../include/boot_display.h"
#include "lib.h"
#include "terminal.h"

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_writeln(pass ? ": PASS" : ": FAILED");
}

static void put16(u8 *config, u32 offset, u16 value) {
    config[offset] = (u8)value;
    config[offset + 1U] = (u8)(value >> 8);
}

static void put32(u8 *config, u32 offset, u32 value) {
    put16(config, offset, (u16)value);
    put16(config, offset + 2U, (u16)(value >> 16));
}

static void config_fixture(u8 *config) {
    k_memset(config, 0, PCI_CONFIG_SPACE_SIZE);
    put16(config, 0U, 0x1002U);
    put16(config, 2U, 0x67DFU);
    put16(config, 4U, PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    config[8U] = 0xEFU;
    config[0x0BU] = 3U;
    config[0x0EU] = 0x80U; /* multifunction type zero */
    put16(config, 0x2CU, 0x1DA2U);
    put16(config, 0x2EU, 0xE353U); /* synthetic, not a qualified board */
    put32(config, 0x10U, 0x0000000CU);
    put32(config, 0x14U, 0x00000012U);
    put32(config, 0x20U, 0x0000E001U);
    put32(config, 0x24U, 0xF0000000U);
}

static bool cleared(const DisplayProbeInfo *info) {
    const u8 *bytes = (const u8 *)info;
    for (u32 i = 0U; i < sizeof(*info); ++i)
        if (bytes[i]) return false;
    return true;
}

static bool rejected(const u8 *config, u32 size) {
    DisplayProbeInfo info;
    k_memset(&info, 0xA5, sizeof(info));
    return !display_probe_decode(config, size, &info) && cleared(&info);
}

static bool identity_and_bars(void) {
    u8 config[PCI_CONFIG_SPACE_SIZE];
    config_fixture(config);
    DisplayProbeInfo info;
    if (!display_probe_decode(config, sizeof(config), &info) || !info.polaris10_candidate ||
        info.subsystem_vendor_id != 0x1DA2U || info.subsystem_device_id != 0xE353U ||
        info.revision != 0xEFU || info.bars[0].type != PCI_BAR_MMIO64 ||
        info.bars[0].base != 0x1200000000ULL || !info.bars[0].prefetchable ||
        !info.bar_continuation[1] || info.bars[4].type != PCI_BAR_IO ||
        info.bars[4].base != 0xE000U || info.bars[5].base != 0xF0000000U) return false;
    for (u32 i = 0U; i < PCI_STANDARD_BAR_COUNT; ++i)
        if (info.bars[i].size) return false;
    put16(config, 2U, 0x67FFU);
    if (!display_probe_decode(config, sizeof(config), &info) || info.polaris10_candidate) return false;
    config_fixture(config);
    put16(config, 0U, 0x8086U);
    if (!display_probe_decode(config, sizeof(config), &info) || info.polaris10_candidate) return false;
    config_fixture(config);
    put32(config, 0x24U, 0xF0000004U);
    if (!rejected(config, sizeof(config))) return false; /* no room for high half */
    config_fixture(config);
    put32(config, 0x10U, 0xE0000002U);
    if (!rejected(config, sizeof(config))) return false; /* reserved BAR type */
    config_fixture(config);
    config[0x0BU] = 6U;
    if (!rejected(config, sizeof(config))) return false;
    config_fixture(config);
    config[0x0EU] = 1U;
    if (!rejected(config, sizeof(config))) return false;
    config_fixture(config);
    put16(config, 0U, 0xFFFFU);
    return rejected(config, sizeof(config)) && rejected(config, sizeof(config) - 1U) &&
        rejected(0, PCI_CONFIG_SPACE_SIZE) && !display_probe_decode(config, sizeof(config), 0);
}

static bool capability_vectors(void) {
    u8 config[PCI_CONFIG_SPACE_SIZE];
    config_fixture(config);
    put16(config, 6U, 1U << 4);
    config[0x34U] = 0x40U;
    config[0x40U] = 0x10U;
    config[0x41U] = 0x50U;
    put32(config, 0x44U, 1U << 28);
    config[0x50U] = 0x05U;
    config[0x51U] = 0x60U;
    put16(config, 0x52U, 1U);
    config[0x60U] = 0x11U;
    config[0x61U] = 0x70U;
    put16(config, 0x62U, 0x8000U);
    config[0x70U] = 0x01U;
    DisplayProbeInfo info;
    if (!display_probe_decode(config, sizeof(config), &info) || info.capability_count != 4U ||
        info.pcie_offset != 0x40U || !info.flr_advertised || info.pm_offset != 0x70U ||
        info.msi_control != 1U || info.msix_control != 0x8000U) return false;
    config[0x71U] = 0x40U;
    if (!rejected(config, sizeof(config))) return false; /* cycle after a known capability */
    config[0x71U] = 0U;
    config[0x41U] = 0x44U;
    if (!rejected(config, sizeof(config))) return false; /* overlapping prefix */
    config[0x41U] = 0x50U;
    config[0x71U] = 0x70U;
    if (!rejected(config, sizeof(config))) return false; /* self-loop */
    config[0x71U] = 0U;
    const u8 bad_pointers[] = {0U, 0x20U, 0x41U, 0xFFU};
    for (u32 i = 0U; i < sizeof(bad_pointers); ++i) {
        config[0x34U] = bad_pointers[i];
        if (!rejected(config, sizeof(config))) return false;
    }
    config[0x34U] = 0xFCU;
    config[0xFCU] = 0x10U;
    if (!rejected(config, sizeof(config))) return false; /* truncated PCIe */
    config[0xFCU] = 0x01U;
    if (!rejected(config, sizeof(config))) return false; /* truncated PM */
    config[0xFCU] = 0x11U;
    if (!rejected(config, sizeof(config))) return false; /* truncated MSI-X */
    config[0xFCU] = 0x09U;
    if (!display_probe_decode(config, sizeof(config), &info) || info.capability_count != 1U ||
        info.flr_advertised || info.pcie_offset) return false; /* unknown final prefix */
    config[0xFCU] = 0xFFU;
    return rejected(config, sizeof(config));
}

static bool firmware_identity(void) {
    BootInfo boot;
    PciDevice device;
    k_memset(&boot, 0, sizeof(boot));
    k_memset(&device, 0, sizeof(device));
    boot.magic = BOOT_INFO_MAGIC;
    boot.version = BOOT_INFO_VERSION;
    boot.size = sizeof(boot);
    device.class_code = 3U;
    device.vendor_id = 0x1002U;
    device.device_id = 0x67DFU;
    device.revision = 0xEFU;
    boot.framebuffer_pci_vendor_device = 0x67DF1002U;
    boot.framebuffer_pci_class_revision = 0x030000EFU;
    device.address = (PciAddress){1U, 0U, 0U};
    boot.framebuffer_pci_bus = 1U;
    if (display_probe_matches_gop(&boot, &device)) return false;
    boot.framebuffer_pci_valid = 1U;
    if (!display_probe_matches_gop(&boot, &device)) return false;
    boot.framebuffer_pci_segment = 1U;
    if (display_probe_matches_gop(&boot, &device)) return false;
    boot.framebuffer_pci_segment = 0U;
    boot.framebuffer_pci_function = 1U;
    if (display_probe_matches_gop(&boot, &device)) return false;
    boot.framebuffer_pci_function = 0U;
    boot.version--;
    if (display_probe_matches_gop(&boot, &device)) return false;
    boot.version++;
    boot.size--;
    return !display_probe_matches_gop(&boot, &device) &&
        !display_probe_matches_gop(0, &device) && !display_probe_matches_gop(&boot, 0);
}

static void put64(u8 *bytes, u32 offset, u64 value) {
    put32(bytes, offset, (u32)value);
    put32(bytes, offset + 4U, (u32)(value >> 32U));
}

static bool firmware_resource_vectors(void) {
    u8 bytes[48];
    k_memset(bytes, 0, sizeof(bytes));
    bytes[0] = 0x8AU;
    bytes[1] = 43U;
    bytes[46] = 0x79U;
    put64(bytes, 6U, 32U);
    put64(bytes, 14U, 0x63400000ULL);
    put64(bytes, 22U, 0x3FFFFULL); /* alignment, deliberately not an endpoint */
    put64(bytes, 38U, 0x40000ULL); /* synthetic extent, not a hardware measurement */
    BootDisplayBar bar;
    if (!boot_display_decode_bar(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar) ||
        bar.base != 0x63400000ULL || bar.size != 0x40000ULL ||
        bar.flags != (BOOT_DISPLAY_BAR_VALID | BOOT_DISPLAY_BAR_MEMORY)) return false;
    if (boot_display_decode_bar(bytes, 47U, 0x63400000U, 0U, 5U, &bar) || bar.flags ||
        boot_display_decode_bar(bytes, sizeof(bytes), 0x63000000U, 0U, 5U, &bar)) return false;
    put64(bytes, 30U, 0x1000ULL);
    if (boot_display_decode_bar(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar)) return false;
    put64(bytes, 30U, 0ULL);
    put64(bytes, 38U, 0ULL);
    if (boot_display_decode_bar(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar)) return false;
    put64(bytes, 38U, 0x30000ULL);
    if (boot_display_decode_bar(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar)) return false;
    put64(bytes, 6U, 64U);
    put64(bytes, 14U, 0x400000000ULL);
    put64(bytes, 38U, 0x10000000ULL);
    if (!boot_display_decode_bar(bytes, sizeof(bytes), 0xCU, 4U, 0U, &bar) ||
        bar.base != 0x400000000ULL || !(bar.flags & BOOT_DISPLAY_BAR_64BIT) ||
        boot_display_decode_bar(bytes, sizeof(bytes), 0xCU, 4U, 5U, &bar)) return false;
    put64(bytes, 14U, 0xFFFFFFFFFFFFF000ULL);
    put64(bytes, 38U, 0x2000ULL);
    if (boot_display_decode_bar(bytes, sizeof(bytes), 0xFFFFF004U, 0xFFFFFFFFU, 0U, &bar))
        return false;
    bytes[0] = 0U;
    return !boot_display_decode_bar(bytes, sizeof(bytes), 0xCU, 4U, 0U, &bar) &&
        !bar.flags && !bar.base && !bar.size;
}

static bool firmware_resource_list_vectors(void) {
    u8 bytes[94];
    k_memset(bytes, 0, sizeof(bytes));
    for (u32 i = 0; i < 2U; ++i) {
        u8 *part = bytes + i * 46U;
        part[0] = 0x8AU;
        part[1] = 43U;
        put64(part, 6U, 32U);
        put64(part, 14U, 0x63400000ULL + i * 0x20000ULL);
        put64(part, 38U, 0x20000ULL);
    }
    bytes[92] = 0x79U;
    BootDisplayBar bar;
    BootDisplayBarAudit audit;
    if (!boot_display_decode_bar_audited(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar, &audit) ||
        bar.size != 0x40000ULL || audit.result != BOOT_DISPLAY_BAR_ACCEPTED ||
        audit.descriptor_count != 2U || audit.reported_base != 0x63420000ULL) return false;
    put64(bytes + 46U, 14U, 0x63430000ULL);
    if (boot_display_decode_bar_audited(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar, &audit) ||
        audit.result != BOOT_DISPLAY_BAR_NONCONTIGUOUS || bar.flags || bar.size || bar.base)
        return false;
    put64(bytes + 46U, 14U, 0x63420000ULL);
    put64(bytes + 46U, 30U, 0x1000ULL);
    return !boot_display_decode_bar_audited(bytes, sizeof(bytes), 0x63400000U, 0U, 5U, &bar, &audit) &&
        audit.result == BOOT_DISPLAY_BAR_TRANSLATED && audit.translation == 0x1000ULL &&
        audit.descriptor_count == 2U && !bar.flags && !bar.size && !bar.base;
}

static bool query_association_vectors(void) {
    u32 config[16];
    u8 bytes[48];
    k_memset(config, 0, sizeof(config));
    k_memset(bytes, 0, sizeof(bytes));
    config[4] = 0x5000000CU;
    config[6] = 0x6320000CU;
    config[8] = 0x4001U;
    config[9] = 0x63400000U;
    bytes[0] = 0x8AU;
    bytes[1] = 43U;
    bytes[46] = 0x79U;
    put64(bytes, 6U, 32U);
    put64(bytes, 14U, 0x63400000ULL);
    put64(bytes, 38U, 0x40000ULL); /* synthetic */
    BootDisplayBarQuery query;
    BootDisplayBar extent;
    boot_display_match_query(bytes, sizeof(bytes), config, 3U, &query, &extent);
    if (query.matching_bars != (1U << 5U) || query.audit.result != BOOT_DISPLAY_BAR_ACCEPTED ||
        extent.base != 0x63400000ULL || extent.size != 0x40000ULL ||
        query.prefix_size != sizeof(bytes) || query.prefix[46] != 0x79U) return false;
    config[6] = 0x63400000U; /* same type/base in a second real slot is ambiguous */
    boot_display_match_query(bytes, sizeof(bytes), config, 3U, &query, &extent);
    if (query.audit.result != BOOT_DISPLAY_BAR_AMBIGUOUS || extent.flags || extent.size ||
        query.matching_bars != ((1U << 2U) | (1U << 5U))) return false;
    config[6] = 0x6320000CU;
    bytes[3] = 0x41U; /* reported in monitor transcription; never mask unknown bits */
    boot_display_match_query(bytes, sizeof(bytes), config, 2U, &query, &extent);
    return !query.matching_bars && !extent.flags && query.prefix[3] == 0x41U &&
        query.audit.result == BOOT_DISPLAY_BAR_TYPE_MISMATCH &&
        boot_display_bar_slot(config, 1U) == BOOT_DISPLAY_BAR_CONTINUATION &&
        boot_display_bar_slot(config, 3U) == BOOT_DISPLAY_BAR_CONTINUATION;
}

static bool firmware_binding_vectors(void) {
    BootInfo boot;
    PciDevice device;
    DisplayProbeInfo info;
    k_memset(&boot, 0, sizeof(boot));
    k_memset(&device, 0, sizeof(device));
    k_memset(&info, 0, sizeof(info));
    boot.magic = BOOT_INFO_MAGIC;
    boot.version = BOOT_INFO_VERSION;
    boot.size = sizeof(boot);
    boot.framebuffer_pci_valid = 1U;
    boot.framebuffer_pci_vendor_device = 0x67DF1002U;
    boot.framebuffer_pci_class_revision = 0x030000EFU;
    boot.framebuffer_pci_subsystem = 0x23101458U;
    boot.framebuffer_bar_audit[5].result = BOOT_DISPLAY_BAR_ACCEPTED;
    boot.framebuffer_bar_audit[5].descriptor_count = 1U;
    boot.framebuffer_bar_query_index[5] = 3U; /* synthetic non-slot firmware numbering */
    boot.framebuffer_bar_queries[3].matching_bars = 1U << 5U;
    boot.framebuffer_bar_queries[3].audit.result = BOOT_DISPLAY_BAR_ACCEPTED;
    boot.framebuffer_bar_queries[3].prefix_size = 48U;
    boot.framebuffer_bar_queries[3].prefix[0] = 0x8AU;
    boot.framebuffer_bar_queries[3].prefix[1] = 43U;
    boot.framebuffer_bars[5].base = 0x63400000ULL;
    boot.framebuffer_bars[5].size = 0x40000ULL;
    boot.framebuffer_bars[5].flags = BOOT_DISPLAY_BAR_VALID | BOOT_DISPLAY_BAR_MEMORY;
    device.class_code = 3U;
    device.vendor_id = info.vendor_id = 0x1002U;
    device.device_id = info.device_id = 0x67DFU;
    device.revision = info.revision = 0xEFU;
    info.subsystem_vendor_id = 0x1458U;
    info.subsystem_device_id = 0x2310U;
    info.bars[5].base = 0x63400000ULL;
    info.bars[5].type = PCI_BAR_MMIO32;
    BootDisplayBar bar;
    if (!display_probe_firmware_bar(&boot, &device, &info, 5U, &bar) ||
        bar.size != 0x40000ULL) return false;
    boot.framebuffer_bar_query_index[5] = BOOT_DISPLAY_QUERY_NONE;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar) || bar.size) return false;
    boot.framebuffer_bar_query_index[5] = 3U;
    boot.framebuffer_bar_queries[3].matching_bars |= 1U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_queries[3].matching_bars = 1U << 5U;
    boot.framebuffer_bar_queries[2].matching_bars = 1U << 5U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_queries[2].matching_bars = 0U;
    boot.framebuffer_bar_queries[3].prefix_size = 49U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_queries[3].prefix_size = 48U;
    boot.framebuffer_bar_audit[5].result = BOOT_DISPLAY_BAR_QUERY_FAILED;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar) || bar.size) return false;
    boot.framebuffer_bar_audit[5].result = BOOT_DISPLAY_BAR_ACCEPTED;
    boot.framebuffer_bar_audit[5].reserved = 1U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_audit[5].reserved = 0U;
    boot.framebuffer_bar_audit[5].query_status = 1ULL << 63U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_audit[5].query_status = 0U;
    boot.framebuffer_bar_audit[5].descriptor_count = 0U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bar_audit[5].descriptor_count = 1U;
    info.bars[5].base++;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar) || bar.size) return false;
    info.bars[5].base--;
    device.device_id++;
    if (display_probe_matches_gop(&boot, &device)) return false;
    device.device_id--;
    info.subsystem_device_id++;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    info.subsystem_device_id--;
    boot.framebuffer_bars[5].reserved = 1U;
    if (display_probe_firmware_bar(&boot, &device, &info, 5U, &bar)) return false;
    boot.framebuffer_bars[5].reserved = 0U;
    boot.reserved1 = 1U;
    return !display_probe_matches_gop(&boot, &device) &&
        !display_probe_firmware_bar(&boot, &device, &info, PCI_STANDARD_BAR_COUNT, &bar);
}

static bool edid_vectors(void) {
    u8 bytes[BOOT_DISPLAY_EDID_BYTES];
    k_memset(bytes, 0, sizeof(bytes));
    for (u32 i = 1U; i < 7U; ++i) bytes[i] = 0xFFU;
    bytes[18U] = 1U;
    bytes[19U] = 4U;
    u8 sum = 0U;
    for (u32 i = 0U; i < sizeof(bytes) - 1U; ++i) sum = (u8)(sum + bytes[i]);
    bytes[127U] = (u8)(0U - sum);
    if (!boot_display_edid_valid(bytes, sizeof(bytes)) ||
        boot_display_edid_valid(bytes, sizeof(bytes) - 1U) || boot_display_edid_valid(0, sizeof(bytes)))
        return false;
    bytes[127U]++;
    if (boot_display_edid_valid(bytes, sizeof(bytes))) return false;
    bytes[127U]--;
    bytes[1U] = 0U;
    return !boot_display_edid_valid(bytes, sizeof(bytes));
}

static bool physical_read_only(void) {
    DisplayLeaseState lease = display_direct_state();
    u64 owner = display_direct_owner_process_id();
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || device->class_code != 3U || (device->header_type & 0x7FU)) continue;
        u16 command = pci_config_read16(device->address, 4U);
        u32 bars[PCI_STANDARD_BAR_COUNT];
        for (u32 j = 0U; j < PCI_STANDARD_BAR_COUNT; ++j)
            bars[j] = pci_config_read32(device->address, (u16)(0x10U + j * 4U));
        u32 rom = pci_config_read32(device->address, 0x30U);
        u16 irq = pci_config_read16(device->address, 0x3CU);
        DisplayProbeInfo info;
        if (!display_probe_read(device, &info) || info.command != command) return false;
        if (pci_config_read16(device->address, 4U) != command ||
            pci_config_read32(device->address, 0x30U) != rom ||
            pci_config_read16(device->address, 0x3CU) != irq) return false;
        for (u32 j = 0U; j < PCI_STANDARD_BAR_COUNT; ++j)
            if (pci_config_read32(device->address, (u16)(0x10U + j * 4U)) != bars[j]) return false;
        PciDevice forged = *device;
        if (display_probe_read(&forged, &info) || !cleared(&info)) return false;
    }
    DisplayProbeInfo info;
    k_memset(&info, 0xA5, sizeof(info));
    return !display_probe_read(0, &info) && cleared(&info) &&
        display_direct_state() == lease && display_direct_owner_process_id() == owner;
}

void display_probe_test_run(void) {
    terminal_writeln("DISPLAY PROBE QUALIFICATION TEST");
    bool identity = identity_and_bars();
    bool caps = capability_vectors();
    bool provenance = firmware_identity();
    bool physical = physical_read_only();
    bool resources = firmware_resource_vectors();
    bool lists = firmware_resource_list_vectors();
    bool queries = query_association_vectors();
    bool binding = firmware_binding_vectors();
    bool edid = edid_vectors();
    bool mmio = polaris_mmio_plan_test();
    report("DISPLAY IDENTITY / 64-BIT BAR BOUNDS / UNKNOWN SIZES", identity);
    report("BOUNDED CAPABILITY PREFIXES / MALFORMED CHAINS REJECTED", caps);
    report("EXACT FIRMWARE PCI PROVENANCE / UNKNOWN OWNER REFUSED", provenance);
    report("READ-ONLY CONFIG / FORGED IDENTITY / GOP LEASE PRESERVED", physical);
    report("FIRMWARE BAR EXTENTS / OVERFLOW / TRANSLATION REFUSALS", resources);
    report("CONTIGUOUS RESOURCE LISTS / FAILURE AUDIT / NO PARTIAL EXTENTS", lists);
    report("QUERY INDEX / EXACT BAR MATCH / AMBIGUITY / RAW TYPE REFUSAL", queries);
    report("EXACT DEVICE / SUBSYSTEM / CURRENT BAR BINDING", binding);
    report("BOUNDED EDID BASE BLOCK / CHECKSUM REFUSALS", edid);
    report("POLARIS BAR5 / COMPLETE RAW DESCRIPTOR / D0 / FOUR-PAGE PLAN", mmio);
    terminal_writeln("MODEL AND INVENTORY CHECK ONLY; NO NATIVE HARDWARE QUALIFICATION.");
    terminal_writeln(identity && caps && provenance && physical && resources && lists && queries && binding && edid && mmio ?
        "DISPLAY PROBE QUALIFICATION TEST: PASS" : "DISPLAY PROBE QUALIFICATION TEST: FAILED");
}
