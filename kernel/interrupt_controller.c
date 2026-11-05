#include "interrupt_controller.h"
#include "arch.h"
#include "interrupts.h"
#include "lib.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2       (1ULL << 10)
#define APIC_BASE_MASK     0x000FFFFFFFFFF000ULL

#define LAPIC_TPR          0x080
#define LAPIC_EOI          0x0B0
#define LAPIC_SIVR         0x0F0
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_LVT_THERMAL  0x330
#define LAPIC_LVT_PERF     0x340
#define LAPIC_LVT_LINT0    0x350
#define LAPIC_LVT_LINT1    0x360
#define LAPIC_LVT_ERROR    0x370
#define LAPIC_LVT_MASKED   (1U << 16)
#define LAPIC_LVT_EXTINT   (7U << 8)

#define KEYBOARD_VECTOR 0x21U
#define SPURIOUS_VECTOR 0xFFU
#define CONTROLLER_RFLAGS_IF (1ULL << 9)
#define IOAPIC_DELIVERY_STATUS (1U << 12)
#define IOAPIC_REMOTE_IRR      (1U << 14)

static InterruptControllerInfo g_info;
static u64 g_lapic_base;
static const AcpiInfo *g_acpi;

static u64 controller_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void controller_irq_restore(u64 flags) {
    if (flags & CONTROLLER_RFLAGS_IF) interrupts_enable();
}

static void io_wait(void) {
    arch_out8(0x80, 0);
}

static void pic_mask_all(void) {
    arch_out8(PIC1_DATA, 0xFF); 
    arch_out8(PIC2_DATA, 0xFF);
}

static void pic_initialize_keyboard(void) {
    arch_out8(PIC1_COMMAND, 0x11); io_wait();
    arch_out8(PIC2_COMMAND, 0x11); io_wait();
    arch_out8(PIC1_DATA, 0x20); io_wait();
    arch_out8(PIC2_DATA, 0x28); io_wait();
    arch_out8(PIC1_DATA, 0x04); io_wait();
    arch_out8(PIC2_DATA, 0x02); io_wait();
    arch_out8(PIC1_DATA, 0x01); io_wait();
    arch_out8(PIC2_DATA, 0x01); io_wait();
    arch_out8(PIC1_DATA, 0xFD); /* Unmask IRQ1 only. */ 
    arch_out8(PIC2_DATA, 0xFF);
}

static bool cpu_has_apic(void) {
    u32 a, b, c, d;
    arch_cpuid(1, 0, &a, &b, &c, &d);
    return (d & (1U << 9)) != 0;
}

static u32 lapic_read(u32 offset) {
    if (g_info.x2apic) return (u32)arch_read_msr(0x800U + (offset >> 4));
    return *(volatile u32 *)(u64)(g_lapic_base + offset);
}

static void lapic_write(u32 offset, u32 value) {
    if (g_info.x2apic) {
        arch_write_msr(0x800U + (offset >> 4), value);
        return;
    }
    *(volatile u32 *)(u64)(g_lapic_base + offset) = value;
    (void)*(volatile u32 *)(u64)(g_lapic_base + offset);
}

static bool lapic_initialize(const AcpiInfo *acpi) {
    if (!cpu_has_apic()) return false;
    u64 apic_base = arch_read_msr(IA32_APIC_BASE_MSR);
    if (!(apic_base & APIC_BASE_ENABLE)) {
        apic_base |= APIC_BASE_ENABLE;
        arch_write_msr(IA32_APIC_BASE_MSR, apic_base);
    }

    g_info.x2apic = (apic_base & APIC_BASE_X2) != 0;
    g_lapic_base = acpi->lapic_address ? acpi->lapic_address : (apic_base & APIC_BASE_MASK);
    if (!g_info.x2apic && !g_lapic_base) return false;

    g_info.local_apic_id = g_info.x2apic ? lapic_read(0x020) : (lapic_read(0x020) >> 24);
    lapic_write(LAPIC_TPR, 0); 
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | 0xFEU); 
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED | 0xFEU); 
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED | 0xFEU); 
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED); 
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED | 0xFEU); 
    lapic_write(LAPIC_SIVR, (1U << 8) | SPURIOUS_VECTOR);
    return true;
}

static u32 ioapic_read(u32 base, u8 reg) {
    volatile u32 *select = (volatile u32 *)(u64)base;
    volatile u32 *window = (volatile u32 *)(u64)(base + 0x10);
    *select = reg;
    return *window;
}

