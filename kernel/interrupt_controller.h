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

#endif
