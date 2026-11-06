#include "display_probe.h"

#include "display.h"
#include "../include/boot_display.h"
#include "lib.h"
#include "serial.h"
#include "terminal.h"

#define DISPLAY_CLASS 0x03U
#define CONFIG_HEADER_SIZE 0x40U
#define STATUS_CAPABILITIES (1U << 4)

static u16 read16(const u8 *bytes, u32 offset) {
    return (u16)((u16)bytes[offset] | ((u16)bytes[offset + 1U] << 8));
}

static u32 read32(const u8 *bytes, u32 offset) {
    return (u32)read16(bytes, offset) | ((u32)read16(bytes, offset + 2U) << 16);
}

static bool capability_offset_valid(u8 offset) {
    return offset >= 0x40U && offset <= 0xFCU && !(offset & 3U);
}

/* Only these standard prefixes are interpreted, never private registers. */
static u32 capability_prefix_size(u8 id) {
    if (id == 0x01U) return 8U;  /* power management */
    if (id == 0x10U) return 12U; /* PCIe through Device Status */
    if (id == 0x11U) return 12U; /* MSI-X (table descriptors only) */
    if (id == 0x05U) return 4U;  /* MSI through Message Control */
    return 2U;
}

static bool decode_bars(const u8 *config, DisplayProbeInfo *info) {
    for (u8 index = 0U; index < PCI_STANDARD_BAR_COUNT; ++index) {
        PciBar *bar = &info->bars[index];
        bar->index = index;
        u32 low = read32(config, 0x10U + (u32)index * 4U);
        if (!low) continue;
        if (low == 0xFFFFFFFFU) return false;
        if (low & 1U) {
            bar->type = PCI_BAR_IO;
            bar->base = (u64)(low & ~3U);
            continue;
        }
        u32 type = (low >> 1) & 3U;
        bar->prefetchable = (low & 8U) != 0U;
        bar->base = (u64)(low & ~0xFU);
        if (type == 0U) {
            bar->type = PCI_BAR_MMIO32;
        } else if (type == 2U && index + 1U < PCI_STANDARD_BAR_COUNT) {
            bar->type = PCI_BAR_MMIO64;
            bar->base |= (u64)read32(config, 0x14U + (u32)index * 4U) << 32;
            info->bar_continuation[++index] = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool decode_capabilities(const u8 *config, DisplayProbeInfo *info) {
    if (!(info->status & STATUS_CAPABILITIES)) return true;
    u8 pointer = config[0x34U];
    if (!pointer) return false;
    bool occupied[PCI_CONFIG_SPACE_SIZE];
    k_memset(occupied, 0, sizeof(occupied));
    for (u32 step = 0U; step < DISPLAY_PROBE_CAPACITY; ++step) {
        if (!capability_offset_valid(pointer) || occupied[pointer]) return false;
        u8 id = config[pointer];
        u8 next = config[(u32)pointer + 1U];
        u32 length = capability_prefix_size(id);
        if (id == 0xFFU || length > PCI_CONFIG_SPACE_SIZE - (u32)pointer) return false;
        for (u32 i = pointer; i < (u32)pointer + length; ++i) {
            if (occupied[i]) return false;
            occupied[i] = true;
        }
        info->capability_offsets[step] = pointer;
        info->capability_ids[step] = id;
        info->capability_count++;
        if (id == 0x01U) {
            if (info->pm_offset) return false;
            info->pm_offset = pointer;
        } else if (id == 0x10U) {
            if (info->pcie_offset) return false;
            info->pcie_offset = pointer;
            u32 caps = read32(config, (u32)pointer + 4U);
            if (caps == 0xFFFFFFFFU) return false;
            info->flr_advertised = (caps & (1U << 28)) != 0U;
        } else if (id == 0x05U) {
            if (info->msi_offset) return false;
            info->msi_offset = pointer;
            info->msi_control = read16(config, (u32)pointer + 2U);
        } else if (id == 0x11U) {
            if (info->msix_offset) return false;
            info->msix_offset = pointer;
            info->msix_control = read16(config, (u32)pointer + 2U);
        }
        if (!next) return true;
        pointer = next;
    }
    return false;
}

bool display_probe_decode(const u8 *config, u32 size, DisplayProbeInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!config || size < PCI_CONFIG_SPACE_SIZE || read16(config, 0U) == 0xFFFFU ||
        !read16(config, 0U) || config[0x0BU] != DISPLAY_CLASS ||
        (config[0x0EU] & 0x7FU) != 0U || read16(config, 6U) == 0xFFFFU) return false;
    DisplayProbeInfo info;
    k_memset(&info, 0, sizeof(info));
    info.vendor_id = read16(config, 0U);
    info.device_id = read16(config, 2U);
    info.subsystem_vendor_id = read16(config, 0x2CU);
    info.subsystem_device_id = read16(config, 0x2EU);
    info.command = read16(config, 4U);
    info.status = read16(config, 6U);
    info.revision = config[8U];
    info.prog_if = config[9U];
    info.subclass = config[0x0AU];
    info.interrupt_line = config[0x3CU];
    info.interrupt_pin = config[0x3DU];
    info.rom_bar = read32(config, 0x30U);
    if (!decode_bars(config, &info) || !decode_capabilities(config, &info)) return false;
    /* Linux v6.12 amdgpu PCI table identifies 1002:67DF as POLARIS10.
     * This shared silicon ID does not establish the board model or support. */
    info.polaris10_candidate = info.vendor_id == 0x1002U && info.device_id == 0x67DFU;
    k_memcpy(out, &info, sizeof(info));
    return true;
}

bool display_probe_read(const PciDevice *device, DisplayProbeInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!pci_device_is_enumerated(device) || device->class_code != DISPLAY_CLASS ||
        (device->header_type & 0x7FU) != 0U) return false;
    u8 config[PCI_CONFIG_SPACE_SIZE];
    k_memset(config, 0, sizeof(config));
    for (u16 offset = 0U; offset < CONFIG_HEADER_SIZE; offset += 4U) {
        u32 value = pci_config_read32(device->address, offset);
        for (u32 byte = 0U; byte < 4U; ++byte)
            config[(u32)offset + byte] = (u8)(value >> (byte * 8U));
    }
    if (read16(config, 0U) != device->vendor_id ||
        read16(config, 2U) != device->device_id || config[8U] != device->revision) return false;
    if (read16(config, 6U) & STATUS_CAPABILITIES) {
        bool visited[PCI_CONFIG_SPACE_SIZE];
        k_memset(visited, 0, sizeof(visited));
        u8 pointer = config[0x34U];
        for (u32 step = 0U; pointer && step < DISPLAY_PROBE_CAPACITY; ++step) {
            if (!capability_offset_valid(pointer) || visited[pointer]) return false;
            visited[pointer] = true;
            u8 id = pci_config_read8(device->address, pointer);
            u32 length = capability_prefix_size(id);
            if (length > PCI_CONFIG_SPACE_SIZE - (u32)pointer) return false;
            for (u32 byte = 0U; byte < length; ++byte)
                config[(u32)pointer + byte] = pci_config_read8(device->address, (u16)((u32)pointer + byte));
            if (config[pointer] != id) return false;
            pointer = config[(u32)pointer + 1U];
        }
        if (pointer) return false;
    }
    /* Detect disappearance or identity changes during the read. This is an
     * inventory snapshot, not an atomic device-state or hotplug contract. */
    if (pci_config_read32(device->address, 0U) != read32(config, 0U)) return false;
    return display_probe_decode(config, sizeof(config), out);
}

static bool boot_metadata_valid(const BootInfo *boot) {
    return boot && boot->magic == BOOT_INFO_MAGIC && boot->version == BOOT_INFO_VERSION &&
        boot->size >= sizeof(*boot) && !boot->reserved1 && !boot->framebuffer_metadata_reserved;
}

bool display_probe_matches_gop(const BootInfo *boot, const PciDevice *device) {
    if (!boot_metadata_valid(boot) || !device || boot->framebuffer_pci_valid != 1U ||
        boot->framebuffer_pci_segment || device->class_code != DISPLAY_CLASS ||
        !pci_address_valid(device->address)) return false;
    u32 identity = (u32)device->vendor_id | ((u32)device->device_id << 16U);
    u32 class_revision = (u32)device->revision | ((u32)device->prog_if << 8U) |
        ((u32)device->subclass << 16U) | ((u32)device->class_code << 24U);
    return boot->framebuffer_pci_vendor_device == identity &&
        boot->framebuffer_pci_class_revision == class_revision &&
        boot->framebuffer_pci_bus == device->address.bus &&
        boot->framebuffer_pci_device == device->address.device &&
        boot->framebuffer_pci_function == device->address.function;
}

bool display_probe_firmware_bar(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u32 index, BootDisplayBar *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!display_probe_matches_gop(boot, device) || !info || index >= PCI_STANDARD_BAR_COUNT ||
        info->bar_continuation[index] || info->vendor_id != device->vendor_id ||
        info->device_id != device->device_id || info->revision != device->revision ||
        info->subclass != device->subclass || info->prog_if != device->prog_if ||
        boot->framebuffer_pci_subsystem != ((u32)info->subsystem_vendor_id |
            ((u32)info->subsystem_device_id << 16U))) return false;
    u32 query_index = boot->framebuffer_bar_query_index[index];
    if (query_index >= BOOT_DISPLAY_BAR_COUNT) return false;
    const BootDisplayBarQuery *query = &boot->framebuffer_bar_queries[query_index];
    if (query->matching_bars != (1U << index) ||
        query->audit.result != BOOT_DISPLAY_BAR_ACCEPTED || query->audit.reserved ||
        (query->audit.query_status >> 63U) ||
        query->prefix_size < 46U || query->prefix_size > BOOT_DISPLAY_QUERY_PREFIX_BYTES ||
        query->prefix[0] != 0x8AU || query->prefix[1] != 43U || query->prefix[2]) return false;
    for (u32 i = 0; i < BOOT_DISPLAY_BAR_COUNT; ++i)
        if (i != query_index && (boot->framebuffer_bar_queries[i].matching_bars & (1U << index)))
            return false;
    const BootDisplayBarAudit *audit = &boot->framebuffer_bar_audit[index];
    if (audit->result != BOOT_DISPLAY_BAR_ACCEPTED || audit->reserved ||
        (audit->query_status >> 63U) || !audit->descriptor_count ||
        audit->descriptor_count > BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT) return false;
    const PciBar *bar = &info->bars[index];
    const BootDisplayBar *saved = &boot->framebuffer_bars[index];
    u32 flags = BOOT_DISPLAY_BAR_VALID;
    if (bar->type == PCI_BAR_MMIO32 || bar->type == PCI_BAR_MMIO64) {
        flags |= BOOT_DISPLAY_BAR_MEMORY;
        if (bar->type == PCI_BAR_MMIO64) flags |= BOOT_DISPLAY_BAR_64BIT;
        if (bar->prefetchable) flags |= BOOT_DISPLAY_BAR_PREFETCH;
    } else if (bar->type != PCI_BAR_IO) {
        return false;
    }
    if (saved->flags != flags || saved->reserved || !saved->base || !saved->size ||
        saved->base != bar->base || (saved->size & (saved->size - 1ULL)) ||
        (saved->base & (saved->size - 1ULL)) ||
        saved->size - 1ULL > ~0ULL - saved->base ||
        (!(flags & BOOT_DISPLAY_BAR_64BIT) && saved->base + saved->size - 1ULL > 0xFFFFFFFFULL))
        return false;
    k_memcpy(out, saved, sizeof(*out));
    return true;
}

