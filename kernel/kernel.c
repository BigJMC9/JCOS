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
#include "vfs.h"
#include "tar.h"
#include "pci.h"
#include "ahci.h"
#include "block.h"
#include "gpt.h"
#include "partition.h"
#include "fat32.h"
#include "splash.h"

static void boot_delay(void) {
    for (volatile u64 i = 0; i < 50000000ULL; ++i) arch_pause();
}

void kernel_main(BootInfo *boot) {
    arch_cli();
    (void)serial_init();

    if (!boot || boot->magic != BOOT_INFO_MAGIC || boot->version != BOOT_INFO_VERSION || boot->size < sizeof(BootInfo)) {
        serial_write("JA OS: invalid BootInfo.\n");
        cpu_halt_forever();
    }

    if (!framebuffer_init(boot) || !terminal_init()) { 
        serial_write("JA OS: framebuffer initialization failed.\n");
        cpu_halt_forever(); 
    }

    splash_show(); 
    splash_progress(5);

    idt_init();
    splash_progress(20);

    bool pmm_ok = pmm_init(boot);
    splash_progress(35);

    vfs_init();
    bool rootfs_ok = false;

    if (boot->initrd_base && boot->initrd_size) rootfs_ok = tar_mount((const void *)(u64)boot->initrd_base, boot->initrd_size);

    splash_progress(50);
    bool acpi_ok = acpi_init(boot->acpi_rsdp);
    splash_progress(65);
    pci_init();
    splash_progress(70);
    block_init();
    splash_progress(73);
    partition_init();
    splash_progress(75);
    bool ahci_ok = ahci_init();
    splash_progress(78);
    const AcpiInfo *acpi = acpi_get();
    bool controller_ok = interrupt_controller_init(acpi);
    splash_progress(80);
    bool gpt_ok = false;
    bool partitions_ok = false;
    bool fat32_ok = false;
    BlockDevice *boot_disk = block_find("sda");
    if (boot_disk) {
        gpt_ok = gpt_probe(boot_disk);
        if (gpt_ok) partitions_ok = gpt_register_partitions();
    }
    if (partitions_ok) {
        BlockDevice *esp = block_find("sda1");
        if (esp) fat32_ok = fat32_probe(esp);
    }
    splash_progress(85);
    bool keyboard_ok = (!acpi->i8042_known || acpi->i8042_present) ? ps2_init() : false;
    splash_progress(95);
    interrupts_enable();
    splash_progress(100);

    boot_delay();

    terminal_clear();
    terminal_set_color(terminal_accent_color());
    terminal_writeln("JA OS V5 - INTERACTIVE X86_64 KERNEL");
    terminal_set_color(terminal_default_color());
    terminal_writeln("UEFI BOOT SERVICES EXITED. FRAMEBUFFER + SERIAL CONSOLES ONLINE.");
    terminal_write("IDT: READY  PMM: "); terminal_write(pmm_ok ? "READY" : "FAILED");
    terminal_write("  ACPI: "); terminal_write(acpi_ok ? "READY" : "FALLBACK");
    terminal_write("  IRQ: ");
    terminal_writeln(controller_ok ? interrupt_controller_name() : "FAILED");
    terminal_write("PCI: "); terminal_write_u64(pci_device_count());
    terminal_writeln(" DEVICE(S)");
    terminal_write("BLOCK DEVICES: "); terminal_write_u64(block_device_count()); terminal_putchar('\n');
    terminal_write("AHCI: ");
    terminal_writeln( ahci_ok ? "DETECTED" : "NOT DETECTED");
    terminal_write("PS/2: "); terminal_write(keyboard_ok ? "DETECTED" : "NOT DETECTED");
    terminal_write("  COM1: ");
    terminal_writeln(serial_available() ? "READY" : "NOT DETECTED");
    terminal_write("ROOTFS: ");
    terminal_writeln(rootfs_ok ? "READY" : "FAILED");
    terminal_write("GPT: ");
    terminal_writeln(gpt_ok ? "READY" : "FAILED");
    terminal_write("  PARTITIONS: ");
    terminal_writeln(partitions_ok ? "READY" : "FAILED");
    terminal_write("FAT32: "); terminal_writeln( fat32_ok ? "READY" : "FAILED");
    terminal_writeln("TYPE help AND PRESS ENTER.");
    terminal_putchar('\n');

    shell_run(boot);
}