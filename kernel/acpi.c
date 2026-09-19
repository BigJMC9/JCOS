#include "acpi.h"
#include "arch.h"
#include "lib.h"

#define ACPI_FADT_RESET_SUPPORTED (1U << 10)
#define ACPI_PM1_SCI_EN           (1U << 0)
#define ACPI_PM1_SLP_TYP_MASK     (7U << 10)
#define ACPI_PM1_SLP_EN           (1U << 13)
#define ACPI_MODE_ENABLE_SPINS    1000000U

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

static bool register_from_gas(AcpiRegister *out, const GenericAddress *gas) {
    if (!out || !gas || !gas->address) return false;
    if (gas->address_space != ACPI_ADDRESS_SPACE_SYSTEM_MEMORY &&
        gas->address_space != ACPI_ADDRESS_SPACE_SYSTEM_IO) return false;

    out->address_space = gas->address_space;
    out->bit_width = gas->bit_width;
    out->bit_offset = gas->bit_offset;
    out->access_size = gas->access_size;
    out->address = gas->address;
    return true;
}

static void register_from_legacy_io(AcpiRegister *out, u32 address, u8 byte_width) {
    if (!out || !address || byte_width < 2U) return;
    out->address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    out->bit_width = (u8)(byte_width * 8U);
    out->bit_offset = 0;
    out->access_size = 2U;
    out->address = address;
}

static bool register_supports_u16(const AcpiRegister *reg) {
    if (!reg || !reg->address || reg->bit_offset) return false;
    if (reg->bit_width && reg->bit_width < 16U) return false;
    if (reg->access_size && reg->access_size != 2U) return false;
    if (reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO) return reg->address <= 0xFFFFULL;
    return reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY;
}

static bool register_read_u16(const AcpiRegister *reg, u16 *value) {
    if (!value || !register_supports_u16(reg)) return false;
    if (reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        *value = arch_in16((u16)reg->address);
        return true;
    }
    *value = *(volatile const u16 *)(u64)reg->address;
    return true;
}

static bool register_write_u16(const AcpiRegister *reg, u16 value) {
    if (!register_supports_u16(reg)) return false;
    if (reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        arch_out16((u16)reg->address, value);
        return true;
    }
    *(volatile u16 *)(u64)reg->address = value;
    return true;
}

static bool aml_package_length(const u8 **cursor, const u8 *end, u32 *out_length) {
    if (!cursor || !*cursor || !out_length || *cursor >= end) return false;
    const u8 *p = *cursor;
    u8 lead = *p++;
    u8 follow = (u8)(lead >> 6);
    u32 length = follow ? (u32)(lead & 0x0FU) : (u32)(lead & 0x3FU);

    if ((u64)(end - p) < follow) return false;
    for (u8 i = 0; i < follow; ++i) length |= (u32)(*p++) << (4U + 8U * i);

    *cursor = p;
    *out_length = length;
    return true;
}

static bool aml_integer(const u8 **cursor, const u8 *end, u64 *out_value) {
    if (!cursor || !*cursor || !out_value || *cursor >= end) return false;
    const u8 *p = *cursor;
    u8 opcode = *p++;
    u64 value = 0;

    if (opcode == 0x00U) value = 0;
    else if (opcode == 0x01U) value = 1;
    else if (opcode == 0xFFU) value = ~0ULL;
    else if (opcode == 0x0AU) {
        if (p >= end) return false;
        value = *p++;
    } else if (opcode == 0x0BU) {
        if ((u64)(end - p) < 2ULL) return false;
        value = (u64)p[0] | ((u64)p[1] << 8);
        p += 2;
    } else if (opcode == 0x0CU) {
        if ((u64)(end - p) < 4ULL) return false;
        for (u32 i = 0; i < 4U; ++i) value |= (u64)p[i] << (i * 8U);
        p += 4;
    } else if (opcode == 0x0EU) {
        if ((u64)(end - p) < 8ULL) return false;
        for (u32 i = 0; i < 8U; ++i) value |= (u64)p[i] << (i * 8U);
        p += 8;
    } else return false;

    *cursor = p;
    *out_value = value;
    return true;
}