static void field(const char *name, u64 value) {
    terminal_write(name);
    terminal_write_hex(value);
}

static void address(PciAddress value) {
    terminal_write_u64(value.bus);
    terminal_putchar(':');
    terminal_write_u64(value.device);
    terminal_putchar('.');
    terminal_write_u64(value.function);
}

static const char *bar_result(u32 result) {
    /* Inline character rows avoid pointer-table runtime relocations in PIE. */
    static const char names[][32] = {
        [BOOT_DISPLAY_BAR_NOT_QUERIED] = "NOT QUERIED",
        [BOOT_DISPLAY_BAR_UNASSIGNED] = "UNASSIGNED",
        [BOOT_DISPLAY_BAR_CONTINUATION] = "64-BIT CONTINUATION",
        [BOOT_DISPLAY_BAR_NO_PROTOCOL] = "NO GETBARATTRIBUTES",
        [BOOT_DISPLAY_BAR_QUERY_FAILED] = "EFI QUERY FAILED",
        [BOOT_DISPLAY_BAR_EMPTY_RESPONSE] = "EMPTY EFI RESPONSE",
        [BOOT_DISPLAY_BAR_DESCRIPTOR] = "UNSUPPORTED DESCRIPTOR",
        [BOOT_DISPLAY_BAR_MALFORMED] = "MALFORMED LIST",
        [BOOT_DISPLAY_BAR_CHECKSUM] = "CHECKSUM REJECTED",
        [BOOT_DISPLAY_BAR_TRANSLATED] = "TRANSLATION UNSUPPORTED",
        [BOOT_DISPLAY_BAR_TYPE_MISMATCH] = "TYPE / WIDTH MISMATCH",
        [BOOT_DISPLAY_BAR_BASE_MISMATCH] = "PCI BASE MISMATCH",
        [BOOT_DISPLAY_BAR_EXTENT] = "EXTENT REJECTED",
        [BOOT_DISPLAY_BAR_NONCONTIGUOUS] = "NONCONTIGUOUS LIST",
        [BOOT_DISPLAY_BAR_ACCEPTED] = "ACCEPTED INVENTORY",
        [BOOT_DISPLAY_BAR_CONFIG_CHANGED] = "CONFIG CHANGED",
        [BOOT_DISPLAY_BAR_LIST_LIMIT] = "DESCRIPTOR LIMIT",
        [BOOT_DISPLAY_BAR_EMPTY_LIST] = "NO ADDRESS DESCRIPTORS",
        [BOOT_DISPLAY_BAR_NO_MATCH] = "NO EXACT BAR MATCH",
        [BOOT_DISPLAY_BAR_AMBIGUOUS] = "AMBIGUOUS ASSOCIATION",
    };
    if (result >= sizeof(names) / sizeof(names[0])) return "UNKNOWN RESULT";
    return names[result];
}

