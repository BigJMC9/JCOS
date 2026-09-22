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
#include "process.h"
#include "endpoint.h"
#include "execution_profile.h"
#include "scheduler.h"
#include "supervisor.h"
#include "system_console.h"
#include "userspace_shell.h"
#include "timer.h"
#include "splash.h"
#include "audio.h"
#include "display.h"
#include "usb_xhci.h"

#define CR4_LA57 (1ULL << 12)
#define DIRECT_MAP_MIN_PHYSICAL 0x100000ULL
#define BOOT_STAGE_COUNT 24U

extern const u8 __kernel_text_start[] __attribute__((visibility("hidden")));
extern const u8 __kernel_text_end[] __attribute__((visibility("hidden")));
extern const u8 __kernel_data_start[] __attribute__((visibility("hidden")));
extern const u8 __kernel_data_end[] __attribute__((visibility("hidden")));

typedef struct {
    u64 text_base;
    u64 text_end;
    u64 data_base;
    u64 data_end;
} KernelImageLayout;

static u8 g_storage_probe[4096];

static bool map_physical_direct_map(VmPageMap *map, const BootInfo *boot) {
    if (!map ||
        !boot ||
        !boot->memory_map ||
        !boot->memory_map_size ||
        boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor)) {
        return false;
    }
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
        if (physical < DIRECT_MAP_MIN_PHYSICAL) {
            physical = DIRECT_MAP_MIN_PHYSICAL;
        }

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


static bool kernel_image_layout(const BootInfo *boot, KernelImageLayout *layout) {
    if (!boot || !layout || !boot->kernel_base || !boot->kernel_size) return false;
    if (boot->kernel_base > ~0ULL - boot->kernel_size) return false;

    layout->text_base = (u64)(const void *)__kernel_text_start;
    layout->text_end = (u64)(const void *)__kernel_text_end;
    layout->data_base = (u64)(const void *)__kernel_data_start;
    layout->data_end = (u64)(const void *)__kernel_data_end;

    u64 kernel_end = boot->kernel_base + boot->kernel_size;
    u64 page_mask = VM_PAGE_SIZE - 1ULL;
    if ((layout->text_base | layout->text_end | layout->data_base | layout->data_end) & page_mask) return false;
    if (layout->text_base != boot->kernel_base) return false;
    if (layout->text_base >= layout->text_end || layout->text_end != layout->data_base) return false;
    if (layout->data_base >= layout->data_end || layout->data_end != kernel_end) return false;
    if (boot->kernel_entry < layout->text_base || boot->kernel_entry >= layout->text_end) return false;
    return true;
}

static bool kernel_range_permissions_valid(const VmPageMap *map, u64 base, u64 end, vm_flags_t expected) {
    if (!map || !base || end <= base || (base & (VM_PAGE_SIZE - 1ULL)) || (end & (VM_PAGE_SIZE - 1ULL))) {
        return false;
    }
    for (u64 address = base; address < end; address += VM_PAGE_SIZE) {
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        if (!vmm_query_page(map, address, &frame, &flags) || frame != phys_to_frame(address)) return false;
        if ((flags & (VM_WRITE | VM_USER | VM_EXEC)) != expected) return false;

        u64 direct = 0;
        if (!physmap_virtual_address(address, &direct)) return false;
        if (vmm_query_page(map, direct, 0, 0)) return false;
    }
    return true;
}

