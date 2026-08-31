#include "../include/boot_info.h"
#include "acpi.h"
#include "arch.h"
#include "gdt.h"
#include "framebuffer.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "pmm.h"
#include "vmm.h"
#include "physmap.h"
#include "address_space.h"
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
#include "thread.h"
#include "splash.h"

#define CR4_LA57 (1ULL << 12)
#define DIRECT_MAP_MIN_PHYSICAL 0x100000ULL

static void boot_delay(void) {
    for (volatile u64 i = 0; i < 50000000ULL; ++i) arch_pause();
}

static bool map_physical_direct_map(VmPageMap *map, const BootInfo *boot) {
    if (!map || !boot || !boot->memory_map || !boot->memory_map_size || boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor)) return false;
    for (u64 offset = 0; offset + sizeof(BootMemoryDescriptor) <= boot->memory_map_size; offset += boot->memory_map_descriptor_size) {

        const BootMemoryDescriptor *descriptor = (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);

        if (descriptor->type != 7U) continue;
        if (!descriptor->number_of_pages) continue;
        if (descriptor->number_of_pages > ~0ULL / VM_PAGE_SIZE) return false;

        u64 bytes = descriptor->number_of_pages * VM_PAGE_SIZE;
        u64 physical = descriptor->physical_start;

        if (physical > ~0ULL - bytes) return false;

        u64 end = physical + bytes;

        /* PML4 must have no low alias and a valid physmap alias. */
        if (end <= DIRECT_MAP_MIN_PHYSICAL) continue;
        if (physical < DIRECT_MAP_MIN_PHYSICAL) physical = DIRECT_MAP_MIN_PHYSICAL;

        u64 size = end - physical;

        if (!size) continue;
        if (physical >= PHYS_MAP_SIZE) return false;
        if (size > PHYS_MAP_SIZE - physical) return false;

        u64 virtual_address = 0;

        if (!physmap_virtual_address(physical, &virtual_address)) return false;
        if (!vmm_map_range(map, virtual_address, physical, size, VM_WRITE)) return false;
    }

    return true;
}

static bool map_optional_range(VmPageMap *map, u64 base, u64 size) {
    if (!base || !size) return true;

    return
        vmm_identity_map_range(map, base, size, VM_WRITE);
}

static bool identity_mapping_valid(const VmPageMap *map, u64 address) {
    if (!map || !address) return false;

    u64 page = address & ~(VM_PAGE_SIZE - 1ULL);
    frame_t expected = phys_to_frame(page);

    if (expected == FRAME_INVALID) return false;

    frame_t actual = FRAME_INVALID;

    if (!vmm_query_page(map, page, &actual, 0)) return false;

    return
        actual == expected;
}