static void print_bar_audit(const BootInfo *boot, const PciDevice *device, u32 index,
    bool bound) {
    if (!display_probe_matches_gop(boot, device)) return;
    const BootDisplayBarAudit *audit = &boot->framebuffer_bar_audit[index];
    terminal_write("    FW BIND: ");
    terminal_write(bar_result(audit->result));
    terminal_write(" QUERY_INDEX=");
    u32 query_index = boot->framebuffer_bar_query_index[index];
    if (query_index < BOOT_DISPLAY_BAR_COUNT) terminal_write_u64(query_index);
    else terminal_write("NONE");
    field(" EFI_STATUS=", audit->query_status);
    if (audit->result == BOOT_DISPLAY_BAR_ACCEPTED && !bound)
        terminal_write(" CURRENT BINDING REJECTED");
    terminal_putchar('\n');
}

void display_probe_firmware_print(const BootInfo *boot) {
    terminal_write("DISPLAY FIRMWARE QUERIES: BOOT INVENTORY ABI=");
    terminal_write_u64(BOOT_INFO_VERSION);
    terminal_putchar('\n');
    terminal_writeln("COPIED UEFI DATA ONLY; QUERY INDEX IS DISTINCT FROM PHYSICAL BAR SLOT.");
    if (!boot_metadata_valid(boot) || boot->framebuffer_pci_valid != 1U) {
        terminal_writeln("NO VALID SELECTED-CONTROLLER PROVENANCE; NATIVE ACCESS REFUSED.");
        return;
    }
    field("SELECTED CONTROLLER SEGMENT=", boot->framebuffer_pci_segment);
    terminal_write(" BDF=");
    PciAddress gop = {boot->framebuffer_pci_bus, boot->framebuffer_pci_device, boot->framebuffer_pci_function};
    address(gop);
    field(" ID=", boot->framebuffer_pci_vendor_device);
    field(" SUBSYSTEM=", boot->framebuffer_pci_subsystem);
    terminal_putchar('\n');
    const char *hex = "0123456789ABCDEF";
    for (u32 i = 0; i < BOOT_DISPLAY_BAR_COUNT; ++i) {
        const BootDisplayBarQuery *query = &boot->framebuffer_bar_queries[i];
        const BootDisplayBarAudit *audit = &query->audit;
        terminal_write("FW QUERY INDEX=");
        terminal_write_u64(i);
        terminal_write(" RESULT=");
        terminal_write(bar_result(audit->result));
        field(" EFI_STATUS=", audit->query_status);
        field(" MATCHED_BAR_MASK=", query->matching_bars);
        terminal_putchar('\n');
        terminal_write("  DESCRIPTORS=");
        terminal_write_u64(audit->descriptor_count);
        field(" TAG=", audit->descriptor_tag);
        field(" LENGTH=", audit->descriptor_length);
        terminal_putchar('\n');
        if (audit->descriptor_count) {
            field("  LAST QWORD TYPE=", audit->resource_type);
            field(" GRAN=", audit->granularity);
            field(" FLAGS=", (u32)audit->general_flags | ((u32)audit->specific_flags << 8U));
            terminal_putchar('\n');
            field("  REPORTED BASE=", audit->reported_base);
            field(" SPAN=", audit->reported_size);
            field(" MAX=", audit->reported_maximum);
            field(" TRANSLATION=", audit->translation);
            terminal_putchar('\n');
        }
        if (query->prefix_size > BOOT_DISPLAY_QUERY_PREFIX_BYTES) {
            terminal_writeln("  INVALID PREFIX SIZE; NOT READ.");
            continue;
        }
        for (u32 offset = 0; offset < query->prefix_size; offset += 16U) {
            field("  RAW PREFIX +", offset);
            terminal_write(":");
            for (u32 j = offset; j < query->prefix_size && j < offset + 16U; ++j) {
                terminal_putchar(' ');
                terminal_putchar(hex[query->prefix[j] >> 4U]);
                terminal_putchar(hex[query->prefix[j] & 15U]);
            }
            terminal_putchar('\n');
        }
    }
    terminal_writeln("FIRMWARE QUERY REPORT COMPLETE; INVENTORY ONLY, NATIVE ACCESS UNQUALIFIED.");
}

