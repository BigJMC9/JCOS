#include "shell.h"
#include "acpi.h"
#include "arch.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "lib.h"
#include "pmm.h"
#include "ps2.h"
#include "serial.h"
#include "terminal.h"

#define INPUT_CAPACITY 128U

static const BootInfo *g_boot;
static char g_input[INPUT_CAPACITY];
static u32 g_length;

static void prompt(void) {
    terminal_set_color(terminal_accent_color());
    terminal_write("JA> ");
    terminal_set_color(terminal_default_color());
}

static char *trim(char *s) {
    while (*s && k_ascii_space(*s)) ++s;
    char *end = s + k_strlen(s);
    while (end > s && k_ascii_space(end[-1])) --end;
    *end = 0;
    return s;
}

static void print_mib(u64 pages) {
    terminal_write_u64((pages * 4096ULL) / (1024ULL * 1024ULL));
    terminal_write(" MiB");
}

static void command_help(void) {
    terminal_writeln("COMMANDS:");
    terminal_writeln("  help        show this list");
    terminal_writeln("  about       describe this kernel");
    terminal_writeln("  clear       clear framebuffer and serial terminal");
    terminal_writeln("  memory      show UEFI memory-map and allocator state");
    terminal_writeln("  alloc       allocate one physical 4 KiB page");
    terminal_writeln("  cpu         show CPUID information");
    terminal_writeln("  interrupts  show APIC/PIC and keyboard counters");
    terminal_writeln("  acpi        show ACPI discovery results");
    terminal_writeln("  fault       deliberately execute UD2 to test the IDT");
    terminal_writeln("  reboot      reset via ACPI, keyboard controller, or triple fault");
}

static void command_about(void) {
    terminal_writeln("JA OS V5 IS A FREESTANDING X86_64 UEFI KERNEL.");
    terminal_writeln("THE EFI LOADER LOADS KERNEL.ELF, CAPTURES GOP + ACPI + MEMORY MAP,");
    terminal_writeln("CALLS EXITBOOTSERVICES(), THEN AN ASSEMBLY SHIM ENTERS THIS C KERNEL.");
    terminal_writeln("INPUT: PS/2 SET-1 KEYBOARD WITH IRQ + POLLING, OR COM1 SERIAL.");
}

static void command_memory(void) {
    PmmStats stats = pmm_stats();
    terminal_write("UEFI DESCRIPTORS: ");
    terminal_write_u64(g_boot->memory_map_descriptor_size ?
                       g_boot->memory_map_size / g_boot->memory_map_descriptor_size : 0);
    terminal_putchar('\n');
    terminal_write("ALLOCATOR RANGES: "); terminal_write_u64(stats.range_count); terminal_putchar('\n');
    terminal_write("CONVENTIONAL MEMORY: "); print_mib(stats.total_pages); terminal_putchar('\n');
    terminal_write("FREE BUMP-ALLOCATOR MEMORY: "); print_mib(stats.free_pages); terminal_putchar('\n');
    terminal_write("FREE PAGES: "); terminal_write_u64(stats.free_pages); terminal_putchar('\n');
    terminal_write("KERNEL BASE: "); terminal_write_hex(g_boot->kernel_base); terminal_putchar('\n');
    terminal_write("KERNEL SIZE: "); terminal_write_u64(g_boot->kernel_size); terminal_writeln(" bytes");
    if (stats.discarded_ranges) {
        terminal_write("DISCARDED EXTRA RANGES: "); terminal_write_u64(stats.discarded_ranges); terminal_putchar('\n');
    }
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
    k_memcpy(vendor + 0, &b, 4);
    k_memcpy(vendor + 4, &d, 4);
    k_memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    u32 max_basic = a;

    char brand[49];
    k_memset(brand, 0, sizeof(brand));
    arch_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    u32 max_extended = a;
    if (max_extended >= 0x80000004U) {
        u32 *words = (u32 *)(void *)brand;
        for (u32 leaf = 0; leaf < 3; ++leaf)
            arch_cpuid(0x80000002U + leaf, 0, &words[leaf * 4], &words[leaf * 4 + 1],
                       &words[leaf * 4 + 2], &words[leaf * 4 + 3]);
        brand[48] = 0;
    }

    arch_cpuid(1, 0, &a, &b, &c, &d);
    u32 family = (a >> 8) & 0xF;
    u32 model = (a >> 4) & 0xF;
    u32 stepping = a & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF) model += ((a >> 16) & 0xF) << 4;

    terminal_write("VENDOR: "); terminal_writeln(vendor);
    if (brand[0]) { terminal_write("BRAND: "); terminal_writeln(brand); }
    terminal_write("FAMILY: "); terminal_write_u64(family);
    terminal_write(" MODEL: "); terminal_write_u64(model);
    terminal_write(" STEPPING: "); terminal_write_u64(stepping); terminal_putchar('\n');
    terminal_write("APIC: "); terminal_write((d & (1U << 9)) ? "YES" : "NO");
    terminal_write("  X2APIC: "); terminal_write((c & (1U << 21)) ? "YES" : "NO");
    terminal_write("  SSE2: "); terminal_writeln((d & (1U << 26)) ? "YES" : "NO");
    bool long_mode = false;
    if (max_extended >= 0x80000001U) {
        arch_cpuid(0x80000001U, 0, &a, &b, &c, &d);
        long_mode = (d & (1U << 29)) != 0;
    }
    terminal_write("LONG MODE: "); terminal_writeln(long_mode ? "YES" : "NO");
    terminal_write("MAX BASIC CPUID LEAF: "); terminal_write_hex(max_basic); terminal_putchar('\n');
}

