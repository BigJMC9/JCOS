#include "shell.h"
#include "acpi.h"
#include "arch.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "lib.h"
#include "pci.h"
#include "pmm.h"
#include "ps2.h"
#include "serial.h"
#include "terminal.h"
#include "vfs.h"
#include "ahci.h"
#include "block.h"
#include "gpt.h"

#define INPUT_CAPACITY 128U

static VfsNode *g_cwd;

static const BootInfo *g_boot;
static char g_input[INPUT_CAPACITY];
static u32 g_length;

static char *trim(char *s) {
    while (*s && k_ascii_space(*s)) ++s;
    char *end = s + k_strlen(s);
    while (end > s && k_ascii_space(end[-1])) --end;
    *end = 0;
    return s;
}

static const char *block_type_name(BlockDeviceType type) {
    switch (type) {
        case BLOCK_DEVICE_AHCI: return "AHCI";
        case BLOCK_DEVICE_NVME: return "NVME";
        case BLOCK_DEVICE_VIRTIO: return "VIRTIO";
        case BLOCK_DEVICE_PARTITION: return "PARTITION";
        case BLOCK_DEVICE_UNKNOWN:
        default: return "UNKNOWN";
    }
}

static bool parse_u64(const char *text, u64 *value) {
    if (!text || !*text || !value) return false;
    u64 result = 0;
    while (*text) {
        if (*text < '0' || *text > '9') return false;
        u64 digit = (u64)(*text - '0');
        if (result > (~0ULL - digit) / 10ULL) return false;
        result = result * 10ULL + digit;
        ++text;
    }
    *value = result;
    return true;
}

static void terminal_hex_byte(u8 value) {
    static const char digits[] = "0123456789ABCDEF";
    terminal_putchar(digits[(value >> 4) & 0x0F]); terminal_putchar(digits[value & 0x0F]);
}

static void prompt(void) {
    char path[VFS_PATH_MAX];
    terminal_set_color(terminal_accent_color()); terminal_write("JA:");
    if (vfs_get_path(g_cwd, path, sizeof(path))) terminal_write(path);
    else terminal_write("?");
    terminal_write("> "); terminal_set_color(terminal_default_color());
}

static void print_mib(u64 pages) {
    terminal_write_u64((pages * 4096ULL) / (1024ULL * 1024ULL)); terminal_write(" MiB");
}

static void command_help(void) {
    terminal_writeln("COMMANDS:"); 
    terminal_writeln("  help        show this list"); 
    terminal_writeln("  about       describe this kernel"); 
    terminal_writeln("  clear       clear framebuffer and serial terminal"); 
    terminal_writeln("  ls          list files in current directory");
    terminal_writeln("  cd PATH     change current directory"); 
    terminal_writeln("  pwd         print current directory"); 
    terminal_writeln("  cat FILE    print a file"); 
    terminal_writeln("  memory      show UEFI memory-map and allocator state");
    terminal_writeln("  alloc       allocate one physical 4 KiB page"); 
    terminal_writeln("  cpu         show CPUID information"); 
    terminal_writeln("  interrupts  show APIC/PIC and keyboard counters"); 
    terminal_writeln("  acpi        show ACPI discovery results");
    terminal_writeln("  pci         list discovered PCI devices"); 
    terminal_writeln("  ahci        show AHCI controller and SATA ports"); 
    terminal_writeln("  sector LBA  dump a raw disk sector");
    terminal_writeln("  partitions  list GPT partitions");
    terminal_writeln("  fault       deliberately execute UD2 to test the IDT");
    terminal_writeln("  reboot      reset via ACPI, keyboard controller, or triple fault"); 
    terminal_writeln("  disks       list block devices");
}

static void command_about(void) {
    terminal_writeln("JA OS V5 IS A FREESTANDING X86_64 UEFI KERNEL CREATED BY JACOB CROSBIE."); 
    terminal_writeln("THE EFI LOADER LOADS KERNEL.ELF, CAPTURES GOP + ACPI + MEMORY MAP,"); 
    terminal_writeln("CALLS EXITBOOTSERVICES(), THEN AN ASSEMBLY SHIM ENTERS THIS C KERNEL.");
    terminal_writeln("INPUT: PS/2 SET-1 KEYBOARD WITH IRQ + POLLING, OR COM1 SERIAL.");
}

