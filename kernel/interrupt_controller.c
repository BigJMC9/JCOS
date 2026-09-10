#include "interrupt_controller.h"
#include "arch.h"

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

static InterruptControllerInfo g_info;
static u64 g_lapic_base;

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

static void ioapic_mask_all(const AcpiIoApic *io) {
    u32 count = ioapic_redirection_count(io->address);
    for (u32 pin = 0; pin < count; ++pin) {
        u8 low = (u8)(0x10 + pin * 2);
        ioapic_write(io->address, (u8)(low + 1), 0); 
        ioapic_write(io->address, low, LAPIC_LVT_MASKED | 0x20U);
    }
}

static bool ioapic_route_keyboard(const AcpiInfo *acpi) {
    /* The legacy I/O APIC destination field is eight bits without interrupt remapping. */
    if (g_info.local_apic_id > 255) return false;
    const AcpiIoApic *target = 0;
    u32 pin = 0;
    for (u32 i = 0; i < acpi->io_apic_count; ++i) {
        const AcpiIoApic *io = &acpi->io_apics[i];
        u32 count = ioapic_redirection_count(io->address);
        if (acpi->keyboard_gsi >= io->gsi_base && acpi->keyboard_gsi - io->gsi_base < count) {
            target = io;
            pin = acpi->keyboard_gsi - io->gsi_base;
            break;
        }
    }
    if (!target) return false;

    u32 polarity = acpi->keyboard_flags & 3U;
    u32 trigger = (acpi->keyboard_flags >> 2) & 3U;
    u32 low = KEYBOARD_VECTOR;
    if (polarity == 3U) low |= (1U << 13); /* Active low. */
    if (trigger == 3U) low |= (1U << 15);  /* Level triggered. */
    u32 high = (g_info.local_apic_id & 0xFFU) << 24;
    u8 reg = (u8)(0x10 + pin * 2);
    ioapic_write(target->address, (u8)(reg + 1), high); 
    ioapic_write(target->address, reg, low);
    return true;
}

bool interrupt_controller_init(const AcpiInfo *acpi) {
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
            if (ioapic_route_keyboard(acpi)) {
                pic_mask_all();
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

bool interrupt_controller_unmask_legacy_irq(u8 irq) {
    if (g_info.mode != INTERRUPT_CONTROLLER_PIC || irq >= 16U) return false;
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