static bool parse_s5(const AcpiSdtHeader *dsdt) {
    if (!sdt_valid(dsdt) || !bytes_equal(dsdt->signature, "DSDT", 4)) return false;
    const u8 *begin = (const u8 *)dsdt + sizeof(AcpiSdtHeader);
    const u8 *end = (const u8 *)dsdt + dsdt->length;

    for (const u8 *p = begin; p < end; ++p) {
        if (*p != 0x08U) continue; /* NameOp */
        const u8 *q = p + 1;

        while (q < end && (*q == 0x5CU || *q == 0x5EU)) ++q; /* root / parent prefixes */
        if ((u64)(end - q) < 5ULL) continue;
        if (q[0] != '_' || q[1] != 'S' || q[2] != '5' || q[3] != '_') continue;
        q += 4;
        if (q >= end || *q++ != 0x12U) continue; /* PackageOp */

        const u8 *package_start = q;
        u32 package_length = 0;
        if (!aml_package_length(&q, end, &package_length) || !package_length) continue;
        if ((u64)(end - package_start) < package_length) continue;
        const u8 *package_end = package_start + package_length;
        if (q >= package_end || *q++ < 2U) continue;

        u64 type_a = 0;
        u64 type_b = 0;
        if (!aml_integer(&q, package_end, &type_a) || !aml_integer(&q, package_end, &type_b)) continue;
        if (type_a > 7ULL || type_b > 7ULL) continue;

        g_info.s5_valid = true;
        g_info.s5_type_a = (u8)type_a;
        g_info.s5_type_b = (u8)type_b;
        return true;
    }
    return false;
}

