#ifndef JA_OS_INTERRUPT_CONTROLLER_H
#define JA_OS_INTERRUPT_CONTROLLER_H

#include "types.h"
#include "acpi.h"

typedef enum {
    INTERRUPT_CONTROLLER_NONE = 0,
    INTERRUPT_CONTROLLER_PIC,
    INTERRUPT_CONTROLLER_APIC
} InterruptControllerMode;

typedef struct {
    InterruptControllerMode mode;
    u32 keyboard_gsi;
    u32 local_apic_id;
    bool x2apic;
} InterruptControllerInfo;

typedef struct {
    u32 ioapic_address;
    u32 gsi;
    u32 saved_low;
    u32 saved_high;
    u32 routed_low;
    u32 routed_high;
    u8 redirection_register;
    u8 vector;
    bool valid;
} InterruptControllerGsiLease;

bool interrupt_controller_init(const AcpiInfo *acpi);
void interrupt_controller_eoi(u8 vector);
InterruptControllerInfo interrupt_controller_info(void);
const char *interrupt_controller_name(void);

/*
 * Enable a legacy ISA IRQ through the active interrupt controller.
 * APIC mode applies the ACPI MADT interrupt-source override, if any.
 * PIC fallback accepts the fixed remapped vector for that IRQ.
 */
bool interrupt_controller_enable_legacy_irq(u8 irq, u8 vector);

/* Runtime device routing is APIC/IOAPIC-only. A lease may replace only an
 * already-masked GSI and restores the exact previous redirection entry. */
bool interrupt_controller_legacy_irq_route(u8 irq, u32 *out_gsi, u16 *out_flags);
bool interrupt_controller_gsi_lease_acquire(u32 gsi, u16 flags, u8 vector,
    InterruptControllerGsiLease *out);
bool interrupt_controller_gsi_lease_mask(InterruptControllerGsiLease *lease);
bool interrupt_controller_gsi_lease_unmask(InterruptControllerGsiLease *lease);
bool interrupt_controller_gsi_lease_query(const InterruptControllerGsiLease *lease,
    bool *out_masked);
bool interrupt_controller_gsi_lease_release(InterruptControllerGsiLease *lease);

#endif