static void print_function(const BootInfo *boot, const PciDevice *device) {
    terminal_write("DISPLAY PCI ");
    address(device->address);
    terminal_writeln(display_probe_matches_gop(boot, device) ? "  SELECTED GOP" : "  GOP OWNER NOT ESTABLISHED");
    DisplayProbeInfo info;
    if (!display_probe_read(device, &info)) {
        terminal_writeln("  INVALID / CHANGED CONFIGURATION; NATIVE ACCESS REFUSED.");
        return;
    }
    field("  VEN=", info.vendor_id);
    field(" DEV=", info.device_id);
    field(" REV=", info.revision);
    field(" SUBVEN=", info.subsystem_vendor_id);
    field(" SUBDEV=", info.subsystem_device_id);
    terminal_putchar('\n');
    terminal_writeln(info.polaris10_candidate ?
        "  AMD POLARIS10 CANDIDATE; RX 570 BOARD IDENTITY REQUIRES CONFIRMATION." :
        "  NO RX 570 CANDIDATE MATCH; FIRMWARE DISPLAY RETAINED.");
    field("  COMMAND=", info.command);
    field(" STATUS=", info.status);
    field(" CLASS=0x03 SUB=", info.subclass);
    field(" IF=", info.prog_if);
    terminal_putchar('\n');
    terminal_write("  FIRMWARE BUS MASTER=");
    terminal_writeln(info.command & PCI_COMMAND_BUS_MASTER ? "ENABLED (INHERITED; NOT ISOLATED)" : "DISABLED");
    for (u32 index = 0U; index < PCI_STANDARD_BAR_COUNT; ++index) {
        terminal_write("  BAR");
        terminal_write_u64(index);
        const PciBar *bar = &info.bars[index];
        if (info.bar_continuation[index]) {
            terminal_writeln(" 64-BIT CONTINUATION");
            continue;
        }
        if (bar->type == PCI_BAR_NONE) {
            terminal_writeln(" UNASSIGNED / UNIMPLEMENTED");
            continue;
        }
        terminal_write(bar->type == PCI_BAR_IO ? " IO" :
            bar->type == PCI_BAR_MMIO64 ? " MMIO64" : " MMIO32");
        field(" BASE=", bar->base);
        terminal_write(bar->prefetchable ? " PREFETCH" : "");
        BootDisplayBar extent;
        bool bound = display_probe_firmware_bar(boot, device, &info, index, &extent);
        if (bound) {
            field(" SIZE=", extent.size);
            terminal_writeln(" (FIRMWARE INVENTORY; ACCESS UNQUALIFIED)");
        } else {
            terminal_writeln(" SIZE=UNKNOWN / UNVERIFIED (NO SIZING WRITE)");
        }
        print_bar_audit(boot, device, index, bound);
    }
    field("  ROM BAR=", info.rom_bar);
    terminal_writeln(" (NO ROM ACCESS BY PROBE)");
    for (u32 i = 0U; i < info.capability_count; ++i) {
        field("  CAP ID=", info.capability_ids[i]);
        field(" OFFSET=", info.capability_offsets[i]);
        terminal_putchar('\n');
    }
    field("  MSI CONTROL=", info.msi_control);
    field(" MSI-X CONTROL=", info.msix_control);
    terminal_writeln(" (NO IRQ ROUTE INSTALLED)");
    field("  INTx PIN=", info.interrupt_pin);
    field(" LINE=", info.interrupt_line);
    terminal_writeln(" (FIRMWARE HINT; ROUTE UNQUALIFIED)");
    terminal_writeln(info.flr_advertised ?
        "  FLR ADVERTISED; NOT EXECUTED / NOT QUALIFIED." :
        "  NO FLR ADVERTISEMENT; RESET / RESTORATION UNQUALIFIED.");
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *peer = pci_device(i);
        if (!peer || peer == device || peer->address.bus != device->address.bus ||
            peer->address.device != device->address.device) continue;
        terminal_write("  SIBLING ");
        address(peer->address);
        field(" VEN=", peer->vendor_id);
        field(" DEV=", peer->device_id);
        field(" CLASS=", peer->class_code);
        terminal_writeln(" (SHARED RESET IMPACT UNKNOWN)");
    }
}