static void command_memory(void) {
    PmmStats stats = pmm_stats();
    terminal_write("UEFI DESCRIPTORS: "); terminal_write_u64(g_boot->memory_map_descriptor_size ? g_boot->memory_map_size / g_boot->memory_map_descriptor_size : 0);
    terminal_putchar('\n');
    terminal_write("ALLOCATOR RANGES: "); terminal_write_u64(stats.range_count); terminal_putchar('\n');
    terminal_write("CONVENTIONAL MEMORY: "); print_mib(stats.total_pages); terminal_putchar('\n');
    terminal_write("FREE BUMP-ALLOCATOR MEMORY: "); print_mib(stats.free_pages); terminal_putchar('\n');
    terminal_write("FREE PAGES: "); terminal_write_u64(stats.free_pages); terminal_putchar('\n');
    terminal_write("KERNEL BASE: "); terminal_write_hex(g_boot->kernel_base); terminal_putchar('\n');
    terminal_write("KERNEL SIZE: "); terminal_write_u64(g_boot->kernel_size);
    terminal_writeln(" bytes");
    if (stats.discarded_ranges) { terminal_write("DISCARDED EXTRA RANGES: "); terminal_write_u64(stats.discarded_ranges); terminal_putchar('\n'); }
}

static void command_alloc(void) {
    u64 page = pmm_alloc_page();
    if (!page) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("OUT OF PHYSICAL PAGES.");
        terminal_set_color(terminal_default_color());
        return;
    }
    terminal_write("ALLOCATED PHYSICAL PAGE: "); terminal_write_hex(page); terminal_putchar('\n');
    terminal_writeln("THIS V5 ALLOCATOR IS MONOTONIC; FREE() AND OWN PAGE TABLES COME NEXT.");
}

static void command_cpu(void) {
    u32 a, b, c, d;
    char vendor[13];
    arch_cpuid(0, 0, &a, &b, &c, &d);
    k_memcpy(vendor + 0, &b, 4); k_memcpy(vendor + 4, &d, 4); k_memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    u32 max_basic = a;

    char brand[49];
    k_memset(brand, 0, sizeof(brand));
    arch_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    u32 max_extended = a;
    if (max_extended >= 0x80000004U) {
        u32 *words = (u32 *)(void *)brand;
        for (u32 leaf = 0; leaf < 3; ++leaf) arch_cpuid(0x80000002U + leaf, 0, &words[leaf * 4], &words[leaf * 4 + 1], &words[leaf * 4 + 2], &words[leaf * 4 + 3]);
        brand[48] = 0;
    }

    arch_cpuid(1, 0, &a, &b, &c, &d);
    u32 family = (a >> 8) & 0xF;
    u32 model = (a >> 4) & 0xF;
    u32 stepping = a & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF) model += ((a >> 16) & 0xF) << 4;

    terminal_write("VENDOR: ");
    terminal_writeln(vendor);
    if (brand[0]) { terminal_write("BRAND: "); terminal_writeln(brand); }
    terminal_write("FAMILY: "); terminal_write_u64(family);
    terminal_write(" MODEL: "); terminal_write_u64(model);
    terminal_write(" STEPPING: "); terminal_write_u64(stepping); terminal_putchar('\n');
    terminal_write("APIC: "); terminal_write((d & (1U << 9)) ? "YES" : "NO");
    terminal_write("  X2APIC: "); terminal_write((c & (1U << 21)) ? "YES" : "NO");
    terminal_write("  SSE2: ");
    terminal_writeln((d & (1U << 26)) ? "YES" : "NO");
    bool long_mode = false;
    if (max_extended >= 0x80000001U) {
        arch_cpuid(0x80000001U, 0, &a, &b, &c, &d);
        long_mode = (d & (1U << 29)) != 0;
    }
    terminal_write("LONG MODE: ");
    terminal_writeln(long_mode ? "YES" : "NO");
    terminal_write("MAX BASIC CPUID LEAF: "); terminal_write_hex(max_basic); terminal_putchar('\n');
}

