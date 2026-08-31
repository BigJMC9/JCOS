#include "shell.h"
#include "acpi.h"
#include "arch.h"
#include "interrupt_controller.h"
#include "interrupts.h"
#include "lib.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "address_space.h"
#include "physmap.h"
#include "ps2.h"
#include "serial.h"
#include "terminal.h"
#include "vfs.h"
#include "ahci.h"
#include "block.h"
#include "gpt.h"
#include "fat32.h"
#include "thread.h"
#include "gdt.h"
#include "scheduler.h"

#define INPUT_CAPACITY 128U

static VfsNode *g_cwd;

static const BootInfo *g_boot;
static char g_input[INPUT_CAPACITY];
static u32 g_length;

static Thread *g_switchtest_main;
static Thread *g_switchtest_a;
static Thread *g_switchtest_b;

static volatile u64 g_switchtest_a_count;
static volatile u64 g_switchtest_b_count;
static volatile bool g_switchtest_failed;

static volatile u64 g_schedtest_a_count;
static volatile u64 g_schedtest_b_count;
static volatile bool g_schedtest_failed;

static volatile bool g_exittest_started;

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

static bool next_argument(const char **cursor, char *output, u32 capacity) {
    if (!cursor || !*cursor || !output || capacity == 0) return false;

    const char *p = *cursor;

    while (*p && k_ascii_space(*p)) ++p;
    if (!*p) {
        output[0] = 0;
        *cursor = p;
        return false;
    }

    u32 length = 0;

    while (*p && !k_ascii_space(*p)) {
        if (length + 1U >= capacity) return false;

        output[length++] = *p++;
    }

    output[length] = 0;
    *cursor = p;

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
    terminal_writeln("  alloc       allocate one physical 4 KiB frame");
    terminal_writeln("  frametest   test PMM allocation/free/reuse");
    terminal_writeln("  vmmtest     test x86-64 page-table operations");
    terminal_writeln("  astest      test address-space ownership/sharing");
    terminal_writeln("  threadtest  test thread lifecycle and kernel stacks");
    terminal_writeln("  switchtest  test cooperative thread context switches");
    terminal_writeln("  schedtest   test cooperative round-robin scheduling");
    terminal_writeln("  exittest    test permanent current-thread termination");
    terminal_writeln("  syscalltest test Ring3 syscall round-trip");
    terminal_writeln("  cpu         show CPUID information");
    terminal_writeln("  interrupts  show APIC/PIC and keyboard counters");
    terminal_writeln("  acpi        show ACPI discovery results");
    terminal_writeln("  pci         list discovered PCI devices"); 
    terminal_writeln("  ahci        show AHCI controller and SATA ports"); 
    terminal_writeln("  sector LBA  dump a raw disk sector");
    terminal_writeln("  partitions  list GPT partitions");
    terminal_writeln("  fatls       list FAT32 root directory");
    terminal_writeln("  fat32       show FAT32 filesystem information");
    terminal_writeln("  fatread FILE [OFFSET] [COUNT]  read/test a FAT32 root file");
    terminal_writeln("  fault       deliberately execute UD2 to test the IDT");
    terminal_writeln("  userfault   enter Ring3 and deliberately execute UD2");
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

    terminal_write("UEFI DESCRIPTORS: "); terminal_write_u64(g_boot->memory_map_descriptor_size ? g_boot->memory_map_size / g_boot->memory_map_descriptor_size : 0); terminal_putchar('\n');
    terminal_write("ALLOCATOR RANGES: "); terminal_write_u64(stats.range_count); terminal_putchar('\n');
    terminal_write("MANAGED MEMORY: ");
    print_mib(stats.total_pages);
    terminal_putchar('\n');
    terminal_write("TOTAL FRAMES: "); terminal_write_u64(stats.total_pages); terminal_putchar('\n');
    terminal_write("FREE FRAME MEMORY: ");
    print_mib(stats.free_pages);
    terminal_putchar('\n');
    terminal_write("FREE FRAMES: "); terminal_write_u64(stats.free_pages); terminal_putchar('\n');
    terminal_write("USED/RESERVED FRAMES: "); terminal_write_u64(stats.total_pages - stats.free_pages); terminal_putchar('\n');
    terminal_write("PMM BITMAP: "); terminal_write_hex(stats.bitmap_physical);
    terminal_write("  PAGES: "); terminal_write_u64(stats.bitmap_pages); terminal_putchar('\n');
    terminal_write("KERNEL BASE: "); terminal_write_hex(g_boot->kernel_base); terminal_putchar('\n');
    terminal_write("KERNEL SIZE: "); terminal_write_u64(g_boot->kernel_size);
    terminal_writeln(" bytes");

    if (stats.discarded_ranges) { terminal_write("DISCARDED EXTRA RANGES: "); terminal_write_u64(stats.discarded_ranges); terminal_putchar('\n'); }
}

static void command_alloc(void) {
    frame_t frame = frame_alloc();

    if (frame == FRAME_INVALID) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("OUT OF PHYSICAL FRAMES.");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("ALLOCATED FRAME: "); terminal_write_u64(frame); terminal_write("  PHYSICAL: "); terminal_write_hex(frame_to_phys(frame)); terminal_putchar('\n');
    terminal_writeln("FRAME REMAINS ALLOCATED.");
}