static bool kernel_image_permissions_valid(const VmPageMap *map, const BootInfo *boot) {
    KernelImageLayout layout;
    if (!kernel_image_layout(boot, &layout)) return false;
    if (!kernel_range_permissions_valid(map, layout.text_base, layout.text_end, VM_EXEC)) return false;
    if (!kernel_range_permissions_valid(map, layout.data_base, layout.data_end, VM_WRITE)) return false;

    if (!boot->kernel_stack_base || !boot->kernel_stack_size ||
        (boot->kernel_stack_base & (VM_PAGE_SIZE - 1ULL)) ||
        (boot->kernel_stack_size & (VM_PAGE_SIZE - 1ULL)) ||
        boot->kernel_stack_base > ~0ULL - boot->kernel_stack_size) return false;
    u64 stack_end = boot->kernel_stack_base + boot->kernel_stack_size;
    for (u64 address = boot->kernel_stack_base; address < stack_end; address += VM_PAGE_SIZE) {
        frame_t frame = FRAME_INVALID;
        vm_flags_t flags = 0;
        if (!vmm_query_page(map, address, &frame, &flags) || frame != phys_to_frame(address)) return false;
        if ((flags & (VM_WRITE | VM_USER | VM_EXEC)) != VM_WRITE) return false;
    }
    return true;
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
    KernelImageLayout layout;
    if (!kernel_image_layout(boot, &layout)) {
        serial_write("PAGING: kernel image layout invalid\n");
        return false;
    }

    /* PMM bitmap must exist only through its physmap alias. */
    if (!map_physical_direct_map(map, boot)) {
        serial_write("PAGING: direct map build FAILED\n");
        goto fail;
    }

    serial_write("PAGING: direct map built\n");

    /* Explicit low identity mappings required by the currently running kernel follow. */
    if (!map_optional_range(map, (u64)boot, sizeof(BootInfo))) goto fail;
    if (!map_optional_range(map, boot->memory_map, boot->memory_map_size)) goto fail;
    if (!vmm_identity_map_range(map, layout.text_base, layout.text_end - layout.text_base, VM_EXEC)) goto fail;
    if (!vmm_identity_map_range(map, layout.data_base, layout.data_end - layout.data_base, VM_WRITE)) goto fail;
    if (!map_optional_range(map, boot->kernel_stack_base, boot->kernel_stack_size)) goto fail;
    if (!map_optional_range(map, boot->initrd_base, boot->initrd_size)) goto fail;

    /* GOP framebuffer. */
    if (!boot->framebuffer_base || !boot->framebuffer_size ||
        !vmm_identity_map_range(map, boot->framebuffer_base, boot->framebuffer_size,
            VM_WRITE | VM_UNCACHED)) goto fail;

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
    if (!identity_mapping_valid(map, boot->kernel_entry)) goto fail;
    if (!kernel_image_permissions_valid(map, boot)) goto fail;
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
        serial_write("PAGING: PML4 unexpectedly identity mapped\n");
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

    /* ACPI PM1 control blocks may also be SystemMemory GAS registers. */
    if (acpi && acpi->poweroff_supported) {
        if (acpi->pm1a_control.address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY &&
            acpi->pm1a_control.address &&
            !vmm_identity_map_range(map, acpi->pm1a_control.address, sizeof(u16), VM_WRITE)) goto fail;

        if (acpi->pm1b_control.address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY &&
            acpi->pm1b_control.address &&
            !vmm_identity_map_range(map, acpi->pm1b_control.address, sizeof(u16), VM_WRITE)) goto fail;
    }

    return true;

fail:
    vmm_page_map_destroy(map);

    return false;
}