static void command_interrupts(void) {
    InterruptControllerInfo info = interrupt_controller_info();
    terminal_write("CONTROLLER: "); terminal_writeln(interrupt_controller_name());
    terminal_write("KEYBOARD GSI: "); terminal_write_u64(info.keyboard_gsi); terminal_putchar('\n');
    if (info.mode == INTERRUPT_CONTROLLER_APIC) {
        terminal_write("LOCAL APIC ID: "); terminal_write_u64(info.local_apic_id); terminal_putchar('\n');
    }
    terminal_write("VECTOR 0x21 COUNT: "); terminal_write_u64(interrupt_count(0x21)); terminal_putchar('\n');
    terminal_write("PS/2 IRQ HANDLER COUNT: "); terminal_write_u64(ps2_irq_count()); terminal_putchar('\n');
    terminal_write("PS/2 SCANCODE BYTES: "); terminal_write_u64(ps2_scancode_count()); terminal_putchar('\n');
    terminal_write("DROPPED KEY EVENTS: "); terminal_write_u64(ps2_dropped_count()); terminal_putchar('\n');
    terminal_write("LOCAL APIC SPURIOUS: "); terminal_write_u64(interrupt_spurious_count()); terminal_putchar('\n');
}

static void command_acpi(void) {
    const AcpiInfo *info = acpi_get();
    terminal_write("RSDP: "); terminal_write_hex(info->rsdp_address); terminal_putchar('\n');
    terminal_write("ACPI VALID: "); terminal_writeln(info->valid ? "YES" : "NO");
    terminal_write("MADT VALID: "); terminal_writeln(info->madt_valid ? "YES" : "NO");
    terminal_write("LOCAL APIC ADDRESS: "); terminal_write_hex(info->lapic_address); terminal_putchar('\n');
    terminal_write("IO APIC COUNT: "); terminal_write_u64(info->io_apic_count); terminal_putchar('\n');
    terminal_write("KEYBOARD GSI: "); terminal_write_u64(info->keyboard_gsi); terminal_putchar('\n');
    terminal_write("8042 CONTROLLER: ");
    terminal_writeln(!info->i8042_known ? "UNKNOWN" : (info->i8042_present ? "PRESENT" : "ABSENT"));
    terminal_write("ACPI RESET REGISTER: "); terminal_writeln(info->reset_supported ? "YES" : "NO");
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

static void execute(char *line) {
    char *command = trim(line);
    if (!*command) return;
    if (k_strieq(command, "help")) command_help();
    else if (k_strieq(command, "about")) command_about();
    else if (k_strieq(command, "clear")) terminal_clear();
    else if (k_strieq(command, "memory")) command_memory();
    else if (k_strieq(command, "alloc")) command_alloc();
    else if (k_strieq(command, "cpu")) command_cpu();
    else if (k_strieq(command, "interrupts")) command_interrupts();
    else if (k_strieq(command, "acpi")) command_acpi();
    else if (k_strieq(command, "fault")) __asm__ volatile ("ud2");
    else if (k_strieq(command, "reboot")) command_reboot();
    else {
        terminal_set_color(terminal_error_color());
        terminal_write("UNKNOWN COMMAND: "); terminal_writeln(command);
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
    g_boot = boot;
    g_length = 0;
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
