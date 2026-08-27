#include "acpi.h"
#include "arch.h"
#include "lib.h"

#define ACPI_FADT_RESET_SUPPORTED (1U << 10)

typedef struct PACKED {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
} RsdpV1;

typedef struct PACKED {
    RsdpV1 first;
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
} RsdpV2;

typedef struct PACKED {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} AcpiSdtHeader;

typedef struct PACKED {
    AcpiSdtHeader header;
    u32 local_apic_address;
    u32 flags;
    u8 entries[];
} Madt;

typedef struct PACKED {
    u8 address_space;
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;
    u64 address;
} GenericAddress;

static AcpiInfo g_info;

static bool bytes_equal(const char *a, const char *b, u32 count) {
    for (u32 i = 0; i < count; ++i) if (a[i] != b[i]) return false;
    return true;
}

static bool checksum_ok(const void *address, u32 length) {
    if (!address || !length || length > (16U * 1024U * 1024U)) return false;
    const u8 *bytes = (const u8 *)address;
    u8 sum = 0;
    for (u32 i = 0; i < length; ++i) sum = (u8)(sum + bytes[i]);
    return sum == 0;
}

static bool sdt_valid(const AcpiSdtHeader *header) {
    return header && header->length >= sizeof(AcpiSdtHeader) && checksum_ok(header, header->length);
}

static const AcpiSdtHeader *find_table(const RsdpV1 *rsdp, const char signature[4]) {
    if (rsdp->revision >= 2) {
        const RsdpV2 *v2 = (const RsdpV2 *)rsdp;
        if (v2->xsdt_address) {
            const AcpiSdtHeader *xsdt = (const AcpiSdtHeader *)(u64)v2->xsdt_address;
            if (sdt_valid(xsdt) && bytes_equal(xsdt->signature, "XSDT", 4)) {
                u32 count = (xsdt->length - sizeof(AcpiSdtHeader)) / 8U;
                const u64 *entries = (const u64 *)((const u8 *)xsdt + sizeof(AcpiSdtHeader));
                for (u32 i = 0; i < count; ++i) {
                    const AcpiSdtHeader *table = (const AcpiSdtHeader *)(u64)entries[i];
                    if (sdt_valid(table) && bytes_equal(table->signature, signature, 4)) return table;
                }
            }
        }
    }

    if (!rsdp->rsdt_address) return 0;
    const AcpiSdtHeader *rsdt = (const AcpiSdtHeader *)(u64)rsdp->rsdt_address;
    if (!sdt_valid(rsdt) || !bytes_equal(rsdt->signature, "RSDT", 4)) return 0;
    u32 count = (rsdt->length - sizeof(AcpiSdtHeader)) / 4U;
    const u32 *entries = (const u32 *)((const u8 *)rsdt + sizeof(AcpiSdtHeader));
    for (u32 i = 0; i < count; ++i) {
        const AcpiSdtHeader *table = (const AcpiSdtHeader *)(u64)entries[i];
        if (sdt_valid(table) && bytes_equal(table->signature, signature, 4)) return table;
    }
    return 0;
}

static void parse_madt(const AcpiSdtHeader *header) {
    if (!header || header->length < sizeof(Madt)) return;
    const Madt *madt = (const Madt *)header;
    g_info.lapic_address = madt->local_apic_address;
    g_info.madt_flags = madt->flags;
    g_info.keyboard_gsi = 1;
    g_info.keyboard_flags = 0;

    const u8 *cursor = madt->entries;
    const u8 *end = (const u8 *)madt + madt->header.length;
    while (cursor + 2 <= end) {
        u8 type = cursor[0];
        u8 length = cursor[1];
        if (length < 2 || cursor + length > end) break;

        if (type == 1 && length >= 12 && g_info.io_apic_count < ACPI_MAX_IO_APICS) {
            AcpiIoApic *io = &g_info.io_apics[g_info.io_apic_count++];
            io->id = cursor[2];
            io->address = *(const u32 *)(const void *)(cursor + 4);
            io->gsi_base = *(const u32 *)(const void *)(cursor + 8);
        } else if (type == 2 && length >= 10) {
            u8 bus = cursor[2];
            u8 source = cursor[3];
            if (bus == 0 && source == 1) {
                g_info.keyboard_gsi = *(const u32 *)(const void *)(cursor + 4);
                g_info.keyboard_flags = *(const u16 *)(const void *)(cursor + 8);
            }
        } else if (type == 5 && length >= 12) {
            g_info.lapic_address = *(const u64 *)(const void *)(cursor + 4);
        }
        cursor += length;
    }

    g_info.madt_valid = g_info.lapic_address != 0 && g_info.io_apic_count != 0;
}

static void parse_fadt(const AcpiSdtHeader *header) {
    if (!header) return;
    const u8 *bytes = (const u8 *)header;

    /* ACPI 2.0+ FADT offset 109: IA-PC Boot Architecture Flags.
       Bit 1 tells an OS whether the legacy 8042 controller is present. */
    if (header->revision >= 2 && header->length >= 111) {
        u16 boot_architecture = *(const u16 *)(const void *)(bytes + 109);
        g_info.i8042_known = true;
        g_info.i8042_present = (boot_architecture & (1U << 1)) != 0;
    }

    /* ACPI 2.0+ FADT offsets: Flags=112, ResetReg=116, ResetValue=128. */
    if (header->length < 129) return;
    u32 flags = *(const u32 *)(const void *)(bytes + 112);
    if (!(flags & ACPI_FADT_RESET_SUPPORTED)) return;
    const GenericAddress *reg = (const GenericAddress *)(const void *)(bytes + 116);
    if (!reg->address || (reg->address_space != 0 && reg->address_space != 1)) return;
    g_info.reset_supported = true;
    g_info.reset_address_space = reg->address_space;
    g_info.reset_access_size = reg->access_size;
    g_info.reset_address = reg->address;
    g_info.reset_value = bytes[128];
}

bool acpi_init(u64 rsdp_address) {
    k_memset(&g_info, 0, sizeof(g_info));
    g_info.rsdp_address = rsdp_address;
    g_info.keyboard_gsi = 1;
    if (!rsdp_address) return false;

    const RsdpV1 *rsdp = (const RsdpV1 *)(u64)rsdp_address;
    if (!bytes_equal(rsdp->signature, "RSD PTR ", 8) || !checksum_ok(rsdp, sizeof(RsdpV1))) return false;
    if (rsdp->revision >= 2) {
        const RsdpV2 *v2 = (const RsdpV2 *)rsdp;
        if (v2->length < sizeof(RsdpV2) || v2->length > 4096 || !checksum_ok(v2, v2->length)) return false;
    }

    g_info.valid = true;
    parse_madt(find_table(rsdp, "APIC"));
    parse_fadt(find_table(rsdp, "FACP"));
    return true;
}

const AcpiInfo *acpi_get(void) {
    return &g_info;
}

bool acpi_try_reset(void) {
    if (!g_info.reset_supported) return false;
    if (g_info.reset_address_space == 1 && g_info.reset_address <= 0xFFFF) {
        arch_out8((u16)g_info.reset_address, g_info.reset_value);
        return true;
    }
    if (g_info.reset_address_space == 0) {
        volatile u8 *reg = (volatile u8 *)(u64)g_info.reset_address;
        *reg = g_info.reset_value;
        return true;
    }
    return false;
}