static void ioapic_write(u32 base, u8 reg, u32 value) {
    volatile u32 *select = (volatile u32 *)(u64)base;
    volatile u32 *window = (volatile u32 *)(u64)(base + 0x10);
    *select = reg;
    *window = value;
}

static u32 ioapic_redirection_count(u32 base) {
    return ((ioapic_read(base, 1) >> 16) & 0xFFU) + 1U;
}

static bool ioapic_find_gsi(u32 gsi, const AcpiIoApic **out_io, u32 *out_pin) {
    if (!g_acpi || !out_io || !out_pin) return false;
    *out_io = 0;
    *out_pin = 0U;
    for (u32 i = 0U; i < g_acpi->io_apic_count; ++i) {
        const AcpiIoApic *io = &g_acpi->io_apics[i];
        u32 count = ioapic_redirection_count(io->address);
        if (gsi >= io->gsi_base && gsi - io->gsi_base < count) {
            *out_io = io;
            *out_pin = gsi - io->gsi_base;
            return true;
        }
    }
    return false;
}

static bool route_flags_valid(u16 flags) {
    u32 polarity = flags & 3U;
    u32 trigger = (flags >> 2) & 3U;
    return polarity != 2U && trigger != 2U;
}

static u32 route_low(u16 flags, u8 vector) {
    u32 low = vector;
    if ((flags & 3U) == 3U) low |= (1U << 13);
    if (((flags >> 2) & 3U) == 3U) low |= (1U << 15);
    return low;
}

static void ioapic_mask_all(const AcpiIoApic *io) {
    u32 count = ioapic_redirection_count(io->address);
    for (u32 pin = 0; pin < count; ++pin) {
        u8 low = (u8)(0x10 + pin * 2);
        ioapic_write(io->address, (u8)(low + 1), 0); 
        ioapic_write(io->address, low, LAPIC_LVT_MASKED | 0x20U);
    }
}

static bool ioapic_route_gsi(const AcpiInfo *acpi, u32 gsi, u16 flags, u8 vector) {
    /* The legacy I/O APIC destination field is eight bits without interrupt remapping. */
    if (!acpi || g_info.local_apic_id > 255 || vector < 0x20U || vector == SPURIOUS_VECTOR) return false;

    const AcpiIoApic *target = 0;
    u32 pin = 0;
    for (u32 i = 0; i < acpi->io_apic_count; ++i) {
        const AcpiIoApic *io = &acpi->io_apics[i];
        u32 count = ioapic_redirection_count(io->address);
        if (gsi >= io->gsi_base && gsi - io->gsi_base < count) {
            target = io;
            pin = gsi - io->gsi_base;
            break;
        }
    }
    if (!target) return false;

    u32 polarity = flags & 3U;
    u32 trigger = (flags >> 2) & 3U;
    if (polarity == 2U || trigger == 2U) return false; /* Reserved ACPI encodings. */

    u32 low = vector;
    if (polarity == 3U) low |= (1U << 13); /* Active low. */
    if (trigger == 3U) low |= (1U << 15);  /* Level triggered. */
    u32 high = (g_info.local_apic_id & 0xFFU) << 24;
    u8 reg = (u8)(0x10 + pin * 2);

    /* Keep the entry masked while changing its destination and mode. */
    ioapic_write(target->address, reg, low | LAPIC_LVT_MASKED);
    ioapic_write(target->address, (u8)(reg + 1), high);
    ioapic_write(target->address, reg, low);
    return true;
}

static bool ioapic_route_legacy_irq(const AcpiInfo *acpi, u8 irq, u8 vector) {
    if (!acpi || irq >= ACPI_LEGACY_IRQ_COUNT) return false;
    const AcpiLegacyIrq *route = &acpi->legacy_irqs[irq];
    return ioapic_route_gsi(acpi, route->gsi, route->flags, vector);
}

