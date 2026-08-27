#ifndef JA_OS_ACPI_H
#define JA_OS_ACPI_H

#include "types.h"

#define ACPI_MAX_IO_APICS 8U

typedef struct {
    u8 id;
    u32 address;
    u32 gsi_base;
} AcpiIoApic;

typedef struct {
    bool valid;
    bool madt_valid;
    u64 rsdp_address;
    u64 lapic_address;
    u32 madt_flags;

    AcpiIoApic io_apics[ACPI_MAX_IO_APICS];
    u32 io_apic_count;
    u32 keyboard_gsi;
    u16 keyboard_flags;

    bool i8042_known;
    bool i8042_present;

    bool reset_supported;
    u8 reset_address_space;
    u8 reset_access_size;
    u8 reset_value;
    u64 reset_address;
} AcpiInfo;

bool acpi_init(u64 rsdp_address);
const AcpiInfo *acpi_get(void);
bool acpi_try_reset(void);

#endif