static void print_edid(const BootInfo *boot) {
    if (!boot_metadata_valid(boot) || boot->framebuffer_edid_reserved ||
        boot->framebuffer_edid_size != BOOT_DISPLAY_EDID_BYTES ||
        !boot_display_edid_valid(boot->framebuffer_edid, boot->framebuffer_edid_size)) {
        terminal_writeln("EDID: UNAVAILABLE / INVALID; NO MONITOR OR MODE INFERRED.");
        return;
    }
    const u8 *edid = boot->framebuffer_edid;
    terminal_writeln("EDID: SELECTED GOP ACTIVE BASE BLOCK (FIRMWARE; MAY BE OVERRIDDEN).");
    u16 manufacturer = ((u16)edid[8U] << 8U) | (u16)edid[9U];
    field("EDID MANUFACTURER CODE=", manufacturer);
    field(" PRODUCT=", boot_display_read_le(edid + 10U, 2U));
    field(" SERIAL=", boot_display_read_le(edid + 12U, 4U));
    terminal_write(" VERSION=");
    terminal_write_u64(edid[18U]);
    terminal_putchar('.');
    terminal_write_u64(edid[19U]);
    terminal_write(" EXTENSION BLOCKS NOT CAPTURED=");
    terminal_write_u64(edid[126U]);
    terminal_putchar('\n');
    for (u32 i = 0U; i < 4U; ++i) {
        const u8 *descriptor = edid + 54U + i * 18U;
        if (descriptor[0] || descriptor[1] || descriptor[2] ||
            descriptor[3] != 0xFCU || descriptor[4]) continue;
        char name[14];
        k_memset(name, 0, sizeof(name));
        u32 count = 0U;
        bool valid = true;
        for (u32 j = 5U; j < 18U; ++j) {
            if (descriptor[j] == 0x0AU || !descriptor[j]) break;
            if (descriptor[j] < 0x20U || descriptor[j] > 0x7EU) {
                valid = false;
                break;
            }
            name[count++] = (char)descriptor[j];
        }
        while (count && name[count - 1U] == ' ') name[--count] = 0;
        if (valid && count) {
            terminal_write("EDID DISPLAY NAME=");
            terminal_writeln(name);
        }
    }
    u32 clock = (u32)boot_display_read_le(edid + 54U, 2U);
    u32 width = (u32)edid[56U] | ((u32)(edid[58U] & 0xF0U) << 4U);
    u32 height = (u32)edid[59U] | ((u32)(edid[61U] & 0xF0U) << 4U);
    if (clock && width && height) {
        terminal_write("EDID DTD0 ACTIVE=");
        terminal_write_u64(width);
        terminal_putchar('x');
        terminal_write_u64(height);
        terminal_write(" PIXEL CLOCK HZ=");
        terminal_write_u64((u64)clock * 10000ULL);
        terminal_writeln(" (INVENTORY; NO MODE PROGRAMMING)");
    }
}