static void command_interrupts(void) {
    InterruptControllerInfo info = interrupt_controller_info();
    terminal_write("CONTROLLER: ");
    terminal_writeln(interrupt_controller_name());
    terminal_write("KEYBOARD GSI: "); terminal_write_u64(info.keyboard_gsi); terminal_putchar('\n');
    if (info.mode == INTERRUPT_CONTROLLER_APIC) { terminal_write("LOCAL APIC ID: "); terminal_write_u64(info.local_apic_id); terminal_putchar('\n'); }
    terminal_write("VECTOR 0x21 COUNT: "); terminal_write_u64(interrupt_count(0x21)); terminal_putchar('\n');
    terminal_write("PS/2 IRQ HANDLER COUNT: "); terminal_write_u64(ps2_irq_count()); terminal_putchar('\n');
    terminal_write("PS/2 SCANCODE BYTES: "); terminal_write_u64(ps2_scancode_count()); terminal_putchar('\n');
    terminal_write("DROPPED KEY EVENTS: "); terminal_write_u64(ps2_dropped_count()); terminal_putchar('\n');
    terminal_write("LOCAL APIC SPURIOUS: "); terminal_write_u64(interrupt_spurious_count()); terminal_putchar('\n');
}

static void command_acpi(void) {
    const AcpiInfo *info = acpi_get();
    terminal_write("RSDP: "); terminal_write_hex(info->rsdp_address); terminal_putchar('\n');
    terminal_write("ACPI VALID: ");
    terminal_writeln(info->valid ? "YES" : "NO");
    terminal_write("MADT VALID: ");
    terminal_writeln(info->madt_valid ? "YES" : "NO");
    terminal_write("LOCAL APIC ADDRESS: "); terminal_write_hex(info->lapic_address); terminal_putchar('\n');
    terminal_write("IO APIC COUNT: "); terminal_write_u64(info->io_apic_count); terminal_putchar('\n');
    terminal_write("KEYBOARD GSI: "); terminal_write_u64(info->keyboard_gsi); terminal_putchar('\n');
    terminal_write("8042 CONTROLLER: ");
    terminal_writeln(!info->i8042_known ? "UNKNOWN" : (info->i8042_present ? "PRESENT" : "ABSENT"));
    terminal_write("ACPI RESET REGISTER: ");
    terminal_writeln(info->reset_supported ? "YES" : "NO");
}

static const char *pci_device_kind(const PciDevice *device) {
    if (!device) return "UNKNOWN";
    if (device->class_code == PCI_CLASS_MASS_STORAGE) {
        if (device->subclass == PCI_SUBCLASS_NVM && device->prog_if == PCI_PROGIF_NVME) return "NVME";
        if (device->subclass == PCI_SUBCLASS_SATA && device->prog_if == PCI_PROGIF_AHCI) return "AHCI";

        return "MASS STORAGE";
    }

    if (device->class_code == 0x03U) return "DISPLAY";
    if (device->class_code == 0x02U) return "NETWORK";
    if (device->class_code == 0x06U) return "BRIDGE";
    if (device->class_code == 0x0CU) return "SERIAL BUS";

    return "OTHER";
}

static void command_pci(void) {
    u32 count = pci_device_count();
    terminal_write("PCI DEVICES: "); terminal_write_u64(count); terminal_putchar('\n');

    if (count == 0) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("NO PCI DEVICES DISCOVERED.");
        terminal_set_color(terminal_default_color());
        return;
    }

    for (u32 i = 0; i < count; ++i) {
        const PciDevice *device = pci_device(i);

        if (!device) continue;
        /* Bus:Device.Function */
        terminal_write_u64(device->address.bus); terminal_putchar(':'); terminal_write_u64(device->address.device); terminal_putchar('.'); terminal_write_u64(device->address.function);
        terminal_write("  VEN="); terminal_write_hex(device->vendor_id);
        terminal_write(" DEV="); terminal_write_hex(device->device_id);
        terminal_write("  CLASS="); terminal_write_hex(device->class_code);
        terminal_write(" SUB="); terminal_write_hex(device->subclass);
        terminal_write(" IF="); terminal_write_hex(device->prog_if);
        terminal_write("  ");
        terminal_writeln(pci_device_kind(device));

        if (device->class_code == PCI_CLASS_MASS_STORAGE) {
            /* AHCI uses BAR5 as the ABAR. */
            if (device->subclass == PCI_SUBCLASS_SATA && device->prog_if == PCI_PROGIF_AHCI) {
                PciBar bar;
                if (pci_read_bar(device, 5, &bar) && bar.type != PCI_BAR_NONE) {
                    terminal_write("    ABAR/BAR5: ");
                    if (bar.type == PCI_BAR_MMIO32) terminal_write("MMIO32 ");
                    else if (bar.type == PCI_BAR_MMIO64) terminal_write( "MMIO64 ");
                    else terminal_write("INVALID ");
                    terminal_write_hex(bar.base); terminal_putchar('\n');
                }
            }
        }
    }
}