static void parse_madt(const AcpiSdtHeader *header) {
    if (!header || header->length < sizeof(Madt)) return;
    const Madt *madt = (const Madt *)header;
    g_info.lapic_address = madt->local_apic_address;
    g_info.madt_flags = madt->flags;

    for (u32 irq = 0; irq < ACPI_LEGACY_IRQ_COUNT; ++irq) {
        g_info.legacy_irqs[irq].gsi = irq;
        g_info.legacy_irqs[irq].flags = 0;
    }
    g_info.keyboard_gsi = g_info.legacy_irqs[1U].gsi;
    g_info.keyboard_flags = g_info.legacy_irqs[1U].flags;

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
            if (bus == 0 && source < ACPI_LEGACY_IRQ_COUNT) {
                g_info.legacy_irqs[source].gsi = *(const u32 *)(const void *)(cursor + 4);
                g_info.legacy_irqs[source].flags = *(const u16 *)(const void *)(cursor + 8);
                if (source == 1U) {
                    g_info.keyboard_gsi = g_info.legacy_irqs[source].gsi;
                    g_info.keyboard_flags = g_info.legacy_irqs[source].flags;
                }
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

    if (header->length >= 53U) {
        g_info.smi_command = *(const u32 *)(const void *)(bytes + 48);
        g_info.acpi_enable_command = bytes[52];
    }

    u32 legacy_pm1a = 0;
    u32 legacy_pm1b = 0;
    u8 pm1_control_length = 0;
    if (header->length >= 90U) {
        legacy_pm1a = *(const u32 *)(const void *)(bytes + 64);
        legacy_pm1b = *(const u32 *)(const void *)(bytes + 68);
        pm1_control_length = bytes[89];
    }

    if (header->revision >= 2 && header->length >= 111U) {
        u16 boot_architecture = *(const u16 *)(const void *)(bytes + 109);
        g_info.i8042_known = true;
        g_info.i8042_present = (boot_architecture & (1U << 1)) != 0;
    }

    u64 dsdt_address = 0;
    if (header->length >= 44U) dsdt_address = *(const u32 *)(const void *)(bytes + 40);
    if (header->length >= 148U) {
        u64 extended_dsdt = *(const u64 *)(const void *)(bytes + 140);
        if (extended_dsdt) dsdt_address = extended_dsdt;
    }

    if (header->length >= 184U) {
        const GenericAddress *reg = (const GenericAddress *)(const void *)(bytes + 172);
        (void)register_from_gas(&g_info.pm1a_control, reg);
    }
    if (header->length >= 196U) {
        const GenericAddress *reg = (const GenericAddress *)(const void *)(bytes + 184);
        (void)register_from_gas(&g_info.pm1b_control, reg);
    }

    if (!g_info.pm1a_control.address) register_from_legacy_io(&g_info.pm1a_control, legacy_pm1a, pm1_control_length);
    if (!g_info.pm1b_control.address) register_from_legacy_io(&g_info.pm1b_control, legacy_pm1b, pm1_control_length);

    if (dsdt_address) (void)parse_s5((const AcpiSdtHeader *)(u64)dsdt_address);
    bool pm1b_supported = !g_info.pm1b_control.address || register_supports_u16(&g_info.pm1b_control);
    g_info.poweroff_supported = g_info.s5_valid && register_supports_u16(&g_info.pm1a_control) && pm1b_supported;

    if (header->length >= 129U) {
        u32 flags = *(const u32 *)(const void *)(bytes + 112);
        if (flags & ACPI_FADT_RESET_SUPPORTED) {
            const GenericAddress *reg = (const GenericAddress *)(const void *)(bytes + 116);
            if (reg->address && (reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY ||
                    reg->address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO)) {
                g_info.reset_supported = true;
                g_info.reset_address_space = reg->address_space;
                g_info.reset_access_size = reg->access_size;
                g_info.reset_address = reg->address;
                g_info.reset_value = bytes[128];
            }
        }
    }
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
    if (!g_info.reset_supported || !g_info.reset_address) return false;
    u8 access = g_info.reset_access_size ? g_info.reset_access_size : 1U;

    if (g_info.reset_address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (g_info.reset_address > 0xFFFFULL) return false;
        u16 port = (u16)g_info.reset_address;
        if (access == 1U) arch_out8(port, g_info.reset_value);
        else if (access == 2U) arch_out16(port, (u16)g_info.reset_value);
        else if (access == 3U) arch_out32(port, (u32)g_info.reset_value);
        else return false;
        return true;
    }

    if (g_info.reset_address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY) {
        if (access == 1U) *(volatile u8 *)(u64)g_info.reset_address = g_info.reset_value;
        else if (access == 2U) *(volatile u16 *)(u64)g_info.reset_address = g_info.reset_value;
        else if (access == 3U) *(volatile u32 *)(u64)g_info.reset_address = g_info.reset_value;
        else if (access == 4U) *(volatile u64 *)(u64)g_info.reset_address = g_info.reset_value;
        else return false;
        return true;
    }
    return false;
}

bool acpi_poweroff_supported(void) {
    if (!g_info.poweroff_supported) return false;
    u16 control = 0;
    if (!register_read_u16(&g_info.pm1a_control, &control)) return false;
    if (control & ACPI_PM1_SCI_EN) return true;
    return g_info.smi_command && g_info.smi_command <= 0xFFFFU && g_info.acpi_enable_command;
}

static bool acpi_enable_mode(void) {
    u16 control = 0;
    if (!register_read_u16(&g_info.pm1a_control, &control)) return false;
    if (control & ACPI_PM1_SCI_EN) return true;
    if (!g_info.smi_command || g_info.smi_command > 0xFFFFU || !g_info.acpi_enable_command) return false;

    arch_out8((u16)g_info.smi_command, g_info.acpi_enable_command);
    for (u32 i = 0; i < ACPI_MODE_ENABLE_SPINS; ++i) {
        if (register_read_u16(&g_info.pm1a_control, &control) && (control & ACPI_PM1_SCI_EN)) return true;
        arch_pause();
    }
    return false;
}

bool acpi_try_poweroff(void) {
    if (!g_info.poweroff_supported || !acpi_enable_mode()) return false;

    u16 control_a = 0;
    u16 control_b = 0;
    if (!register_read_u16(&g_info.pm1a_control, &control_a)) return false;
    bool have_b = g_info.pm1b_control.address != 0;
    if (have_b && !register_read_u16(&g_info.pm1b_control, &control_b)) return false;

    /* Program both SLP_TYP fields before asserting SLP_EN on either block. */
    control_a &= (u16)~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
    control_a |= (u16)((u16)g_info.s5_type_a << 10);

    if (have_b) {
        control_b &= (u16)~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
        control_b |= (u16)((u16)g_info.s5_type_b << 10);
    }

    if (!register_write_u16(&g_info.pm1a_control, control_a)) return false;
    if (have_b && !register_write_u16(&g_info.pm1b_control, control_b)) return false;

    /* Arm PM1B first so PM1A, the mandatory block, is the final trigger. */
    if (have_b && !register_write_u16(&g_info.pm1b_control, (u16)(control_b | ACPI_PM1_SLP_EN))) return false;
    return register_write_u16(&g_info.pm1a_control, (u16)(control_a | ACPI_PM1_SLP_EN));
}