void kernel_main(BootInfo *boot) {
    arch_cli();
    (void)serial_init();

    if (!boot ||
        boot->magic != BOOT_INFO_MAGIC ||
        boot->version != BOOT_INFO_VERSION ||
        boot->size < sizeof(BootInfo)) {
        serial_write("JA OS: invalid BootInfo.\n");
        cpu_halt_forever();
    }

    if (!framebuffer_init(boot) || !terminal_init()) {
        serial_write("JA OS: framebuffer initialization failed.\n");
        cpu_halt_forever();
    }

    splash_show();
    u32 boot_stage = 0U;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Preparing interrupt descriptor table");
    /* Firmware GDT/TSS still active. Do NOT use the IST yet. */
    idt_init(false);
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing GDT and TSS");
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
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing physical memory manager");
    bool pmm_ok = pmm_init(boot);
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing virtual filesystem");
    vfs_init();
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Mounting root filesystem");
    bool rootfs_ok = false;

    if (boot->initrd_base && boot->initrd_size) {
        rootfs_ok = tar_mount((const void *)(u64)boot->initrd_base, boot->initrd_size);
    }
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Discovering ACPI platform tables");
    bool acpi_ok = acpi_init(boot->acpi_rsdp);
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Enumerating PCI devices");
    pci_init();
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing block device registry");
    block_init();
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing partition subsystem");
    partition_init();
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing AHCI storage");
    bool ahci_ok = ahci_init();
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing interrupt controller");
    const AcpiInfo *acpi = acpi_get();
    bool controller_ok = interrupt_controller_init(acpi);
    ++boot_stage;

    bool gpt_ok = false;
    bool partitions_ok = false;
    bool fat32_ok = false;
    BlockDevice *gpt_disk = 0;
    BlockDevice *ahci_probe_disk = 0;

    /*
     * Discover storage by capability/type rather than assuming that the first
     * AHCI disk is both "sda" and the disk whose GPT we want. Keep the raw
     * device count because GPT registration adds partition BlockDevices.
     */
    u32 raw_block_devices = block_device_count();
    for (u32 i = 0; i < raw_block_devices; ++i) {
        BlockDevice *candidate = block_device(i);
        if (!candidate || candidate->type == BLOCK_DEVICE_PARTITION) continue;

        if (!ahci_probe_disk && candidate->type == BLOCK_DEVICE_AHCI) {
            ahci_probe_disk = candidate;
        }

        if (!gpt_ok && gpt_probe(candidate)) {
            gpt_ok = true;
            gpt_disk = candidate;
        }
    }

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Discovering GPT partitions");
    if (gpt_disk) {
        partitions_ok = gpt_register_partitions();
    }
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Probing FAT32 EFI system partition");
    if (partitions_ok) {
        BlockDevice *esp = gpt_efi_system_partition();
        if (esp) {
            fat32_ok = fat32_probe(esp);
        }
    }
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Building kernel page tables and physical map");
    u64 old_cr3 = arch_read_cr3() & ~0xFFFULL;
    u64 new_cr3 = 0;
    bool paging_ok = false;
    bool kernel_wx_ok = false;

    /*
    * The current VMM is four-level x86-64 paging.
    *
    * Refuse to switch if firmware somehow entered
    * the kernel with CR4.LA57 enabled.
    */
    AddressSpace *kernel_space = 0;
    bool nx_ok = vmm_enable_nx();

    if (nx_ok && !(arch_read_cr4() & CR4_LA57)) {

        const AhciInfo *ahci = ahci_get();

        if (address_space_kernel_init()) {
            kernel_space = address_space_kernel();

            if (kernel_space && build_kernel_page_map(&kernel_space->page_map, boot, acpi, ahci)) {
                VmPageMap *kernel_map = &kernel_space->page_map;
                new_cr3 = address_space_cr3(kernel_space);

                if (new_cr3) {

                    /* Switch to the JCOS-owned page tables. */
                    arch_write_cr3(new_cr3);
                    u64 active_cr3 = arch_read_cr3() & ~0xFFFULL;
                    paging_ok = active_cr3 == new_cr3;
                    if (paging_ok && !vmm_enable_write_protect()) paging_ok = false;

                    if (paging_ok) {
                        u64 root_physical = frame_to_phys(kernel_map->root_frame);
                        u64 root_direct = 0;
                        if (!physmap_virtual_address(root_physical, &root_direct)) {
                            paging_ok = false;
                        } else {
                            volatile const u64 *direct = (volatile const u64 *)(u64) root_direct;
                            volatile u64 probe = direct[0];
                            (void)probe;
                        }
                    }

                    if (paging_ok) {

                        vmm_enable_phys_map_access();
                        kernel_wx_ok = vmm_write_protect_enabled() && kernel_image_permissions_valid(kernel_map, boot);
                        if (!kernel_wx_ok) paging_ok = false;

                        /* Move PMM metadata to the physmap and probe alloc/free. */
                        if (paging_ok) {

                            PmmStats before = pmm_stats();
                            u64 bitmap_physical = before.bitmap_physical;
                            u64 bitmap_direct = 0;

                            if (!bitmap_physical || !physmap_virtual_address(bitmap_physical, &bitmap_direct)) {
                                paging_ok = false;
                            } else {
                                frame_t mapped = FRAME_INVALID;

                                if (!vmm_query_page(kernel_map, bitmap_direct, &mapped, 0)) {
                                    paging_ok = false;
                                } else if (mapped != phys_to_frame(bitmap_physical)) {
                                    paging_ok = false;
                                }
                            }

                            /* Only change g_bitmap after the mapping has been structurally verified. */
                            if (paging_ok && !pmm_enable_phys_map_access()) {
                                paging_ok = false;
                            }

                            if (paging_ok) {
                                PmmStats probe_before = pmm_stats();
                                frame_t probe = frame_alloc();

                                if (probe == FRAME_INVALID) {
                                    paging_ok = false;
                                } else {
                                    bool freed = frame_free(probe);
                                    PmmStats probe_after = pmm_stats();

                                    if (!freed || probe_before.free_pages != probe_after.free_pages) {
                                        paging_ok = false;
                                    }
                                }
                            }
                        }
                        u64 root_physical = frame_to_phys(kernel_map->root_frame);
                        u64 root_direct = 0;

                        if (!physmap_virtual_address(root_physical, &root_direct)) {
                            paging_ok = false;
                        } else {
                            frame_t mapped = FRAME_INVALID;

                            /* VMM now walks page tables through the physmap. */
                            if (!vmm_query_page(kernel_map, root_direct, &mapped, 0)) {
                                paging_ok = false;
                            } else if (mapped != kernel_map->root_frame) {
                                paging_ok = false;
                            }
                        }
                    }
                    /* ------------------------------------------------ Move AHCI CPU-side DMA accesses onto the physical direct map. ------------------------------------------------ */
                    if (paging_ok && ahci_ok) {
                        if (!ahci_enable_phys_map_access()) {
                            paging_ok = false;
                        } else if (!ahci_probe_disk ||
                            ahci_probe_disk->block_size > sizeof(g_storage_probe)) {
                            /*
                             * AHCI claimed initialization succeeded, so at least
                             * one registered SATA block device should be usable.
                             */
                            paging_ok = false;
                        } else {
                            /*
                             * Switch CPU-side AHCI DMA access to physmap, then
                             * perform a real DMA read using the disk's reported
                             * logical sector size (512 through 4096 supported).
                             */
                            if (!block_read(ahci_probe_disk, 0, 1,
                                    g_storage_probe)) {
                                paging_ok = false;
                            }
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
    ++boot_stage;

    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing process system");
    bool process_ok = process_system_init(kernel_space);
    if (!process_ok) {
        serial_write("JA OS: process initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("PROCESS INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing endpoint system");
    bool endpoint_ok = endpoint_system_init();
    if (!endpoint_ok) {
        serial_write("JA OS: endpoint initialization failed.\n");

        terminal_set_color(terminal_error_color());
        terminal_writeln("ENDPOINT INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());

        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing thread system");
    Process *kernel_process = process_kernel();

    bool thread_ok =
        kernel_process &&
        thread_system_init(kernel_process, boot->kernel_stack_base, boot->kernel_stack_size, boot->kernel_stack_top);

    if (!thread_ok) {
        serial_write("JA OS: thread initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("THREAD INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing scheduler");
    bool scheduler_ok = scheduler_init();
    if (!scheduler_ok) {
        serial_write("JA OS: scheduler initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("SCHEDULER INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    bool execution_profile_ok = execution_profile_init();
    if (!execution_profile_ok) {
        serial_write("JA OS: restricted execution profile initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("EXECUTION PROFILE INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing display and audio resources");
    bool display_ok = display_init();
    bool audio_ok = audio_init();
    if (!display_ok) {
        serial_write("JA OS: display resource initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("DISPLAY RESOURCE INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Starting userspace supervisor");
    bool supervisor_ok = supervisor_start();

    if (!supervisor_ok) {
        serial_write("JA OS: supervisor initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("SUPERVISOR INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());

        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing PIT timer");
    bool timer_ok = timer_init(100U);

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Initializing keyboard input");
    bool keyboard_ok = (!acpi->i8042_known || acpi->i8042_present) ? ps2_init() : false;
    bool usb_keyboard_ok = xhci_init(&kernel_space->page_map);
    (void)keyboard_ok;
    (void)usb_keyboard_ok;

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Enabling hardware interrupts");
    bool preemption_ok = timer_ok && scheduler_preemption_enable();
    if (!preemption_ok) {
        serial_write("JA OS: runtime timer preemption policy failed to start.\n");
        terminal_set_color(terminal_error_color());
        if (!timer_ok) terminal_writeln("PIT TIMER INITIALIZATION FAILED; RUNTIME PREEMPTION UNAVAILABLE.");
        else terminal_writeln("SCHEDULER PREEMPTION POLICY INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }
    interrupts_enable();

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Starting userspace console service");
    bool archive_portal_ok = boot->initrd_base && boot->initrd_size &&
        system_console_configure_boot_archive((const void *)(u64)boot->initrd_base, boot->initrd_size);
    bool console_ok = archive_portal_ok && system_console_start();
    if (!console_ok) {
        serial_write("JA OS: userspace console service initialization failed.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("USERSPACE CONSOLE SERVICE INITIALIZATION FAILED.");
        terminal_set_color(terminal_default_color());
        cpu_halt_forever();
    }

    ++boot_stage;
    splash_update(boot_stage, BOOT_STAGE_COUNT, "Boot complete");

    framebuffer_fill_rect(0U, 0U, 160U, 24U, framebuffer_rgb(220U, 40U, 40U));
    serial_write("POST-SPLASH: before terminal clear\n");
    terminal_clear();
    terminal_writeln("POST-SPLASH: framebuffer console active");
    if (xhci_failure_count()) {
        terminal_writeln("XHCI DIAGNOSTICS:");
        for (u32 i = 0; i < xhci_failure_count(); ++i) {
            const XhciFailureRecord *failure = xhci_failure(i);
            if (!failure) continue;
            terminal_write("  FAIL ");
            terminal_write(failure->stage);
            terminal_write(" USBCMD=");
            terminal_write_hex(failure->usbcmd);
            terminal_write(" USBSTS=");
            terminal_write_hex(failure->usbsts);
            terminal_write(" DETAIL=");
            terminal_write_hex(failure->detail);
            if (failure->port != ~0U) {
                terminal_write(" PORT=");
                terminal_write_u64((u64)failure->port + 1ULL);
                if (failure->protocol_major) {
                    terminal_write(" USB");
                    terminal_write_u64(failure->protocol_major);
                }
            }
            terminal_putchar('\n');
        }
    }

    serial_write("BOOT COMPLETE: framebuffer base=");
    serial_write_hex(boot->framebuffer_base);
    serial_write(" size=");
    serial_write_u64(boot->framebuffer_size);
    serial_write(" geometry=");
    serial_write_u64(boot->framebuffer_width);
    serial_write("x");
    serial_write_u64(boot->framebuffer_height);
    serial_write(" pitch=");
    serial_write_u64(boot->framebuffer_pixels_per_scanline);
    serial_write("\nBOOT COMPLETE: clearing terminal\n");

    ArchDescriptorTablePointer gdtr;

    gdtr.limit = 0;
    gdtr.base = 0;

    arch_store_gdt(&gdtr);

    terminal_set_color(terminal_accent_color());
    terminal_writeln("POST-SPLASH: entering console service");
    system_console_writeln("JA OS - INTERACTIVE X86_64 KERNEL");
    terminal_writeln("POST-SPLASH: console service returned");
    terminal_set_color(terminal_default_color());
    system_console_writeln("UEFI BOOT SERVICES EXITED. FRAMEBUFFER + SERIAL CONSOLES ONLINE.");
    system_console_write("IDT: READY  GDT/TSS: ");
    system_console_write(gdt_ok ? "READY" : "FAILED");
    system_console_write("  PMM: ");
    system_console_writeln(pmm_ok ? "READY" : "FAILED");
    system_console_write("PAGING: ");
    system_console_writeln(paging_ok ? "JCOS CR3 ACTIVE" : "FAILED");
    system_console_write("  NX: ");
    system_console_writeln(vmm_nx_enabled() ? "ACTIVE" : "INACTIVE");
    system_console_write("  CR0.WP: ");
    system_console_writeln(vmm_write_protect_enabled() ? "ACTIVE" : "INACTIVE");
    system_console_write("  KERNEL W^X: ");
    system_console_writeln(kernel_wx_ok ? "ACTIVE" : "FAILED");
    system_console_write("  PHYS MAP: ");
    system_console_writeln(vmm_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");
    system_console_write("  PMM PHYS MAP: ");
    system_console_writeln(pmm_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");
    system_console_writeln("  LOW PMM IDENTITY: OFF");
    system_console_write("  PHYS MAP BASE: ");
    system_console_write_hex(PHYS_MAP_BASE);
    system_console_putchar('\n');
    system_console_write("  AHCI DMA PHYS MAP: ");
    system_console_writeln(ahci_phys_map_access_enabled() ? "ACTIVE" : "INACTIVE");

    u64 root_physical = frame_to_phys(kernel_space->page_map.root_frame);
    u64 root_direct = 0;

    if (physmap_virtual_address(root_physical, &root_direct)) {
        system_console_write("  PML4 DIRECT: ");
        system_console_write_hex(root_direct);
        system_console_putchar('\n');
    }
    system_console_write("  CS: ");
    system_console_write_hex(arch_read_cs());
    system_console_write("  TR: ");
    system_console_write_hex(arch_read_tr());
    system_console_putchar('\n');
    system_console_write("  RSP0: ");
    system_console_write_hex(gdt_rsp0());
    system_console_putchar('\n');
    system_console_write("  IST1: ");
    system_console_write_hex(gdt_ist1());
    system_console_putchar('\n');
    system_console_write("  OLD CR3: ");
    system_console_write_hex(old_cr3);
    system_console_putchar('\n');
    system_console_write("  NEW CR3: ");
    system_console_write_hex(new_cr3);
    system_console_putchar('\n');
    system_console_write("  PML4 FRAME: ");
    system_console_write_u64(kernel_space->page_map.root_frame);
    system_console_write("  GDTR BASE: ");
    system_console_write_hex(gdtr.base);
    system_console_write("  LIMIT: ");
    system_console_write_u64(gdtr.limit);
    system_console_putchar('\n');
    system_console_write("  ACPI: ");
    system_console_write(acpi_ok ? "READY" : "FALLBACK");
    system_console_write("  IRQ: ");
    system_console_writeln(controller_ok ? interrupt_controller_name() : "FAILED");
    system_console_write("PCI: ");
    system_console_write_u64(pci_device_count());
    system_console_writeln(" DEVICE(S)");
    system_console_write("BLOCK DEVICES: ");
    system_console_write_u64(block_device_count());
    system_console_putchar('\n');
    system_console_write("AHCI: ");
    system_console_writeln(ahci_ok ? "DETECTED" : "NOT DETECTED");
    system_console_write("PROCESSES: ");
    system_console_writeln(process_ok ? "READY" : "FAILED");
    system_console_write("ENDPOINTS: ");
    system_console_writeln(endpoint_ok ? "READY" : "FAILED");
    system_console_write("THREADS: ");
    system_console_writeln(thread_ok ? "READY" : "FAILED");
    system_console_write("SCHEDULER: ");
    system_console_writeln(scheduler_ok ? "READY" : "FAILED");
    system_console_write("EXECUTION PROFILE: ");
    system_console_writeln(execution_profile_ok && execution_profile_fp_simd_restricted() ? "UP / FP-SIMD RESTRICTED" : "FAILED");
    system_console_write("SUPERVISOR: ");
    system_console_writeln(supervisor_ok ? "READY" : "FAILED");
    system_console_write("CONSOLE SERVICE: ");
    system_console_writeln(console_ok && system_console_running() ? "READY" : "FAILED");
    system_console_write("BOOT ARCHIVE: ");
    system_console_writeln(archive_portal_ok && system_console_archive_size() == boot->initrd_size
        ? "RAW PORTAL / RING3 NAMESPACE" : "FAILED");
    system_console_write("DISPLAY: ");
    system_console_writeln(display_ok ? "CAPABILITY RESOURCE READY" : "FAILED");
    system_console_write("AUDIO: ");
    system_console_writeln(audio_ok ? "AC97 48 KHZ READY" : "NOT DETECTED");
    system_console_write("TIMER: ");
    if (timer_ok) {
        system_console_write("PIT ");
        system_console_write_u64(timer_frequency());
        system_console_writeln(" HZ");
    } 
    else {
        system_console_writeln("FAILED");
    }
    system_console_write("PREEMPTION: ");
    system_console_writeln(preemption_ok && scheduler_preemption_enabled() ? "ACTIVE" : "FAILED");
    system_console_write("PS/2: ");
    system_console_write(keyboard_ok ? "DETECTED" : "NOT DETECTED");
    system_console_write("  USB HID: ");
    system_console_write(xhci_present() ? "DETECTED" : "NOT DETECTED");
    system_console_write("  COM1: ");
    system_console_writeln(serial_available() ? "READY" : "NOT DETECTED");

    system_console_write("XHCI: ");
    system_console_write(xhci_controller_ready() ? "READY" : "FAILED");
    system_console_write("  ROOT PORTS: ");
    system_console_write_u64(xhci_root_port_count());
    system_console_write("  SCRATCHPADS: ");
    system_console_write_u64(xhci_scratchpad_count());
    system_console_putchar('\n');
    system_console_write("ROOTFS: ");
    system_console_writeln(rootfs_ok ? "READY" : "FAILED");
    system_console_write("GPT: ");
    if (gpt_ok && gpt_disk) {
        system_console_write("READY ON ");
        system_console_writeln(gpt_disk->name);
    } else {
        system_console_writeln("NOT FOUND ON REGISTERED BLOCK DEVICES");
    }
    system_console_write("  PARTITIONS: ");
    system_console_writeln(partitions_ok ? "READY" : "NOT AVAILABLE");
    system_console_write("FAT32 ESP: ");
    system_console_writeln(fat32_ok ? "READY" : "NOT FOUND");
    system_console_writeln("DEFAULT SHELL: RING3 USERSPACE.");
    system_console_writeln("TYPE help AT THE USER SHELL PROMPT; TYPE monitor OR PRESS ESC FOR THE KERNEL MONITOR.");
    system_console_putchar('\n');

    if (!userspace_shell_run_boot_default()) {
        serial_write("R7 SHELL: default userspace shell unavailable; entering kernel emergency monitor.\n");
        terminal_set_color(terminal_error_color());
        terminal_writeln("USERSPACE SHELL UNAVAILABLE. ENTERING KERNEL EMERGENCY MONITOR.");
        terminal_set_color(terminal_default_color());
    } else {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("KERNEL EMERGENCY / DEVELOPMENT MONITOR ACTIVE.");
        terminal_set_color(terminal_default_color());
        terminal_writeln("TYPE usershell TO RETURN TO THE NORMAL RING3 SHELL.");
    }
    terminal_putchar('\n');

    shell_run(boot);
}