static void command_ahci(void) {
    const AhciInfo *info = ahci_get();

    if (!info || !info->initialized) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("AHCI CONTROLLER NOT INITIALIZED.");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_writeln("AHCI CONTROLLER:");
    terminal_write("  PCI: "); terminal_write_u64(info->pci_address.bus); terminal_putchar(':'); terminal_write_u64(info->pci_address.device);
    terminal_putchar('.'); terminal_write_u64(info->pci_address.function); terminal_putchar('\n');
    terminal_write("  ABAR: "); terminal_write_hex(info->abar); terminal_putchar('\n');
    terminal_write("  CAP: "); terminal_write_hex(info->capabilities); terminal_putchar('\n');
    terminal_write("  VERSION: "); terminal_write_hex(info->version); terminal_putchar('\n');
    terminal_write("  PORTS IMPLEMENTED: "); terminal_write_hex(info->ports_implemented); terminal_putchar('\n');
    terminal_write("  HARDWARE PORT COUNT: "); terminal_write_u64(info->hardware_port_count); terminal_putchar('\n');
    terminal_write("  ACTIVE DEVICES: "); terminal_write_u64(info->active_port_count); terminal_putchar('\n');
    terminal_writeln("PORTS:");

    bool any = false;

    for (u32 i = 0; i < AHCI_MAX_PORTS; ++i) {
        const AhciPortInfo *port = &info->ports[i];

        if (!port->implemented) continue;

        any = true;

        terminal_write("  PORT "); terminal_write_u64(port->port_number);
        terminal_write(": ");
        if (!port->present) terminal_write("NO DEVICE");
        else terminal_write(ahci_device_type_name(port->type));
        terminal_write("  SIG="); terminal_write_hex(port->signature);
        terminal_write("  SSTS="); terminal_write_hex(port->sata_status);

        /* Only SATA disks currently get their AHCI command engine/DMA memory initialized. */
        if (port->type == AHCI_DEVICE_SATA) { terminal_write("  ENGINE="); terminal_write(port->command_engine_ready ? "READY" : "FAILED"); }

        terminal_putchar('\n');

        /* Show the physical DMA structures for initialized SATA ports. */
        if (port->type == AHCI_DEVICE_SATA && port->command_engine_ready) {

            u64 clb = 0;
            u64 fis = 0;
            u64 table = 0;
            u64 identify = 0;

            if (ahci_port_dma_info(port->port_number, &clb, &fis, &table, &identify)) {

                terminal_write("    CLB="); terminal_write_hex(clb);
                terminal_write("  FIS="); terminal_write_hex(fis);
                terminal_putchar('\n');
                terminal_write("    CMD TABLE="); terminal_write_hex(table);
                terminal_write("  IDENTIFY="); terminal_write_hex(identify);
                terminal_putchar('\n');
            }
        }

        if (port->type == AHCI_DEVICE_SATA) {
            terminal_write("    IDENTIFY STATUS: ");
            terminal_writeln(port->identify_ok ? "READY" : "FAILED");

            if (port->identify_ok) {
                terminal_write("    MODEL: ");
                terminal_writeln(port->model);
                terminal_write("    SERIAL: ");
                terminal_writeln(port->serial);
                terminal_write("    LBA48: ");
                terminal_writeln(port->lba48 ? "YES" : "NO");
                terminal_write("    LOGICAL SECTOR: "); terminal_write_u64(port->logical_sector_size);
                terminal_writeln(" BYTES");
                terminal_write("    SECTORS: "); terminal_write_u64(port->sector_count); terminal_putchar('\n');

                if (port->logical_sector_size && port->sector_count <= (~0ULL / port->logical_sector_size)) {

                    u64 bytes = port->sector_count * port->logical_sector_size;

                    terminal_write("    CAPACITY: "); terminal_write_u64(bytes / (1024ULL * 1024ULL));
                    terminal_writeln(" MiB");
                }
            }
        }
    }

    if (!any) {
        terminal_writeln("  NO PORTS IMPLEMENTED.");
    }
}