bool interrupt_controller_init(const AcpiInfo *acpi) {
    g_acpi = 0;
    g_info.mode = INTERRUPT_CONTROLLER_NONE;
    g_info.keyboard_gsi = 1;
    g_info.local_apic_id = 0;
    g_info.x2apic = false;

    bool lapic_ready = false;
    if (acpi && acpi->madt_valid) {
        g_info.keyboard_gsi = acpi->keyboard_gsi;
        lapic_ready = lapic_initialize(acpi);
        if (lapic_ready) {
            for (u32 i = 0; i < acpi->io_apic_count; ++i) ioapic_mask_all(&acpi->io_apics[i]);
            if (ioapic_route_legacy_irq(acpi, 1U, KEYBOARD_VECTOR)) {
                pic_mask_all();
                g_acpi = acpi;
                g_info.mode = INTERRUPT_CONTROLLER_APIC;
                return true;
            }
        }
    }

    /* On PC/AT-compatible systems, route the legacy 8259 through LINT0 in
       ExtINT mode if a local APIC was enabled before falling back to the PIC. */
    if (lapic_ready) {
        if (acpi && !(acpi->madt_flags & 1U)) return false;
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_EXTINT);
    }
    pic_initialize_keyboard();
    g_info.mode = INTERRUPT_CONTROLLER_PIC;
    g_info.keyboard_gsi = 1;
    return true;
}

void interrupt_controller_eoi(u8 vector) {
    if (g_info.mode == INTERRUPT_CONTROLLER_APIC) {
        lapic_write(LAPIC_EOI, 0);
    } else if (g_info.mode == INTERRUPT_CONTROLLER_PIC) {
        if (vector >= 0x28) arch_out8(PIC2_COMMAND, PIC_EOI);
        arch_out8(PIC1_COMMAND, PIC_EOI);
    }
}

InterruptControllerInfo interrupt_controller_info(void) {
    return g_info;
}

const char *interrupt_controller_name(void) {
    if (g_info.mode == INTERRUPT_CONTROLLER_APIC) return g_info.x2apic ? "X2APIC + IOAPIC" : "XAPIC + IOAPIC";
    if (g_info.mode == INTERRUPT_CONTROLLER_PIC) return "8259 PIC FALLBACK";
    return "NONE";
}

static bool pic_unmask_irq(u8 irq) {
    if (irq >= 16U) return false;
    if (irq < 8U) {
        u8 mask = arch_in8(PIC1_DATA);
        mask &= (u8)~(1U << irq);
        arch_out8(PIC1_DATA, mask);
        return true;
    }
    /* Slave IRQs require the master cascade line, IRQ2, to be enabled as well. */
    u8 master_mask = arch_in8(PIC1_DATA);
    master_mask &= (u8)~(1U << 2);
    arch_out8(PIC1_DATA, master_mask);

    u8 slave_mask = arch_in8(PIC2_DATA);
    slave_mask &= (u8)~(1U << (irq - 8U));
    arch_out8(PIC2_DATA, slave_mask);
    return true;
}

bool interrupt_controller_enable_legacy_irq(u8 irq, u8 vector) {
    if (irq >= ACPI_LEGACY_IRQ_COUNT || vector < 0x20U || vector == SPURIOUS_VECTOR) return false;

    if (g_info.mode == INTERRUPT_CONTROLLER_PIC) {
        if (vector != (u8)(0x20U + irq)) return false;
        return pic_unmask_irq(irq);
    }

    if (g_info.mode == INTERRUPT_CONTROLLER_APIC) {
        return g_acpi && ioapic_route_legacy_irq(g_acpi, irq, vector);
    }

    return false;
}

bool interrupt_controller_legacy_irq_route(u8 irq, u32 *out_gsi, u16 *out_flags) {
    if (!out_gsi || !out_flags) return false;
    *out_gsi = 0U;
    *out_flags = 0U;
    u64 flags = controller_irq_save();
    bool valid = g_info.mode == INTERRUPT_CONTROLLER_APIC && g_acpi &&
        irq < ACPI_LEGACY_IRQ_COUNT;
    if (valid) {
        *out_gsi = g_acpi->legacy_irqs[irq].gsi;
        *out_flags = g_acpi->legacy_irqs[irq].flags;
    }
    controller_irq_restore(flags);
    return valid;
}

static bool lease_current_locked(const InterruptControllerGsiLease *lease, bool *out_masked) {
    if (out_masked) *out_masked = true;
    if (!lease || !lease->valid || g_info.mode != INTERRUPT_CONTROLLER_APIC || !g_acpi)
        return false;
    const AcpiIoApic *io = 0;
    u32 pin = 0U;
    if (!ioapic_find_gsi(lease->gsi, &io, &pin) || !io ||
        io->address != lease->ioapic_address ||
        (u8)(0x10U + pin * 2U) != lease->redirection_register) return false;
    u32 low = ioapic_read(io->address, lease->redirection_register);
    u32 high = ioapic_read(io->address, (u8)(lease->redirection_register + 1U));
    u32 compare_mask = ~(LAPIC_LVT_MASKED | IOAPIC_DELIVERY_STATUS | IOAPIC_REMOTE_IRR);
    if ((low & compare_mask) != (lease->routed_low & compare_mask) ||
        high != lease->routed_high) return false;
    if (out_masked) *out_masked = (low & LAPIC_LVT_MASKED) != 0U;
    return true;
}

