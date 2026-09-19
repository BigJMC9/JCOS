#ifndef JA_OS_ACPI_H
#define JA_OS_ACPI_H

#include "types.h"

#define ACPI_MAX_IO_APICS 8U
#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY 0U
#define ACPI_ADDRESS_SPACE_SYSTEM_IO     1U
#define ACPI_LEGACY_IRQ_COUNT             16U

typedef struct {
    u8 id;
    u32 address;
    u32 gsi_base;
} AcpiIoApic;

typedef struct {
    u32 gsi;
    u16 flags;
} AcpiLegacyIrq;

typedef struct {
    u8 address_space;
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;
    u64 address;
} AcpiRegister;

typedef struct {
    bool valid;
    bool madt_valid;
    u64 rsdp_address;
    u64 lapic_address;
    u32 madt_flags;

    AcpiIoApic io_apics[ACPI_MAX_IO_APICS];
    u32 io_apic_count;
    AcpiLegacyIrq legacy_irqs[ACPI_LEGACY_IRQ_COUNT];
    u32 keyboard_gsi;
    u16 keyboard_flags;

    bool i8042_known;
    bool i8042_present;

    bool reset_supported;
    u8 reset_address_space;
    u8 reset_access_size;
    u8 reset_value;
    u64 reset_address;

    bool poweroff_supported;
    bool s5_valid;
    u8 s5_type_a;
    u8 s5_type_b;
    u32 smi_command;
    u8 acpi_enable_command;
    AcpiRegister pm1a_control;
    AcpiRegister pm1b_control;
} AcpiInfo;

bool acpi_init(u64 rsdp_address);
const AcpiInfo *acpi_get(void);
bool acpi_try_reset(void);
bool acpi_poweroff_supported(void);
bool acpi_try_poweroff(void);

#endif