void display_probe_print(const BootInfo *boot) {
    terminal_writeln("DISPLAY PROBE: READ-ONLY PCI / FIRMWARE INVENTORY");
    terminal_write("BOOT DISPLAY INVENTORY ABI=");
    terminal_write_u64(BOOT_INFO_VERSION);
    terminal_putchar('\n');
    terminal_writeln(framebuffer_firmware_valid() ?
        "BACKEND: FIRMWARE GOP; NATIVE MODESETTING DISABLED." :
        "BACKEND: NO QUALIFIED SCANOUT; NATIVE MODESETTING DISABLED.");
    FramebufferScanoutStatus scanout;
    framebuffer_scanout_status(&scanout);
    terminal_write("BOOT SCANOUT: ");
    if (scanout.state == FRAMEBUFFER_SCANOUT_FIRMWARE) terminal_write("FIRMWARE VALID");
    else if (scanout.state == FRAMEBUFFER_SCANOUT_RETIRING) terminal_write("RETIRING");
    else if (scanout.state == FRAMEBUFFER_SCANOUT_RETIRED) terminal_write("RETIRED");
    else terminal_write("UNAVAILABLE");
    terminal_write(" / KERNEL WRITERS=");
    terminal_write_u64(scanout.kernel_writers);
    terminal_putchar('\n');
    terminal_writeln("FIRMWARE RECOVERY: REQUIRES UNDISRUPTED BOOT SCANOUT / OPERATIONAL GPU.");
    terminal_writeln("NATIVE RESCUE DRIVER: NOT IMPLEMENTED / NOT QUALIFIED.");
    FramebufferInfo fb;
    if (framebuffer_info(&fb)) {
        field("GOP BASE=", fb.physical_base);
        field(" BYTES=", fb.size);
        terminal_putchar('\n');
        terminal_write("GOP MODE ");
        terminal_write_u64(fb.width);
        terminal_putchar('x');
        terminal_write_u64(fb.height);
        terminal_write(" STRIDE=");
        terminal_write_u64(fb.pixels_per_scanline);
        field(" FORMAT=", fb.pixel_format);
        terminal_putchar('\n');
        field("MASK R=", fb.red_mask);
        field(" G=", fb.green_mask);
        field(" B=", fb.blue_mask);
        field(" RESERVED=", fb.reserved_mask);
        terminal_putchar('\n');
        field("GOP MEMORY TYPE=", fb.memory_type);
        terminal_writeln(fb.direct_map_safe ? " BOOT PAGE SPAN SAFE" : " BOOT PAGE SPAN UNSAFE");
        terminal_writeln(framebuffer_firmware_valid() ?
            "GOP ACCESS: FIRMWARE VALID; EXCLUSIVE WRITER LEASE REQUIRED." :
            "GOP ACCESS: REFUSED; SAVED METADATA IS NOT ACTIVE OUTPUT.");
    } else {
        terminal_writeln("NO VALIDATED FIRMWARE FRAMEBUFFER.");
    }
    if (boot_metadata_valid(boot)) {
        terminal_write("FIRMWARE MODE INDEX=");
        terminal_write_u64(boot->framebuffer_mode);
        terminal_putchar('\n');
    }
    print_edid(boot);
    if (boot_metadata_valid(boot) && boot->framebuffer_pci_valid == 1U) {
        field("GOP PCI SEGMENT=", boot->framebuffer_pci_segment);
        terminal_write(" BDF=");
        PciAddress gop = {boot->framebuffer_pci_bus, boot->framebuffer_pci_device, boot->framebuffer_pci_function};
        address(gop);
        terminal_putchar('\n');
    } else {
        terminal_writeln("GOP PCI PROVENANCE UNKNOWN; NO OWNER INFERRED.");
    }
    field("DISPLAY LEASE STATE=", display_direct_state());
    field(" OWNER PID=", display_direct_owner_process_id());
    terminal_putchar('\n');
    terminal_writeln(serial_available() ? "COM1: AVAILABLE (CAPTURE STILL REQUIRES EXTERNAL VERIFICATION)" : "COM1: UNAVAILABLE");
    u32 displays = 0U;
    u32 candidates = 0U;
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || device->class_code != DISPLAY_CLASS) continue;
        ++displays;
        if (device->vendor_id == 0x1002U && device->device_id == 0x67DFU) ++candidates;
        print_function(boot, device);
    }
    terminal_write("DISPLAY FUNCTIONS=");
    terminal_write_u64(displays);
    terminal_write(" POLARIS10 CANDIDATE IDENTITIES=");
    terminal_write_u64(candidates);
    terminal_putchar('\n');
    terminal_writeln("CONNECTOR / NATIVE MODE: UNKNOWN; IOMMU / IRQ REMAPPING: UNQUALIFIED.");
    terminal_writeln("NATIVE TAKEOVER REFUSED: ACCESS, BUFFER BOUNDS AND RECOVERY NOT QUALIFIED.");
    terminal_writeln(framebuffer_firmware_valid() ?
        "DISPLAY PROBE COMPLETE; FIRMWARE DISPLAY RETAINED." :
        "DISPLAY PROBE COMPLETE; SAVED GOP ACCESS REMAINS DISABLED.");
}
