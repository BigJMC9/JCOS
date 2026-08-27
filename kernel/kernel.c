#include "../include/boot_info.h"
#include "acpi.h"
#include "arch.h"
#include "framebuffer.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "pmm.h"
#include "ps2.h"
#include "serial.h"
#include "shell.h"
#include "terminal.h"

void kernel_main(BootInfo *boot) {
    arch_cli();
    (void)serial_init();

    if (!boot || boot->magic != BOOT_INFO_MAGIC ||
        boot->version != BOOT_INFO_VERSION || boot->size < sizeof(BootInfo)) {
        serial_write("JA OS: invalid BootInfo.\n");
        cpu_halt_forever();
    }

    if (!framebuffer_init(boot) || !terminal_init()) {
        serial_write("JA OS: framebuffer initialization failed.\n");
        cpu_halt_forever();
    }

    terminal_set_color(terminal_accent_color());
    terminal_writeln("JA OS V5 - INTERACTIVE X86_64 KERNEL");
    terminal_set_color(terminal_default_color());
    terminal_writeln("UEFI BOOT SERVICES EXITED. FRAMEBUFFER + SERIAL CONSOLES ONLINE.");

    idt_init();
    bool pmm_ok = pmm_init(boot);
    bool acpi_ok = acpi_init(boot->acpi_rsdp);
    const AcpiInfo *acpi = acpi_get();
    bool controller_ok = interrupt_controller_init(acpi);
    bool keyboard_ok = (!acpi->i8042_known || acpi->i8042_present) ? ps2_init() : false;
    interrupts_enable();

    terminal_write("IDT: READY  PMM: "); terminal_write(pmm_ok ? "READY" : "FAILED");
    terminal_write("  ACPI: "); terminal_write(acpi_ok ? "READY" : "FALLBACK");
    terminal_write("  IRQ: "); terminal_writeln(controller_ok ? interrupt_controller_name() : "FAILED");
    terminal_write("PS/2: "); terminal_write(keyboard_ok ? "DETECTED" : "NOT DETECTED");
    terminal_write("  COM1: "); terminal_writeln(serial_available() ? "READY" : "NOT DETECTED");
    terminal_writeln("TYPE help AND PRESS ENTER.");
    terminal_putchar('\n');

    shell_run(boot);
}