static void command_disks(void) {
    u32 count = block_device_count();

    terminal_write("BLOCK DEVICES: ");
    terminal_write_u64(count); terminal_putchar('\n');

    for (u32 i = 0; i < count; ++i) {
        BlockDevice *device = block_device(i);

        if (!device) continue;

        terminal_write("  "); terminal_write(device->name);
        terminal_write("  TYPE="); terminal_write(block_type_name(device->type));
        terminal_putchar('\n'); terminal_write("    BLOCK SIZE: "); terminal_write_u64(device->block_size);
        terminal_writeln(" BYTES");
        terminal_write("    BLOCKS: "); terminal_write_u64(device->block_count); terminal_putchar('\n');

        u64 bytes = block_capacity_bytes(device);

        terminal_write("    CAPACITY: "); terminal_write_u64(bytes / (1024ULL * 1024ULL));
        terminal_writeln(" MiB");
        terminal_write("    MODE: ");
        terminal_writeln(device->read_only ? "READ-ONLY" : "READ-WRITE");
    }
}

static void command_sector(const char *argument) {
    u64 lba = 0;

    if (!parse_u64(argument, &lba)) { terminal_writeln("usage: sector LBA"); return; }

    BlockDevice *device = block_device(0);

    if (!device) { terminal_writeln("NO BLOCK DEVICE."); return; }
    if (device->block_size > 4096U) { terminal_writeln("BLOCK TOO LARGE."); return; }

    if (lba >= device->block_count) { terminal_writeln("LBA OUT OF RANGE."); return; }

    static u8 buffer[4096];

    if (!block_read(device, lba, 1, buffer)) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("SECTOR READ FAILED.");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("LBA "); terminal_write_u64(lba); terminal_write(" FROM ");
    terminal_writeln(device->name);

    /* First 128 bytes for now. */
    for (u32 row = 0; row < 8U; ++row) {
        terminal_write_hex(row * 16U);
        terminal_write(": ");

        for (u32 col = 0; col < 16U; ++col) { terminal_hex_byte(buffer[row * 16U + col]); terminal_putchar(' '); }

        terminal_putchar('\n');
    }

    if (lba == 0 && device->block_size >= 512U) {

        terminal_write("MBR SIGNATURE: ");
        terminal_hex_byte(buffer[510]); terminal_putchar(' '); terminal_hex_byte(buffer[511]); terminal_putchar('\n');
    }

    if (lba == 1 && device->block_size >= 8U) {

        bool gpt = buffer[0] == 'E' && buffer[1] == 'F' && buffer[2] == 'I' && buffer[3] == ' ' && buffer[4] == 'P' && buffer[5] == 'A' && buffer[6] == 'R' && buffer[7] == 'T';

        terminal_write("GPT SIGNATURE: ");
        terminal_writeln(gpt ? "VALID" : "NOT FOUND");
    }
}

static void command_partitions(void) {
    const GptInfo *gpt = gpt_get();

    if (!gpt || !gpt->valid) {
        terminal_writeln("NO VALID GPT.");
        return;
    }

    terminal_write("GPT PARTITIONS: ");
    terminal_write_u64(gpt->partition_count);
    terminal_putchar('\n');

    for (u32 i = 0; i < gpt->partition_count; ++i) {

        const GptPartition *part = &gpt->partitions[i];

        if (!part->valid) continue;

        terminal_write("  sda"); terminal_write_u64(part->index);

        if (part->name[0]) { terminal_write("  "); terminal_write(part->name); }

        terminal_putchar('\n');
        terminal_write("    FIRST LBA: ");
        terminal_write_u64(part->first_lba);
        terminal_putchar('\n');
        terminal_write("    LAST LBA: ");
        terminal_write_u64(part->last_lba);
        terminal_putchar('\n');

        u64 sectors = part->last_lba - part->first_lba + 1ULL;

        terminal_write("    SECTORS: ");
        terminal_write_u64(sectors);
        terminal_putchar('\n');

        if (gpt->device) {
            u64 bytes = sectors * gpt->device->block_size;

            terminal_write("    SIZE: ");
            terminal_write_u64(bytes / (1024ULL * 1024ULL));
            terminal_writeln(" MiB");
        }
    }
}

static NORETURN void command_reboot(void) {
    terminal_set_color(terminal_accent_color());
    terminal_writeln("REBOOTING...");
    interrupts_disable();
    (void)acpi_try_reset();
    for (volatile u64 i = 0; i < 10000000ULL; ++i) arch_pause();
    for (u32 i = 0; i < 200000; ++i) {
        if (!(arch_in8(0x64) & 0x02)) break;
        arch_pause();
    }
    arch_out8(0x64, 0xFE);
    for (volatile u64 i = 0; i < 10000000ULL; ++i) arch_pause();
    arch_triple_fault();
}