static bool build_kernel_page_map(VmPageMap *map, const BootInfo *boot, const AcpiInfo *acpi, const AhciInfo *ahci) {
    if (!map || !boot) return false;

    /* PMM bitmap must exist only through its physmap alias. */
    if (!map_physical_direct_map(map, boot)) {
        serial_write("PAGING: direct map build FAILED\n");
        goto fail;
    }

    serial_write("PAGING: direct map built\n");

    /* Explicit low identity mappings required by the currently running kernel follow. */
    if (!map_optional_range(map, (u64)boot, sizeof(BootInfo))) goto fail;
    if (!map_optional_range(map, boot->memory_map, boot->memory_map_size)) goto fail;
    if (!map_optional_range(map, boot->kernel_base, boot->kernel_size)) goto fail;
    if (!map_optional_range(map, boot->kernel_stack_base, boot->kernel_stack_size)) goto fail;
    if (!map_optional_range(map, boot->initrd_base, boot->initrd_size)) goto fail;

    /* GOP framebuffer. */
    if (!map_optional_range(map, boot->framebuffer_base, boot->framebuffer_size)) goto fail;

    /* ACPI RSDP itself. */
    if (acpi && acpi->rsdp_address) {
        if (!vmm_identity_map_range(map, acpi->rsdp_address, VM_PAGE_SIZE, VM_WRITE)) goto fail;
    }

    /* Local APIC MMIO. */
    if (acpi && acpi->lapic_address) {
        if (!vmm_identity_map_range(map, acpi->lapic_address, VM_PAGE_SIZE, VM_WRITE)) goto fail;
    }

    /* I/O APIC MMIO pages. */
    if (acpi) {
        for (u32 i = 0; i < acpi->io_apic_count; ++i) {

            u64 address = acpi->io_apics[i].address;

            if (!address) continue;
            if (!vmm_identity_map_range(map, address, VM_PAGE_SIZE, VM_WRITE)) goto fail;
        }
    }

    /*
    * AHCI HBA MMIO.
    * The ABAR itself remains identity-mapped for now.
    *
    * AHCI DMA RAM does NOT need identity mappings:
    *
    *   device -> physical addresses
    *   CPU    -> physical direct map
    */
    if (ahci && ahci->initialized && ahci->abar) {
        if (!vmm_identity_map_range(map, ahci->abar, VM_PAGE_SIZE, VM_WRITE)) goto fail;
    }

    /* Validate the absolutely critical mappings before touching CR3. */
    if (!identity_mapping_valid(map, boot->kernel_base)) goto fail;
    ArchDescriptorTablePointer gdtr;

    gdtr.limit = 0;
    gdtr.base = 0;

    arch_store_gdt(&gdtr);

    if (!gdtr.base || !identity_mapping_valid(map, gdtr.base)) goto fail;
    if (!identity_mapping_valid(map, boot->kernel_stack_base)) goto fail;
    if (!identity_mapping_valid(map, (u64)boot)) goto fail;
    if (!identity_mapping_valid(map, boot->memory_map)) goto fail;

    /* The PML4 itself came from PMM conventional memory, so it must also be accessible through the direct map. */
    u64 root_physical = frame_to_phys(map->root_frame);
    if (!root_physical) goto fail;

    if (vmm_query_page(map, root_physical, 0, 0)) {
        serial_write( "PAGING: PML4 unexpectedly identity mapped\n");
        goto fail;
    } 

    u64 root_direct = 0;
    if (!physmap_virtual_address(root_physical, &root_direct)) goto fail;

    frame_t direct_frame = FRAME_INVALID;
    if (!vmm_query_page(map, root_direct, &direct_frame, 0)) goto fail;
    if (direct_frame != map->root_frame) goto fail;
    if (boot->initrd_base && !identity_mapping_valid(map, boot->initrd_base)) goto fail;
    if (boot->framebuffer_base && !identity_mapping_valid(map, boot->framebuffer_base)) goto fail;
    if (ahci && ahci->initialized && ahci->abar && !identity_mapping_valid(map, ahci->abar)) goto fail;
    if (acpi && acpi->lapic_address && !identity_mapping_valid(map, acpi->lapic_address)) goto fail;

    /* PMM bitmap */
    PmmStats pmm = pmm_stats();
    if (!pmm.bitmap_physical) goto fail;

    /* Low alias must NOT exist */
    if (vmm_query_page(map, pmm.bitmap_physical, 0, 0)) {
        serial_write("PAGING: PMM bitmap unexpectedly identity mapped\n");
        goto fail;
    }

    /* Direct-map alias must exist */
    u64 bitmap_direct = 0;
    if (!physmap_virtual_address(pmm.bitmap_physical, &bitmap_direct)) goto fail;

    frame_t bitmap_frame = FRAME_INVALID;
    if (!vmm_query_page(map, bitmap_direct, &bitmap_frame, 0)) goto fail;
    if (bitmap_frame != phys_to_frame(pmm.bitmap_physical)) goto fail;
    /*
    * ACPI reset register, if lives in System
    * Memory rather than a I/O port.
    *
    * ACPI address-space ID 0 == SystemMemory.
    */
    if (acpi && acpi->reset_supported && acpi->reset_address_space == 0U && acpi->reset_address) {
        if (!vmm_identity_map_range(map, acpi->reset_address, VM_PAGE_SIZE, VM_WRITE)) goto fail;
    }

    return true;

fail:
    vmm_page_map_destroy(map);

    return false;
}