bool interrupt_controller_gsi_lease_acquire(u32 gsi, u16 route_flags, u8 vector,
    InterruptControllerGsiLease *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    u64 flags = controller_irq_save();
    bool acquired = false;
    if (g_info.mode != INTERRUPT_CONTROLLER_APIC || !g_acpi ||
        g_info.local_apic_id > 255U || vector < 0x20U || vector == SPURIOUS_VECTOR ||
        !route_flags_valid(route_flags)) goto done;

    const AcpiIoApic *io = 0;
    u32 pin = 0U;
    if (!ioapic_find_gsi(gsi, &io, &pin) || !io || pin > 119U) goto done;
    u8 reg = (u8)(0x10U + pin * 2U);
    u32 saved_low = ioapic_read(io->address, reg);
    u32 saved_high = ioapic_read(io->address, (u8)(reg + 1U));

    /* Never steal a source that is live or still has an in-flight level IRQ. */
    if (!(saved_low & LAPIC_LVT_MASKED) ||
        (saved_low & (IOAPIC_DELIVERY_STATUS | IOAPIC_REMOTE_IRR))) goto done;

    u32 low = route_low(route_flags, vector);
    u32 high = (g_info.local_apic_id & 0xFFU) << 24;
    ioapic_write(io->address, reg, low | LAPIC_LVT_MASKED);
    ioapic_write(io->address, (u8)(reg + 1U), high);
    ioapic_write(io->address, reg, low | LAPIC_LVT_MASKED);

    out->ioapic_address = io->address;
    out->gsi = gsi;
    out->saved_low = saved_low;
    out->saved_high = saved_high;
    out->routed_low = low;
    out->routed_high = high;
    out->redirection_register = reg;
    out->vector = vector;
    out->valid = true;
    acquired = true;

done:
    controller_irq_restore(flags);
    return acquired;
}

bool interrupt_controller_gsi_lease_mask(InterruptControllerGsiLease *lease) {
    u64 flags = controller_irq_save();
    bool masked = false;
    bool current_masked = true;
    if (lease_current_locked(lease, &current_masked)) {
        if (!current_masked)
            ioapic_write(lease->ioapic_address, lease->redirection_register,
                lease->routed_low | LAPIC_LVT_MASKED);
        masked = true;
    }
    controller_irq_restore(flags);
    return masked;
}

bool interrupt_controller_gsi_lease_unmask(InterruptControllerGsiLease *lease) {
    u64 flags = controller_irq_save();
    bool unmasked = false;
    bool current_masked = true;
    if (lease_current_locked(lease, &current_masked)) {
        if (current_masked)
            ioapic_write(lease->ioapic_address, lease->redirection_register,
                lease->routed_low);
        unmasked = true;
    }
    controller_irq_restore(flags);
    return unmasked;
}

bool interrupt_controller_gsi_lease_query(const InterruptControllerGsiLease *lease,
    bool *out_masked) {
    if (!out_masked) return false;
    *out_masked = true;
    u64 flags = controller_irq_save();
    bool current_masked = true;
    bool valid = lease_current_locked(lease, &current_masked);
    if (valid) *out_masked = current_masked;
    controller_irq_restore(flags);
    return valid;
}

bool interrupt_controller_gsi_lease_release(InterruptControllerGsiLease *lease) {
    if (!lease) return false;
    u64 flags = controller_irq_save();
    bool current_masked = false;
    bool released = lease_current_locked(lease, &current_masked);
    if (released) {
        if (!current_masked)
            ioapic_write(lease->ioapic_address, lease->redirection_register,
                lease->routed_low | LAPIC_LVT_MASKED);
        ioapic_write(lease->ioapic_address, (u8)(lease->redirection_register + 1U),
            lease->saved_high);
        ioapic_write(lease->ioapic_address, lease->redirection_register,
            lease->saved_low);
        k_memset(lease, 0, sizeof(*lease));
    }
    controller_irq_restore(flags);
    return released;
}
