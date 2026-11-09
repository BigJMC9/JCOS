#include "display_inventory.h"
#include "../include/boot_display.h"

#define DISPLAY_HANDLE_LIMIT 64U

static EFI_GUID g_gop_guid = {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
static EFI_GUID g_path_guid = {0x09576E91,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID g_pci_guid = {0x4CF5B200,0x68B8,0x4CA5,{0x9E,0xEC,0xB2,0x3E,0x3F,0x50,0x02,0x9A}};
static EFI_GUID g_edid_guid = {0xBD8C1056,0x9F36,0x44EC,{0x92,0xA8,0xA6,0x33,0x7F,0x81,0x79,0x86}};

static void clear_inventory(BootInfo *boot) {
    boot->framebuffer_pci_valid = 0;
    boot->framebuffer_pci_segment = 0;
    boot->framebuffer_pci_bus = 0;
    boot->framebuffer_pci_device = 0;
    boot->framebuffer_pci_function = 0;
    boot->reserved1 = 0;
    boot_display_zero(&boot->framebuffer_pci_vendor_device,
        sizeof(*boot) - __builtin_offsetof(BootInfo, framebuffer_pci_vendor_device));
    for (u32 i = 0; i < BOOT_DISPLAY_BAR_COUNT; ++i)
        boot->framebuffer_bar_query_index[i] = BOOT_DISPLAY_QUERY_NONE;
}

static EFI_PCI_IO_PROTOCOL *selected_controller(EFI_BOOT_SERVICES *services, EFI_HANDLE handle) {
    EFI_PCI_IO_PROTOCOL *pci = 0;
    EFI_STATUS status = services->HandleProtocol(handle, &g_pci_guid, (void **)&pci);
    if (EFI_ERROR(status)) {
        void *path = 0;
        EFI_HANDLE parent = 0;
        if (EFI_ERROR(services->HandleProtocol(handle, &g_path_guid, &path)) || !path ||
            EFI_ERROR(services->LocateDevicePath(&g_pci_guid, &path, &parent)) ||
            EFI_ERROR(services->HandleProtocol(parent, &g_pci_guid, (void **)&pci))) return 0;
    }
    return pci && pci->Pci.Read && pci->GetLocation ? pci : 0;
}

static void capture_bar_query(EFI_BOOT_SERVICES *services, BootInfo *boot,
    EFI_PCI_IO_PROTOCOL *pci, const u32 *config, u32 index, BootDisplayBar *extent) {
    BootDisplayBarQuery *query = &boot->framebuffer_bar_queries[index];
    BootDisplayBarAudit *audit = &query->audit;
    if (!pci->GetBarAttributes) {
        audit->result = BOOT_DISPLAY_BAR_NO_PROTOCOL;
        return;
    }
    void *resources = 0;
    u64 supports = 0;
    EFI_STATUS status = pci->GetBarAttributes(pci, (u8)index, &supports, &resources);
    audit->query_status = status;
    if (EFI_ERROR(status)) {
        audit->result = BOOT_DISPLAY_BAR_QUERY_FAILED;
        return; /* unspecified failure outputs must not be dereferenced/freed */
    }
    if (!resources) {
        audit->result = BOOT_DISPLAY_BAR_EMPTY_RESPONSE;
        return;
    }
    const u8 *source = (const u8 *)resources;
    u8 bytes[BOOT_DISPLAY_BAR_RESOURCE_BYTES];
    u32 copied = 0;
    u32 count = 0;
    int complete = 0;
    /* Successful firmware allocation promises an ACPI resource list. Copy
     * complete supported items only, under the trusted allocation contract. */
    for (;;) {
        u8 tag = source[copied];
        if (tag == 0x79U) {
            bytes[copied] = tag;
            bytes[copied + 1U] = source[copied + 1U];
            copied += 2U;
            complete = 1;
            break;
        }
        if (count == BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT) {
            audit->result = BOOT_DISPLAY_BAR_LIST_LIMIT;
            break;
        }
        bytes[copied] = tag;
        audit->descriptor_tag = tag;
        if (tag != 0x8AU) {
            ++copied; /* unknown small item: no large-item header read */
            audit->result = BOOT_DISPLAY_BAR_DESCRIPTOR;
            break;
        }
        u16 length = (u16)boot_display_read_le(source + copied + 1U, 2U);
        audit->descriptor_length = length;
        if (length != 43U) {
            bytes[copied + 1U] = source[copied + 1U];
            bytes[copied + 2U] = source[copied + 2U];
            copied += 3U;
            audit->result = BOOT_DISPLAY_BAR_DESCRIPTOR;
            break;
        }
        boot_display_copy(bytes + copied, source + copied, 46U);
        const u8 *part = bytes + copied;
        audit->reported_base = boot_display_read_le(part + 14U, 8U);
        audit->reported_size = boot_display_read_le(part + 38U, 8U);
        audit->translation = boot_display_read_le(part + 30U, 8U);
        audit->granularity = boot_display_read_le(part + 6U, 8U);
        audit->reported_maximum = boot_display_read_le(part + 22U, 8U);
        audit->resource_type = part[3U];
        audit->general_flags = part[4U];
        audit->specific_flags = part[5U];
        copied += 46U;
        audit->descriptor_count = (u16)++count;
    }
    if (complete) {
        boot_display_match_query(bytes, copied, config, index, query, extent);
    } else {
        query->prefix_size = copied < BOOT_DISPLAY_QUERY_PREFIX_BYTES ? copied : BOOT_DISPLAY_QUERY_PREFIX_BYTES;
        boot_display_copy(query->prefix, bytes, query->prefix_size);
    }
    audit->query_status = status;
    services->FreePool(resources);
}

static void capture_bars(EFI_BOOT_SERVICES *services, BootInfo *boot,
    EFI_PCI_IO_PROTOCOL *pci, const u32 *config) {
    BootDisplayBar candidates[BOOT_DISPLAY_BAR_COUNT];
    boot_display_zero(candidates, sizeof(candidates));
    /* Query every legal firmware index, including physical continuation slots.
     * Some firmware responses do not follow config-header slot numbering. */
    for (u32 query = 0; query < BOOT_DISPLAY_BAR_COUNT; ++query)
        capture_bar_query(services, boot, pci, config, query, &candidates[query]);
    for (u32 bar = 0; bar < BOOT_DISPLAY_BAR_COUNT; ++bar) {
        BootDisplayBarAudit *audit = &boot->framebuffer_bar_audit[bar];
        u32 state = boot_display_bar_slot(config, bar);
        audit->result = state;
        if (state != BOOT_DISPLAY_BAR_NOT_QUERIED) continue;
        u32 count = 0;
        u32 chosen = BOOT_DISPLAY_QUERY_NONE;
        int ambiguous = 0;
        for (u32 query = 0; query < BOOT_DISPLAY_BAR_COUNT; ++query) {
            u32 mask = boot->framebuffer_bar_queries[query].matching_bars;
            if (!(mask & (1U << bar))) continue;
            ++count;
            chosen = query;
            if (mask != (1U << bar)) ambiguous = 1;
        }
        if (count != 1U || ambiguous) {
            boot_display_copy(audit, &boot->framebuffer_bar_queries[bar].audit, sizeof(*audit));
            if (count || ambiguous) audit->result = BOOT_DISPLAY_BAR_AMBIGUOUS;
            else if (audit->result == BOOT_DISPLAY_BAR_ACCEPTED)
                audit->result = BOOT_DISPLAY_BAR_NO_MATCH;
            continue;
        }
        /* The only accepted edge is one query <-> one real physical BAR. */
        boot_display_copy(&boot->framebuffer_bars[bar], &candidates[chosen], sizeof(candidates[chosen]));
        boot_display_copy(audit, &boot->framebuffer_bar_queries[chosen].audit, sizeof(*audit));
        boot->framebuffer_bar_query_index[bar] = chosen;
    }
}

static void invalidate_bar_inventory(BootInfo *boot) {
    boot_display_zero(boot->framebuffer_bars, sizeof(boot->framebuffer_bars));
    for (u32 i = 0; i < BOOT_DISPLAY_BAR_COUNT; ++i) {
        boot->framebuffer_bar_audit[i].result = BOOT_DISPLAY_BAR_CONFIG_CHANGED;
        boot->framebuffer_bar_queries[i].audit.result = BOOT_DISPLAY_BAR_CONFIG_CHANGED;
        boot->framebuffer_bar_queries[i].matching_bars = 0U;
        boot->framebuffer_bar_query_index[i] = BOOT_DISPLAY_QUERY_NONE;
    }
}

static void capture_rom(EFI_BOOT_SERVICES *services, BootInfo *boot, EFI_PCI_IO_PROTOCOL *pci) {
    const void *source = pci->RomImage;
    u64 size = pci->RomSize;
    boot->framebuffer_rom_reported_size = size;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_ABSENT;
    if (!source || !size) return;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_TOO_LARGE;
    if (size > BOOT_DISPLAY_ROM_LIMIT) return;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_INVALID;
    if ((u64)source > ~0ULL - size) return;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_ALLOCATION;
    if (!services->AllocatePages) return;
    EFI_PHYSICAL_ADDRESS base = 0;
    UINTN pages = (size + 4095U) / 4096U;
    if (EFI_ERROR(services->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &base))) return;
    /* Firmware allocation remains reserved even if the source changes. */
    boot->framebuffer_rom_base = base;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_CHANGED;
    if (pci->RomImage != source || pci->RomSize != size) return;
    boot_display_zero((void *)(UINTN)base, pages * 4096U);
    boot_display_copy((void *)(UINTN)base, source, size);
    if (pci->RomImage != source || pci->RomSize != size) return;
    boot->framebuffer_rom_size = (u32)size;
    boot->framebuffer_rom_status = BOOT_DISPLAY_ROM_COPIED;
}

static void capture_pci(EFI_BOOT_SERVICES *services, BootInfo *boot, EFI_HANDLE handle) {
    EFI_PCI_IO_PROTOCOL *pci = selected_controller(services, handle);
    if (!pci) return;
    u32 config[16];
    if (EFI_ERROR(pci->Pci.Read(pci, 2U, 0U, 16U, config)) ||
        !(config[0] & 0xFFFFU) || (config[0] & 0xFFFFU) == 0xFFFFU ||
        (config[2] >> 24U) != 3U || ((config[3] >> 16U) & 0x7FU)) return;
    UINTN segment = 0, bus = 0, device = 0, function = 0;
    if (EFI_ERROR(pci->GetLocation(pci, &segment, &bus, &device, &function)) ||
        segment > 0xFFFFFFFFULL || bus > 255U || device > 31U || function > 7U) return;
    capture_bars(services, boot, pci, config);
    u32 after[16];
    if (EFI_ERROR(pci->Pci.Read(pci, 2U, 0U, 16U, after))) {
        invalidate_bar_inventory(boot);
        return;
    }
    /* Refuse stale identity or moved BARs; volatile status is not compared. */
    if (after[0] != config[0] || after[2] != config[2] || after[3] != config[3] || after[11] != config[11]) {
        invalidate_bar_inventory(boot);
        return;
    }
    for (u32 i = 4U; i < 10U; ++i) {
        if (after[i] == config[i]) continue;
        invalidate_bar_inventory(boot);
        return;
    }
    boot->framebuffer_pci_segment = (u32)segment;
    boot->framebuffer_pci_bus = (u8)bus;
    boot->framebuffer_pci_device = (u8)device;
    boot->framebuffer_pci_function = (u8)function;
    boot->framebuffer_pci_vendor_device = config[0];
    boot->framebuffer_pci_class_revision = config[2];
    boot->framebuffer_pci_subsystem = config[11U];
    boot->framebuffer_pci_valid = 1U;
    /* Exact selected AMD controller, RAM copy only; no PCI/ROM BAR writes. */
    if (config[0] == 0x67DF1002U) capture_rom(services, boot, pci);
}

void loader_capture_display_inventory(EFI_BOOT_SERVICES *services, BootInfo *boot,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *selected) {
    if (!boot) return;
    clear_inventory(boot);
    if (!services || !selected || !services->LocateHandleBuffer || !services->HandleProtocol ||
        !services->LocateDevicePath || !services->FreePool) return;
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS status = services->LocateHandleBuffer(2U, &g_gop_guid, 0, &count, &handles);
    if (EFI_ERROR(status)) return; /* failure outputs have no allocation contract */
    if (!handles || !count || count > DISPLAY_HANDLE_LIMIT) {
        if (handles) services->FreePool(handles);
        return;
    }
    EFI_HANDLE match = 0;
    int ambiguous = 0;
    for (UINTN i = 0; i < count; ++i) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
        if (EFI_ERROR(services->HandleProtocol(handles[i], &g_gop_guid, (void **)&gop))) {
            ambiguous = 1;
            continue;
        }
        if (gop != selected) continue;
        if (match) ambiguous = 1;
        match = handles[i];
    }
    if (match && !ambiguous) {
        capture_pci(services, boot, match);
        EFI_EDID_ACTIVE_PROTOCOL *edid = 0;
        if (!EFI_ERROR(services->HandleProtocol(match, &g_edid_guid, (void **)&edid)) &&
            edid && boot_display_edid_valid(edid->Edid, edid->SizeOfEdid)) {
            for (u32 i = 0; i < BOOT_DISPLAY_EDID_BYTES; ++i)
                boot->framebuffer_edid[i] = edid->Edid[i];
            boot->framebuffer_edid_size = BOOT_DISPLAY_EDID_BYTES;
        }
    }
    services->FreePool(handles);
}