void kernel_main(BootInfo *boot) {
    arch_cli();
    (void)serial_init();

    if (!boot || boot->magic != BOOT_INFO_MAGIC || boot->version != BOOT_INFO_VERSION || boot->size < sizeof(BootInfo)) {
        serial_write("JA OS: invalid BootInfo.\n");
        cpu_halt_forever();
    }

    if (!framebuffer_init(boot) || !terminal_init()) { serial_write("JA OS: framebuffer initialization failed.\n"); cpu_halt_forever(); }

    splash_show(); splash_progress(5);
    /* Firmware GDT/TSS still active. Do NOT use the IST yet. */
    idt_init(false);
    splash_progress(10);

    bool gdt_ok = gdt_init(boot->kernel_stack_top);

    if (!gdt_ok) {
        serial_write("JA OS: GDT/TSS initialization failed.\n");

        terminal_set_color(terminal_error_color());
        terminal_writeln("GDT/TSS INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());

        cpu_halt_forever();
    }

    /*
    * CS = 0x08
    * TR = 0x28
    * TSS.IST1 initialized
    *
    * Rebuild the IDT using the JCOS CS selector
    * and enable IST1 for #DF.
    */
    idt_init(true);
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

    u64 old_cr3 = arch_read_cr3() & ~0xFFFULL;
    u64 new_cr3 = 0;

    bool paging_ok = false;

    /*
    * The current VMM is four-level x86-64 paging.
    *
    * Refuse to switch if firmware somehow entered
    * the kernel with CR4.LA57 enabled.
    */
    AddressSpace *kernel_space = 0;

    if (!(arch_read_cr4() & CR4_LA57)) {

        const AhciInfo *ahci = ahci_get();

        if (address_space_kernel_init()) {
            kernel_space = address_space_kernel();

            if (kernel_space && build_kernel_page_map(&kernel_space->page_map,boot, acpi, ahci)) {
                VmPageMap *kernel_map = &kernel_space->page_map;
                new_cr3 = address_space_cr3(kernel_space);

                if (new_cr3) {

                    /* Switch to the JCOS-owned page tables. */
                    arch_write_cr3(new_cr3);
                    u64 active_cr3 = arch_read_cr3() & ~0xFFFULL;
                    paging_ok = active_cr3 == new_cr3;

                    if (paging_ok) {
                        u64 root_physical = frame_to_phys(kernel_map->root_frame);
                        u64 root_direct = 0;
                        if (!physmap_virtual_address(root_physical, &root_direct)) {
                            paging_ok = false;
                        } 
                        else {
                            volatile const u64 *direct = (volatile const u64 *)(u64) root_direct;
                            volatile u64 probe = direct[0];
                            (void)probe;
                        }
                    }

                    if (paging_ok) {

                        vmm_enable_phys_map_access();

                        /* Move PMM metadata to the physmap and probe alloc/free. */
                        if (paging_ok) {

                            PmmStats before = pmm_stats();

                            u64 bitmap_physical = before.bitmap_physical;
                            u64 bitmap_direct = 0;

                            if (!bitmap_physical || !physmap_virtual_address(bitmap_physical, &bitmap_direct)) {
                                paging_ok = false;
                            } 
                            else {
                                frame_t mapped = FRAME_INVALID;

                                if (!vmm_query_page(kernel_map, bitmap_direct, &mapped, 0)) paging_ok = false;
                                else if (mapped != phys_to_frame(bitmap_physical)) paging_ok = false;
                            }

                            /* Only change g_bitmap after the mapping has been structurally verified. */
                            if (paging_ok && !pmm_enable_phys_map_access()) paging_ok = false;

                            if (paging_ok) {
                                PmmStats probe_before = pmm_stats();
                                frame_t probe = frame_alloc();

                                if (probe == FRAME_INVALID) {
                                    paging_ok = false;
                                } 
                                else {
                                    bool freed = frame_free(probe);
                                    PmmStats probe_after = pmm_stats();

                                    if (!freed || probe_before.free_pages != probe_after.free_pages) paging_ok = false;
                                }
                            }
                        }
                        u64 root_physical = frame_to_phys(kernel_map->root_frame);
                        u64 root_direct = 0;

                        if (!physmap_virtual_address(root_physical, &root_direct)) {
                            paging_ok = false;
                        } 
                        else {
                            frame_t mapped = FRAME_INVALID;

                            /* VMM now walks page tables through the physmap. */
                            if (!vmm_query_page(kernel_map, root_direct, &mapped, 0)) paging_ok = false;
                            else if (mapped != kernel_map->root_frame) paging_ok = false;
                        }
                    }
                    /* ------------------------------------------------ Move AHCI CPU-side DMA accesses onto the physical direct map. ------------------------------------------------ */
                    if (paging_ok && ahci_ok) {
                        if (!ahci_enable_phys_map_access()) {
                            paging_ok = false;
                        } 
                        else if (!boot_disk || boot_disk->block_size != 512U) {
                            /* AHCI claimed initialization succeeded, so the expected SATA block device should already exist. */
                            paging_ok = false;

                        } 
                        else {
                            /*
                            * Switch CPU-side AHCI DMA access to physmap,
                            * then perform a real DMA read as a runtime probe.
                            */
                            u8 sector_probe[512];
                            if (!block_read(boot_disk, 0, 1, sector_probe)) paging_ok = false;
                        }
                    }
                }
            }
        }
    }

    if (!paging_ok) {

        serial_write("JA OS: JCOS paging initialization failed.\n");

        terminal_set_color(terminal_error_color());
        terminal_writeln("JCOS PAGING INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());

        cpu_halt_forever();
    }

    /* If this serial message appears, we have successfully executed code after MOV CR3. */
    serial_write("JA OS: JCOS PAGE TABLES ACTIVE.\n");

    bool thread_ok = thread_system_init(
        kernel_space, 
        boot->kernel_stack_base, 
        boot->kernel_stack_size, 
        boot->kernel_stack_top
    );

    if (!thread_ok) {
        serial_write("JA OS: thread initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("THREAD INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    splash_progress(85);
    bool keyboard_ok = (!acpi->i8042_known || acpi->i8042_present) ? ps2_init() : false;
    splash_progress(95);
    interrupts_enable();
    splash_progress(100);

    boot_delay();

    ArchDescriptorTablePointer gdtr;

    gdtr.limit = 0;
    gdtr.base = 0;

    arch_store_gdt(&gdtr);

    terminal_clear(); terminal_set_color(terminal_accent_color());
    terminal_writeln("JA OS - INTERACTIVE X86_64 KERNEL");
    terminal_set_color(terminal_default_color());
    terminal_writeln("UEFI BOOT SERVICES EXITED. FRAMEBUFFER + SERIAL CONSOLES ONLINE.");
    terminal_write("IDT: READY  GDT/TSS: "); terminal_write(gdt_ok ? "READY" : "FAILED"); terminal_write("  PMM: ");
    terminal_writeln(pmm_ok ? "READY" : "FAILED");
    terminal_write("PAGING: ");
    terminal_writeln(paging_ok ? "JCOS CR3 ACTIVE" : "FAILED");
    terminal_write("  PHYS MAP: ");
    terminal_writeln(vmm_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");
    terminal_write("  PMM PHYS MAP: ");
    terminal_writeln(pmm_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");
    terminal_writeln("  LOW PMM IDENTITY: OFF");
    terminal_write("  PHYS MAP BASE: "); terminal_write_hex(PHYS_MAP_BASE); terminal_putchar('\n');
    terminal_write("  AHCI DMA PHYS MAP: ");
    terminal_writeln(ahci_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");

    u64 root_physical = frame_to_phys(kernel_space->page_map.root_frame);
    u64 root_direct = 0;

    if (physmap_virtual_address(root_physical, &root_direct)) { terminal_write("  PML4 DIRECT: "); terminal_write_hex(root_direct); terminal_putchar('\n'); }
    terminal_write("  CS: "); terminal_write_hex(arch_read_cs()); 
    terminal_write("  TR: "); terminal_write_hex(arch_read_tr()); terminal_putchar('\n'); 
    terminal_write("  RSP0: "); terminal_write_hex(gdt_rsp0()); terminal_putchar('\n'); 
    terminal_write("  IST1: "); terminal_write_hex(gdt_ist1()); terminal_putchar('\n'); 
    terminal_write("  OLD CR3: "); terminal_write_hex(old_cr3); terminal_putchar('\n'); 
    terminal_write("  NEW CR3: "); terminal_write_hex(new_cr3); terminal_putchar('\n'); 
    terminal_write("  PML4 FRAME: "); terminal_write_u64(kernel_space->page_map.root_frame);
    terminal_write("  GDTR BASE: "); terminal_write_hex(gdtr.base); 
    terminal_write("  LIMIT: "); terminal_write_u64(gdtr.limit); terminal_putchar('\n');
    terminal_write("  ACPI: "); terminal_write(acpi_ok ? "READY" : "FALLBACK");
    terminal_write("  IRQ: ");
    terminal_writeln(controller_ok ? interrupt_controller_name() : "FAILED");
    terminal_write("PCI: "); terminal_write_u64(pci_device_count());
    terminal_writeln(" DEVICE(S)");
    terminal_write("BLOCK DEVICES: "); terminal_write_u64(block_device_count()); terminal_putchar('\n');
    terminal_write("AHCI: ");
    terminal_writeln( ahci_ok ? "DETECTED" : "NOT DETECTED");
    terminal_write("THREADS: "); terminal_writeln( thread_ok ? "READY" : "FAILED");
    terminal_write("PS/2: "); terminal_write(keyboard_ok ? "DETECTED" : "NOT DETECTED");
    terminal_write("  COM1: ");
    terminal_writeln(serial_available() ? "READY" : "NOT DETECTED");
    terminal_write("ROOTFS: ");
    terminal_writeln(rootfs_ok ? "READY" : "FAILED");
    terminal_write("GPT: ");
    terminal_writeln(gpt_ok ? "READY" : "FAILED");
    terminal_write("  PARTITIONS: ");
    terminal_writeln(partitions_ok ? "READY" : "FAILED");
    terminal_write("FAT32: ");
    terminal_writeln( fat32_ok ? "READY" : "FAILED");
    terminal_writeln("TYPE help AND PRESS ENTER.");
    terminal_putchar('\n');

    shell_run(boot);
}