static void command_frametest(void) {
    terminal_writeln("PMM FRAME TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    frame_t a = frame_alloc();
    frame_t b = frame_alloc();
    frame_t c = frame_alloc();

    if (a == FRAME_INVALID || b == FRAME_INVALID || c == FRAME_INVALID) {
        if (a != FRAME_INVALID) (void)frame_free(a);
        if (b != FRAME_INVALID) (void)frame_free(b);
        if (c != FRAME_INVALID) (void)frame_free(c);

        terminal_set_color(terminal_error_color());
        terminal_writeln("  ALLOCATION: FAILED");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("  A: FRAME "); terminal_write_u64(a); terminal_write("  PHYS="); terminal_write_hex(frame_to_phys(a)); terminal_putchar('\n');
    terminal_write("  B: FRAME "); terminal_write_u64(b); terminal_write("  PHYS="); terminal_write_hex(frame_to_phys(b)); terminal_putchar('\n');
    terminal_write("  C: FRAME "); terminal_write_u64(c); terminal_write("  PHYS="); terminal_write_hex(frame_to_phys(c)); terminal_putchar('\n');

    bool distinct = a != b && a != c && b != c;

    bool freed_b = frame_free(b);
    frame_t d = frame_alloc();
    bool reused = freed_b && d != FRAME_INVALID && d == b;

    terminal_write("  REUSE FREED FRAME: ");
    terminal_writeln(reused ? "PASS" : "FAILED");

    bool freed_a = frame_free(a);
    bool freed_c = frame_free(c);
    bool freed_d = false;

    if (d != FRAME_INVALID) freed_d = frame_free(d);

    /*
     * D has already been freed above.
     *
     * A second free must be rejected.
     */
    bool double_free_rejected = d != FRAME_INVALID && !frame_free(d);

    terminal_write("  DOUBLE FREE REJECTED: ");
    terminal_writeln(double_free_rejected ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();

    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n');

    bool count_restored = before.free_pages == after.free_pages;
    bool pass = distinct && reused && freed_a && freed_c && freed_d && double_free_rejected && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); terminal_write("PMM FRAME TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_vmmtest(void) {
    terminal_writeln("VMM PAGE TABLE TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    VmPageMap map;

    if (!vmm_page_map_create(&map)) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("  CREATE PAGE MAP: FAILED");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("  ROOT FRAME: "); terminal_write_u64(map.root_frame); terminal_write("  PHYS="); terminal_write_hex(frame_to_phys(map.root_frame)); terminal_putchar('\n');

    frame_t data = frame_alloc();

    if (data == FRAME_INVALID) {

        vmm_page_map_destroy(&map);
        terminal_set_color(terminal_error_color());
        terminal_writeln("  DATA FRAME: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("  DATA FRAME: "); terminal_write_u64(data); terminal_write("  PHYS="); terminal_write_hex(frame_to_phys(data)); terminal_putchar('\n');

    const u64 test_virtual = 0x40000000ULL;
    bool mapped = vmm_map_page(&map, test_virtual, data, VM_WRITE);

    terminal_write("  MAP: ");
    terminal_writeln(mapped ? "PASS" : "FAILED");

    frame_t queried_frame = FRAME_INVALID;
    vm_flags_t queried_flags = 0;
    bool queried = mapped && vmm_query_page(&map, test_virtual, &queried_frame, &queried_flags);

    terminal_write("  QUERY: ");
    terminal_writeln(queried ? "PASS" : "FAILED");

    bool frame_match = queried && queried_frame == data;

    terminal_write("  FRAME MATCH: ");
    terminal_writeln(frame_match ? "PASS" : "FAILED");

    bool flags_match = queried && queried_flags == VM_WRITE;

    terminal_write("  FLAGS MATCH: ");
    terminal_writeln(flags_match ? "PASS" : "FAILED");

    /* Mapping same virtual page again must fail rather than silently replacing it. */
    bool duplicate_rejected = mapped && !vmm_map_page(&map, test_virtual, data, VM_WRITE);

    terminal_write("  DUPLICATE MAP REJECTED: ");
    terminal_writeln(duplicate_rejected ? "PASS" : "FAILED");

    frame_t old_frame = FRAME_INVALID;
    bool unmapped = mapped && vmm_unmap_page(&map, test_virtual, &old_frame);

    terminal_write("  UNMAP: ");
    terminal_writeln(unmapped ? "PASS" : "FAILED");

    bool old_frame_match = unmapped && old_frame == data;

    terminal_write("  OLD FRAME MATCH: ");
    terminal_writeln(old_frame_match ? "PASS" : "FAILED");

    frame_t after_frame = FRAME_INVALID;
    vm_flags_t after_flags = 0;
    bool query_after_unmap_rejected = unmapped && !vmm_query_page(&map, test_virtual, &after_frame, &after_flags);

    terminal_write("  QUERY AFTER UNMAP: ");
    terminal_writeln(query_after_unmap_rejected ? "PASS" : "FAILED");

    /* The VMM removed the mapping but deliberately did not free the data frame. */
    bool data_freed = frame_free(data);

    /* Frees the PML4 and any remaining paging-structure frames. */
    vmm_page_map_destroy(&map);

    PmmStats after = pmm_stats();

    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n');

    bool count_restored = before.free_pages == after.free_pages;

    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = mapped && queried && frame_match && flags_match && duplicate_rejected && unmapped && old_frame_match && query_after_unmap_rejected && data_freed && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); terminal_write("VMM PAGE TABLE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_astest(void) {
    terminal_writeln("ADDRESS SPACE TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    AddressSpace space;
    bool created = address_space_create(&space);

    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) return;

    terminal_write("  ID: "); terminal_write_u64(space.id); terminal_write("  CR3: "); terminal_write_hex(address_space_cr3(&space)); terminal_putchar('\n');

    /* The low kernel mapping must be shared but supervisor-only. */
    u64 kernel_page = g_boot->kernel_base & ~(VM_PAGE_SIZE - 1ULL);
    frame_t kernel_frame = FRAME_INVALID;
    vm_flags_t kernel_flags = 0;
    bool kernel_shared = address_space_query_page(&space, kernel_page, &kernel_frame, &kernel_flags) && kernel_frame == phys_to_frame(kernel_page) && !(kernel_flags & VM_USER);

    terminal_write("  KERNEL MAPPING SHARED: ");
    terminal_writeln(kernel_shared ? "PASS" : "FAILED");

    /* The physical direct map must also be shared and supervisor-only. */
    u64 bitmap_direct = 0;
    bool physmap_shared = false;

    if (physmap_virtual_address(before.bitmap_physical, &bitmap_direct)) {
        frame_t mapped = FRAME_INVALID;
        vm_flags_t flags = 0;

        physmap_shared = address_space_query_page(&space, bitmap_direct, &mapped, &flags) && mapped == phys_to_frame(before.bitmap_physical) && !(flags & VM_USER);
    }

    terminal_write("  PHYSMAP SHARED: ");
    terminal_writeln(physmap_shared ? "PASS" : "FAILED");

    frame_t data = frame_alloc();
    bool data_ok = data != FRAME_INVALID;

    bool mapped = false;
    bool queried = false;
    bool flags_match = false;

    if (data_ok) {

        mapped = address_space_map_page(&space, ADDRESS_SPACE_USER_BASE, data, VM_WRITE);

        frame_t queried_frame = FRAME_INVALID;
        vm_flags_t queried_flags = 0;

        queried = mapped && address_space_query_page(&space, ADDRESS_SPACE_USER_BASE, &queried_frame, &queried_flags);
        flags_match = queried && queried_frame == data && queried_flags == (VM_WRITE | VM_USER);
    }

    terminal_write("  USER MAP: ");
    terminal_writeln(mapped ? "PASS" : "FAILED");
    terminal_write("  USER QUERY/FLAGS: ");
    terminal_writeln(flags_match ? "PASS" : "FAILED");

    bool low_rejected = data_ok && !address_space_map_page(&space, 0x40000000ULL, data, VM_WRITE);

    terminal_write("  LOW KERNEL SLOT REJECTED: ");
    terminal_writeln(low_rejected ? "PASS" : "FAILED");

    bool high_rejected = data_ok && !address_space_map_page(&space, PHYS_MAP_BASE, data, VM_WRITE);

    terminal_write("  KERNEL HALF REJECTED: ");
    terminal_writeln(high_rejected ? "PASS" : "FAILED");

    frame_t old_frame = FRAME_INVALID;
    bool unmapped = mapped && address_space_unmap_page(&space, ADDRESS_SPACE_USER_BASE, &old_frame);
    bool old_match = unmapped && old_frame == data;

    terminal_write("  USER UNMAP: ");
    terminal_writeln(unmapped ? "PASS" : "FAILED");

    address_space_destroy(&space);

    bool data_freed = data_ok && frame_free(data);
    PmmStats after = pmm_stats();
    bool count_restored = before.free_pages == after.free_pages;

    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n'); 
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = created && kernel_shared && physmap_shared && data_ok && mapped && queried && flags_match && low_rejected && high_rejected && unmapped && old_match && data_freed && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); terminal_write("ADDRESS SPACE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_threadtest(void) {
    terminal_writeln("THREAD TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    Thread *current = thread_current();
    bool bootstrap_ok = current && current->id == 1 && current->state == THREAD_STATE_RUNNING && current->address_space == address_space_kernel() && current->kernel_stack_top == gdt_rsp0() && !current->owns_kernel_stack;

    terminal_write("  BOOTSTRAP THREAD: ");
    terminal_writeln(bootstrap_ok ? "PASS" : "FAILED");

    AddressSpace space;
    bool space_created = address_space_create(&space);

    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_created ? "PASS" : "FAILED");

    if (!space_created) return;

    Thread thread;
    bool created = thread_create(&thread, &space);

    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) {
        address_space_destroy(&space);

        return;
    }

    terminal_write("  ID: "); terminal_write_u64(thread.id);
    terminal_write("  STACK PHYS: "); terminal_write_hex(thread.kernel_stack_physical); terminal_putchar('\n'); 
    terminal_write("  STACK BASE: "); terminal_write_hex(thread.kernel_stack_base); terminal_write("  TOP: "); terminal_write_hex(thread.kernel_stack_top); terminal_putchar('\n');

    bool metadata_ok = thread.id != current->id && thread.address_space == &space && thread.state == THREAD_STATE_READY && thread.kernel_stack_size == THREAD_KERNEL_STACK_SIZE && thread.owns_kernel_stack;

    terminal_write("  METADATA: ");
    terminal_writeln(metadata_ok ? "PASS" : "FAILED");

    /* The stack is reached through the shared supervisor-only physical direct map. */
    frame_t first_frame = FRAME_INVALID;
    vm_flags_t first_flags = 0;

    bool first_mapped = address_space_query_page(&space, thread.kernel_stack_base, &first_frame, &first_flags) && first_frame == phys_to_frame(thread.kernel_stack_physical) && (first_flags & VM_WRITE) && !(first_flags & VM_USER);

    u64 last_virtual = thread.kernel_stack_top - FRAME_SIZE;
    u64 last_physical = thread.kernel_stack_physical + thread.kernel_stack_size - FRAME_SIZE;

    frame_t last_frame = FRAME_INVALID;
    vm_flags_t last_flags = 0;

    bool last_mapped = address_space_query_page(&space, last_virtual, &last_frame, &last_flags) && last_frame == phys_to_frame(last_physical) && (last_flags & VM_WRITE) && !(last_flags & VM_USER);
    bool stack_mapped = first_mapped && last_mapped;

    terminal_write("  STACK PHYSMAP: ");
    terminal_writeln(stack_mapped ? "PASS" : "FAILED");

    /* Touch both ends of the allocated stack. */
    volatile u64 *first_word = (volatile u64 *)(u64) thread.kernel_stack_base;

    volatile u64 *last_word = (volatile u64 *)(u64) (thread.kernel_stack_top - sizeof(u64));

    const u64 marker = 0x4A434F5354485244ULL;

    *first_word = marker;
    *last_word = ~marker;

    bool stack_rw = *first_word == marker && *last_word == ~marker;

    terminal_write("  STACK READ/WRITE: ");
    terminal_writeln(stack_rw ? "PASS" : "FAILED");

    /* Simulate the scheduler's future RSP0 update. RSP0 is only consumed on CPL3 -> CPL0 entry. */
    u64 original_cr3 = arch_read_cr3() & ~0xFFFULL;
    u64 original_rsp0 = gdt_rsp0();

    /* thread_activate() does not switch the live kernel RSP, so keep IRQs disabled while this structural activation test is in progress. */
    interrupts_disable();

    bool activated = thread_activate(&thread);
    bool current_switched = activated && thread_current() == &thread;
    bool state_switched = activated && thread.state == THREAD_STATE_RUNNING && current->state == THREAD_STATE_READY;
    bool cr3_switched = activated && (arch_read_cr3() & ~0xFFFULL) == address_space_cr3(&space);
    bool rsp0_switched = activated && gdt_rsp0() == thread.kernel_stack_top;
    bool restored = activated && thread_activate(current);
    bool current_restored = restored && thread_current() == current && current->state == THREAD_STATE_RUNNING && thread.state == THREAD_STATE_READY;
    bool cr3_restored = restored && (arch_read_cr3() & ~0xFFFULL) == original_cr3;
    bool rsp0_restored = restored && gdt_rsp0() == original_rsp0;

    interrupts_enable();

    terminal_write("  ACTIVATE: ");
    terminal_writeln(activated ? "PASS" : "FAILED");
    terminal_write("  CURRENT SWITCH: ");
    terminal_writeln(current_switched ? "PASS" : "FAILED");
    terminal_write("  STATE SWITCH: ");
    terminal_writeln(state_switched ? "PASS" : "FAILED");
    terminal_write("  CR3 SWITCH: ");
    terminal_writeln(cr3_switched ? "PASS" : "FAILED");
    terminal_write("  RSP0 SWITCH: ");
    terminal_writeln(rsp0_switched ? "PASS" : "FAILED");
    terminal_write("  RESTORE CURRENT: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  CR3 RESTORE: ");
    terminal_writeln(cr3_restored ? "PASS" : "FAILED");
    terminal_write("  RSP0 RESTORE: ");
    terminal_writeln(rsp0_restored ? "PASS" : "FAILED");

    bool destroyed = thread_destroy(&thread);

    address_space_destroy(&space);

    PmmStats after = pmm_stats();
    bool count_restored = before.free_pages == after.free_pages;

    terminal_write("  DESTROY: ");
    terminal_writeln(destroyed ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n'); 
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = bootstrap_ok && space_created && created && metadata_ok && stack_mapped && stack_rw && activated && current_switched && state_switched && cr3_switched && rsp0_switched && restored && current_restored && cr3_restored && rsp0_restored && destroyed && count_restored;
    
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); 
    terminal_write("THREAD TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void switchtest_thread_a(void *argument) {
    (void)argument;
    for (u32 i = 0; i < 3U; ++i) {
        ++g_switchtest_a_count;
        if (!thread_switch(g_switchtest_b)) {
            g_switchtest_failed = true;
            (void)thread_switch(g_switchtest_main);
            cpu_halt_forever();
        }
    }
    /* Return control to the shell thread. */
    if (!thread_switch(g_switchtest_main)) g_switchtest_failed = true;
    cpu_halt_forever();
}

static void switchtest_thread_b(void *argument) {
    (void)argument;
    for (u32 i = 0; i < 3U; ++i) {
        ++g_switchtest_b_count;
        if (!thread_switch(g_switchtest_a)) {
            g_switchtest_failed = true;
            (void)thread_switch(g_switchtest_main);
            cpu_halt_forever();
        }
    }
    /* Under the expected sequence B remains suspended after its third switch to A. */
    (void)thread_switch(g_switchtest_main);
    cpu_halt_forever();
}

static void command_switchtest(void) {
    terminal_writeln("CONTEXT SWITCH TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread a;
    Thread b;

    AddressSpace *kernel_space = address_space_kernel();
    bool created_a = thread_create(&a, kernel_space);
    bool created_b = created_a && thread_create(&b, kernel_space);

    terminal_write("  CREATE A: ");
    terminal_writeln(created_a ? "PASS" : "FAILED");
    terminal_write("  CREATE B: ");
    terminal_writeln(created_b ? "PASS" : "FAILED");

    if (!created_a || !created_b) {
        if (created_a) (void)thread_destroy(&a);

        return;
    }

    bool prepared_a = thread_prepare_kernel(&a, switchtest_thread_a, 0);
    bool prepared_b = thread_prepare_kernel(&b, switchtest_thread_b, 0);

    terminal_write("  PREPARE A: ");
    terminal_writeln(prepared_a ? "PASS" : "FAILED");
    terminal_write("  PREPARE B: ");
    terminal_writeln(prepared_b ? "PASS" : "FAILED");

    if (!prepared_a || !prepared_b) {

        (void)thread_destroy(&a);
        (void)thread_destroy(&b);

        return;
    }

    g_switchtest_main = main_thread;
    g_switchtest_a = &a;
    g_switchtest_b = &b;
    g_switchtest_a_count = 0;
    g_switchtest_b_count = 0;
    g_switchtest_failed = false;

    /* No IRQ may run while the live kernel RSP moves between cooperative contexts. */
    interrupts_disable();

    bool switched = thread_switch(&a);

    interrupts_enable();

    bool current_restored = switched && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool counts_ok = g_switchtest_a_count == 3 && g_switchtest_b_count == 3;
    bool child_states_ok = a.state == THREAD_STATE_READY && b.state == THREAD_STATE_READY;

    terminal_write("  SWITCH SEQUENCE: ");
    terminal_writeln(switched && !g_switchtest_failed ? "PASS" : "FAILED");
    terminal_write("  A COUNT: "); terminal_write_u64(g_switchtest_a_count); terminal_putchar('\n'); terminal_write("  B COUNT: "); terminal_write_u64(g_switchtest_b_count); terminal_putchar('\n'); terminal_write("  CURRENT RESTORED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  CHILD STATES: ");
    terminal_writeln(child_states_ok ? "PASS" : "FAILED");

    bool destroyed_a = thread_destroy(&a);
    bool destroyed_b = thread_destroy(&b);

    g_switchtest_main = 0;
    g_switchtest_a = 0;
    g_switchtest_b = 0;

    PmmStats after = pmm_stats();
    bool count_restored = before.free_pages == after.free_pages;

    terminal_write("  DESTROY A: ");
    terminal_writeln(destroyed_a ? "PASS" : "FAILED");
    terminal_write("  DESTROY B: ");
    terminal_writeln(destroyed_b ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n'); terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = created_a && created_b && prepared_a && prepared_b && switched && !g_switchtest_failed && counts_ok && current_restored && child_states_ok && destroyed_a && destroyed_b && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); terminal_write("CONTEXT SWITCH TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void schedtest_worker(void *argument) {
    volatile u64 *counter = (volatile u64 *)argument;

    if (!counter) {
        g_schedtest_failed = true;
        cpu_halt_forever();
    }

    for (u32 i = 0; i < 3U; ++i) {
        ++(*counter);
        if (!scheduler_yield()) {
            g_schedtest_failed = true;
            cpu_halt_forever();
        }
    }
    /*
     * Remain cooperatively parked.
     *
     * The test destroys this thread while it is
     * suspended inside scheduler_yield().
     */
    for (;;) {
        if (!scheduler_yield()) {
            g_schedtest_failed = true;
            cpu_halt_forever();
        }
    }
}

static void command_schedtest(void) {
    terminal_writeln("SCHEDULER TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: "); terminal_write_u64(before.free_pages); terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue && scheduler_thread_count() == 1;

    terminal_write("  MAIN QUEUED: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread a;
    Thread b;

    AddressSpace *kernel_space = address_space_kernel();

    bool created_a = thread_create(&a, kernel_space);
    bool created_b = created_a && thread_create(&b, kernel_space);

    terminal_write("  CREATE A: ");
    terminal_writeln(created_a ? "PASS" : "FAILED");
    terminal_write("  CREATE B: ");
    terminal_writeln(created_b ? "PASS" : "FAILED");

    if (!created_a || !created_b) {
        if (created_a) (void)thread_destroy(&a);
        return;
    }

    bool prepared_a = thread_prepare_kernel(&a, schedtest_worker, (void *)&g_schedtest_a_count);
    bool prepared_b = thread_prepare_kernel(&b, schedtest_worker, (void *)&g_schedtest_b_count);

    terminal_write("  PREPARE A: ");
    terminal_writeln(prepared_a ? "PASS" : "FAILED");
    terminal_write("  PREPARE B: ");
    terminal_writeln(prepared_b ? "PASS" : "FAILED");

    if (!prepared_a || !prepared_b) {
        (void)thread_destroy(&a);
        (void)thread_destroy(&b);
        return;
    }

    bool added_a = scheduler_add(&a);
    bool added_b = added_a && scheduler_add(&b);

    terminal_write("  QUEUE A: ");
    terminal_writeln(added_a ? "PASS" : "FAILED");
    terminal_write("  QUEUE B: ");
    terminal_writeln(added_b ? "PASS" : "FAILED");

    if (!added_a || !added_b) {
        if (added_a) (void)scheduler_remove(&a);

        (void)thread_destroy(&a);
        (void)thread_destroy(&b);

        return;
    }

    bool queue_three = scheduler_thread_count() == 3;

    terminal_write("  QUEUE COUNT 3: ");
    terminal_writeln(queue_three ? "PASS" : "FAILED");

    g_schedtest_a_count = 0;
    g_schedtest_b_count = 0;
    g_schedtest_failed = false;

    /*
     * Each main-thread yield produces:
     *
     * main -> A -> B -> main
     */
    interrupts_disable();

    bool yielded = true;

    for (u32 round = 0; round < 3U; ++round) {
        if (!scheduler_yield()) {
            yielded = false;
            break;
        }
    }

    interrupts_enable();

    bool counts_ok = g_schedtest_a_count == 3 && g_schedtest_b_count == 3;
    bool current_restored = thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool children_ready = a.state == THREAD_STATE_READY && b.state == THREAD_STATE_READY;

    terminal_write("  YIELD SEQUENCE: ");
    terminal_writeln(yielded && !g_schedtest_failed ? "PASS" : "FAILED");
    terminal_write("  A COUNT: "); terminal_write_u64(g_schedtest_a_count); terminal_putchar('\n'); terminal_write("  B COUNT: "); terminal_write_u64(g_schedtest_b_count); terminal_putchar('\n'); terminal_write("  CURRENT RESTORED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  CHILDREN READY: ");
    terminal_writeln(children_ready ? "PASS" : "FAILED");

    bool removed_a = scheduler_remove(&a);
    bool removed_b = scheduler_remove(&b);
    bool queue_restored = scheduler_thread_count() == 1;

    terminal_write("  REMOVE A: ");
    terminal_writeln(removed_a ? "PASS" : "FAILED");
    terminal_write("  REMOVE B: ");
    terminal_writeln(removed_b ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    bool destroyed_a = thread_destroy(&a);
    bool destroyed_b = thread_destroy(&b);

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;

    terminal_write("  DESTROY A: ");
    terminal_writeln(destroyed_a ? "PASS" : "FAILED");
    terminal_write("  DESTROY B: ");
    terminal_writeln(destroyed_b ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: "); terminal_write_u64(after.free_pages); terminal_putchar('\n'); terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && created_a && created_b && prepared_a && prepared_b && added_a && added_b && queue_three && yielded && !g_schedtest_failed && counts_ok && current_restored && children_ready && removed_a && removed_b && queue_restored && destroyed_a && destroyed_b && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color()); terminal_write("SCHEDULER TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void exittest_worker(void *argument) {
    (void)argument;
    g_exittest_started = true;
    scheduler_exit_current();
}

static void command_exittest(void) {
    terminal_writeln("THREAD EXIT TEST:");

    PmmStats before = pmm_stats();

    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue && scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread child;
    bool created = thread_create(&child, address_space_kernel());

    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) return;

    bool prepared = thread_prepare_kernel(&child, exittest_worker, 0);

    terminal_write("  PREPARE: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&child);

        return;
    }

    bool queued = scheduler_add(&child);

    terminal_write("  QUEUE: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&child);

        return;
    }

    bool queue_two = scheduler_thread_count() == 2;

    terminal_write("  QUEUE COUNT 2: ");
    terminal_writeln(queue_two ? "PASS" : "FAILED");

    g_exittest_started = false;

    /*
     * main -> child
     *
     * child exits permanently and enters the
     * saved main context without saving itself.
     */
    interrupts_disable();

    bool yielded = scheduler_yield();

    interrupts_enable();

    bool worker_started = g_exittest_started;
    bool current_restored = yielded && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool child_dead = child.state == THREAD_STATE_DEAD && !child.on_run_queue && !child.context_ready;
    bool queue_restored = scheduler_thread_count() == 1;

    terminal_write("  CHILD RAN: ");
    terminal_writeln(worker_started ? "PASS" : "FAILED");
    terminal_write("  CURRENT RESTORED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  CHILD DEAD: ");
    terminal_writeln(child_dead ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    /* We are now executing on the main stack, so the dead child's stack can be freed. */
    bool reaped = thread_destroy(&child);

    terminal_write("  REAP DEAD THREAD: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();

    bool frames_restored = before.free_pages == after.free_pages;

    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && created && prepared && queued && queue_two && yielded && worker_started && current_restored && child_dead && queue_restored && reaped && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("THREAD EXIT TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_syscalltest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;

    AddressSpace space;

    if (!address_space_create(&space)) {
        terminal_writeln("USERFAULT: ADDRESS SPACE FAILED.");
        return;
    }

    Thread thread;

    if (!thread_create(&thread, &space)) {
        address_space_destroy(&space);
        terminal_writeln("USERFAULT: THREAD FAILED.");
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();

    if (code_frame == FRAME_INVALID || stack_frame == FRAME_INVALID) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        address_space_destroy(&space);

        terminal_writeln("USERFAULT: FRAME ALLOCATION FAILED.");
        return;
    }

    bool code_mapped = address_space_map_page(&space, user_code, code_frame, 0);
    bool stack_mapped = address_space_map_page(&space, user_stack, stack_frame, VM_WRITE);

    if (!code_mapped || !stack_mapped) {

        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(&space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(&space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        address_space_destroy(&space);

        terminal_writeln("USERFAULT: USER MAPPING FAILED.");

        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));

    if (!code) {
        terminal_writeln("USERFAULT: PHYSMAP FAILED.");

        return;
    }

    /* mov eax, SYSCALL_THREAD_ID int 0x80 ud2 */
    code[0] = 0xB8;
    code[1] = 0x00;
    code[2] = 0x00;
    code[3] = 0x00;
    code[4] = 0x00;
    code[5] = 0xCD;
    code[6] = 0x80;
    code[7] = 0x0F;
    code[8] = 0x0B;

    terminal_writeln("ENTERING RING3 SYSCALL TEST...");
    terminal_write("  USER RIP: "); terminal_write_hex(user_code); terminal_putchar('\n');
    terminal_write("  USER RSP: "); terminal_write_hex(user_stack_top); terminal_putchar('\n');
    terminal_write("  KERNEL RSP0: "); terminal_write_hex(thread.kernel_stack_top); terminal_putchar('\n');

    /* No interrupt may observe the transitional kernel context before IRETQ enters this thread. */
    interrupts_disable();

    if (!thread_activate(&thread)) {
        interrupts_enable();
        terminal_writeln("SYSCALLTEST: THREAD ACTIVATION FAILED.");
        return;
    }

    terminal_write("  ACTIVE THREAD: "); terminal_write_u64(thread_current()->id); terminal_putchar('\n');
    arch_enter_user(user_code, user_stack_top);
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

    BlockDevice *device = block_find("sda1");

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
        terminal_write_hex(row * 16U); terminal_write(": ");

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

    terminal_write("GPT PARTITIONS: "); terminal_write_u64(gpt->partition_count); terminal_putchar('\n');

    for (u32 i = 0; i < gpt->partition_count; ++i) {

        const GptPartition *part = &gpt->partitions[i];
        if (!part->valid) continue;

        terminal_write("  sda"); terminal_write_u64(part->index);
        if (part->name[0]) { terminal_write("  "); terminal_write(part->name); }

        terminal_putchar('\n'); terminal_write("    FIRST LBA: "); terminal_write_u64(part->first_lba); terminal_putchar('\n'); terminal_write("    LAST LBA: "); terminal_write_u64(part->last_lba); terminal_putchar('\n');
        u64 sectors = part->last_lba - part->first_lba + 1ULL;
        terminal_write("    SECTORS: "); terminal_write_u64(sectors); terminal_putchar('\n');

        if (gpt->device) {
            u64 bytes = sectors * gpt->device->block_size;

            terminal_write("    SIZE: "); terminal_write_u64(bytes / (1024ULL * 1024ULL));
            terminal_writeln(" MiB");
        }
    }
}

static void command_userfault(void) {
    /*
     * Destructive Ring3 probe.
     *
     * Success ends in a Ring3 #UD exception and
     * therefore does not return to the shell.
     */
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;

    AddressSpace space;

    if (!address_space_create(&space)) {
        terminal_writeln("USERFAULT: ADDRESS SPACE FAILED.");

        return;
    }

    Thread thread;

    if (!thread_create(&thread, &space)) {
        address_space_destroy(&space);
        terminal_writeln("USERFAULT: THREAD FAILED.");
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();

    if (code_frame == FRAME_INVALID || stack_frame == FRAME_INVALID) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        address_space_destroy(&space);

        terminal_writeln("USERFAULT: FRAME ALLOCATION FAILED.");
        return;
    }

    bool code_mapped = address_space_map_page(&space, user_code, code_frame, 0);
    bool stack_mapped = address_space_map_page(&space, user_stack, stack_frame, VM_WRITE);

    if (!code_mapped || !stack_mapped) {

        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(&space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(&space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        address_space_destroy(&space);

        terminal_writeln("USERFAULT: USER MAPPING FAILED.");

        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));

    if (!code) {
        terminal_writeln("USERFAULT: PHYSMAP FAILED.");

        return;
    }

    /*
     * UD2
     *
     * If Ring3 entry succeeds this raises #UD,
     * forcing a CPL3 -> CPL0 transition.
     */
    code[0] = 0x0F;
    code[1] = 0x0B;

    terminal_writeln("ENTERING RING3...");
    terminal_write("  USER RIP: "); terminal_write_hex(user_code); terminal_putchar('\n');
    terminal_write("  USER RSP: "); terminal_write_hex(user_stack_top); terminal_putchar('\n');
    terminal_write("  KERNEL RSP0: "); terminal_write_hex(thread.kernel_stack_top); terminal_putchar('\n');

    /* No interrupt may observe the transitional kernel context before IRETQ enters this thread. */
    interrupts_disable();

    if (!thread_activate(&thread)) {
        interrupts_enable();
        terminal_writeln("USERFAULT: THREAD ACTIVATION FAILED.");
        return;
    }

    terminal_write("  ACTIVE THREAD: "); terminal_write_u64(thread_current()->id); terminal_putchar('\n');

    arch_enter_user(user_code, user_stack_top);
}

static void command_fat32(void) {
    const Fat32Info *info = fat32_get();

    if (!info || !info->valid) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("FAT32 NOT INITIALIZED.");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_writeln("FAT32:");
    terminal_write("  DEVICE: ");
    terminal_writeln(info->device->name);
    terminal_write("  BYTES/SECTOR: "); terminal_write_u64(info->bytes_per_sector); terminal_putchar('\n');
    terminal_write("  SECTORS/CLUSTER: "); terminal_write_u64(info->sectors_per_cluster); terminal_putchar('\n');
    terminal_write("  RESERVED SECTORS: "); terminal_write_u64(info->reserved_sectors); terminal_putchar('\n');
    terminal_write("  FAT COUNT: "); terminal_write_u64(info->fat_count); terminal_putchar('\n');
    terminal_write("  SECTORS/FAT: "); terminal_write_u64(info->sectors_per_fat); terminal_putchar('\n');
    terminal_write("  TOTAL SECTORS: "); terminal_write_u64(info->total_sectors); terminal_putchar('\n');
    terminal_write("  CLUSTERS: "); terminal_write_u64(info->cluster_count); terminal_putchar('\n');
    terminal_write("  ROOT CLUSTER: "); terminal_write_u64(info->root_cluster); terminal_putchar('\n');
    terminal_write("  FIRST FAT SECTOR: "); terminal_write_u64(info->first_fat_sector); terminal_putchar('\n');
    terminal_write("  FIRST DATA SECTOR: "); terminal_write_u64(info->first_data_sector); terminal_putchar('\n');
}

static void command_fatls(void) {
    const Fat32Info *info = fat32_get();

    if (!info || !info->valid) {
        terminal_writeln("FAT32 NOT INITIALIZED.");
        return;
    }

    static Fat32DirectoryEntry
        entries[64];

    u32 count = 0;

    if (!fat32_read_root(entries, ARRAY_COUNT(entries), &count)) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("FAT32 ROOT DIRECTORY READ FAILED.");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("FAT32 ROOT: "); terminal_write_u64(count);
    terminal_writeln(" ENTRIES");

    for (u32 i = 0; i < count; ++i) {

        Fat32DirectoryEntry *entry = &entries[i];

        terminal_write("  "); terminal_write(entry->name);
        if (entry->attributes & FAT32_ATTR_DIRECTORY) terminal_putchar('/');
        terminal_putchar('\n'); terminal_write("    CLUSTER: "); terminal_write_u64(entry->first_cluster);

        if (!(entry->attributes & FAT32_ATTR_DIRECTORY)) { terminal_write("  SIZE: "); terminal_write_u64(entry->size); terminal_write(" BYTES"); }

        terminal_putchar('\n');
    }
}

static void command_fatread(const char *arguments) {

    char name[VFS_NAME_MAX + 1];
    char offset_text[32];
    char count_text[32];

    const char *cursor = arguments;

    if (!next_argument(&cursor, name, sizeof(name))) {

        terminal_writeln("usage: fatread FILE [OFFSET] [COUNT]");
        return;
    }

    u64 offset = 0;
    u64 requested = 1024;

    if (next_argument(&cursor, offset_text, sizeof(offset_text))) {
        if (!parse_u64(offset_text, &offset)) {

            terminal_writeln("fatread: invalid offset");
            return;
        }
    }

    if (next_argument(&cursor, count_text, sizeof(count_text))) {
        if (!parse_u64(count_text, &requested)) {

            terminal_writeln("fatread: invalid byte count");
            return;
        }
    }

    Fat32DirectoryEntry entry;

    if (!fat32_find_root(name, &entry)) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("FAT32 FILE NOT FOUND.");
        terminal_set_color(terminal_default_color());

        return;
    }

    if (entry.attributes & FAT32_ATTR_DIRECTORY) {

        terminal_writeln("fatread: entry is a directory");
        return;
    }

    /*
     * 1024 bytes deliberately crosses two
     * clusters on the current image because
     * sectors/cluster = 1 and sector = 512.
     *
     * This therefore tests FAT-chain following,
     * not just one-cluster reads.
     */
    static u8 buffer[1024];

    k_memset(buffer, 0, sizeof(buffer));

    if (requested > sizeof(buffer)) requested = sizeof(buffer);

    u64 amount = requested;

    u64 read = 0;

    if (!fat32_read_file(&entry, offset, buffer, amount, &read)) {

        terminal_set_color(terminal_error_color());
        terminal_writeln("FAT32 FILE READ FAILED.");
        terminal_set_color(terminal_default_color());

        return;
    }

    terminal_write("FILE: ");
    terminal_writeln(entry.name);
    terminal_write("SIZE: "); terminal_write_u64(entry.size);
    terminal_writeln(" BYTES");
    terminal_write("OFFSET: "); terminal_write_u64(offset); terminal_putchar('\n');
    terminal_write("FIRST CLUSTER: "); terminal_write_u64(entry.first_cluster); terminal_putchar('\n');
    terminal_write("TEST READ: "); terminal_write_u64(read);
    terminal_writeln(" BYTES");
    terminal_writeln("FIRST 64 BYTES OF READ:");

    u64 dump = read < 64ULL ? read : 64ULL;

    for (u64 offset = 0; offset < dump; offset += 16ULL) {

        terminal_write_hex(offset); terminal_write(": ");

        u64 row = dump - offset;

        if (row > 16ULL) row = 16ULL;
        for (u64 i = 0; i < row; ++i) { terminal_hex_byte(buffer[offset + i]); terminal_putchar(' '); }

        terminal_putchar('\n');
    }

    /* ELF64 signature: 7F 'E' 'L' 'F' */
    if (read >= 4 && buffer[0] == 0x7FU && buffer[1] == 'E' && buffer[2] == 'L' && buffer[3] == 'F') {
        terminal_writeln("ELF SIGNATURE: VALID");
    }

    /* USTAR magic starts at byte 257 of the first TAR header. */
    if (read >= 262 && buffer[257] == 'u' && buffer[258] == 's' && buffer[259] == 't' && buffer[260] == 'a' && buffer[261] == 'r') {
        terminal_writeln("USTAR SIGNATURE: VALID");
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
    else if (k_strieq(command, "frametest")) command_frametest();
    else if (k_strieq(command, "vmmtest")) command_vmmtest();
    else if (k_strieq(command, "astest")) command_astest();
    else if (k_strieq(command, "threadtest")) command_threadtest();
    else if (k_strieq(command, "switchtest")) command_switchtest();
    else if (k_strieq(command, "schedtest")) command_schedtest();
    else if (k_strieq(command, "exittest")) command_exittest();
    else if (k_strieq(command, "syscalltest")) command_syscalltest();
    else if (k_strieq(command, "cpu")) command_cpu();
    else if (k_strieq(command, "interrupts")) command_interrupts();
    else if (k_strieq(command, "acpi")) command_acpi();
    else if (k_strieq(command, "pci")) command_pci();
    else if (k_strieq(command, "ahci")) command_ahci();
    else if (k_strieq(command, "disks")) command_disks();
    else if (k_strieq(command, "sector")) command_sector(args);
    else if (k_strieq(command, "partitions")) command_partitions();
    else if (k_strieq(command, "fault")) __asm__ volatile ("ud2");
    else if (k_strieq(command, "userfault")) command_userfault();
    else if (k_strieq(command, "reboot")) command_reboot();
    else if (k_strieq(command, "pwd")) command_pwd();
    else if (k_strieq(command, "fat32")) command_fat32();
    else if (k_strieq(command, "fatls")) command_fatls();
    else if (k_strieq(command, "fatread"))command_fatread(args);
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
    g_boot = boot;
    g_length = 0;
    g_cwd = vfs_root();
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