static void command_pwd(void) {
    char path[VFS_PATH_MAX];
    if (!vfs_get_path(g_cwd, path, sizeof(path))) {
        terminal_writeln("pwd: unable to determine path");
        return;
    }
    terminal_writeln(path);
}

static void command_ls(void) {
    if (!g_cwd) return;
    for (VfsNode *node = g_cwd->first_child; node; node = node->next_sibling) {
        terminal_write(node->name);
        if (node->type == VFS_DIRECTORY) terminal_putchar('/');
        terminal_putchar('\n');
    }
}

static void command_cd(const char *path) {
    if (!path || !*path) { g_cwd = vfs_root(); return; }

    VfsNode *node = vfs_resolve(g_cwd, path);

    if (!node) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("cd: path not found");
        terminal_set_color(terminal_default_color());
        return;
    }
    
    if (node->type != VFS_DIRECTORY) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("cd: not a directory");
        terminal_set_color(terminal_default_color());
        return;
    }

    g_cwd = node;
}

static void command_cat(const char *path) {
    VfsNode *node = vfs_resolve(g_cwd, path);

    if (!node) { terminal_writeln("cat: file not found"); return;}
    if (node->type != VFS_FILE) { terminal_writeln("cat: not a file"); return;}
    for (u64 i = 0; i < node->size; ++i) terminal_putchar((char)node->data[i]);
    if (node->size && node->data[node->size - 1] != '\n') terminal_putchar('\n');
}

static void execute(char *line) {
    char *command = trim(line);
    if (!*command) return;
    char *args = command;

    while (*args && !k_ascii_space(*args)) ++args;
    if (*args) {
        *args++ = 0; 
        args = trim(args); 
    } else { 
        args = ""; 
    }

    if (k_strieq(command, "help")) command_help();
    else if (k_strieq(command, "about")) command_about();
    else if (k_strieq(command, "clear")) terminal_clear();
    else if (k_strieq(command, "memory")) command_memory();
    else if (k_strieq(command, "alloc")) command_alloc();
    else if (k_strieq(command, "cpu")) command_cpu();
    else if (k_strieq(command, "interrupts")) command_interrupts();
    else if (k_strieq(command, "acpi")) command_acpi();
    else if (k_strieq(command, "pci")) command_pci();
    else if (k_strieq(command, "ahci")) command_ahci();
    else if (k_strieq(command, "disks")) command_disks();
    else if (k_strieq(command, "sector")) command_sector(args);
    else if (k_strieq(command, "partitions")) command_partitions();
    else if (k_strieq(command, "fault")) __asm__ volatile ("ud2");
    else if (k_strieq(command, "reboot")) command_reboot();
    else if (k_strieq(command, "pwd")) command_pwd();
    else if (k_strieq(command, "ls")) command_ls();
    else if (k_strieq(command, "cd")) command_cd(args);
    else if (k_strieq(command, "cat")) command_cat(args);
    else {
        terminal_set_color(terminal_error_color()); terminal_write("UNKNOWN COMMAND: ");
        terminal_writeln(command);
        terminal_set_color(terminal_default_color());
        terminal_writeln("TYPE help FOR AVAILABLE COMMANDS.");
    }
}

static int next_input(void) {
    int c;
    interrupts_disable();
    ps2_poll();
    c = ps2_getchar();
    interrupts_enable();
    if (c >= 0) return c;
    return serial_read_nonblocking();
}

NORETURN void shell_run(const BootInfo *boot) {
    g_boot = boot; g_length = 0; g_cwd = vfs_root();
    prompt();
    for (;;) {
        int input = next_input();
        if (input < 0) {
            arch_pause();
            continue;
        }
        char c = (char)input;
        if (c == '\r') c = '\n';
        if ((u8)c == 0x7F) c = '\b';
        if (c == '\n') {
            terminal_putchar('\n');
            g_input[g_length] = 0;
            execute(g_input);
            g_length = 0;
            prompt();
        } else if (c == '\b') {
            if (g_length) {
                --g_length;
                terminal_putchar('\b');
            }
        } else if ((u8)c >= 32 && (u8)c <= 126 && g_length + 1 < INPUT_CAPACITY) {
            g_input[g_length++] = c;
            terminal_putchar(c);
        }
    }
}
