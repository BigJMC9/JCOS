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
#include "timer.h"
#include "capability.h"
#include "process.h"
#include "endpoint.h"
#include "ipc.h"
#include "syscall.h"
#include "user_elf.h"
#include "supervisor.h"
#include "power.h"
#include "user_runtime_test.h"
#include "user_runtime_block_test.h"
#include "user_ipc_cancel_test.h"
#include "force_thread_test.h"
#include "process_terminate_test.h"
#include "lifetime_stress_test.h"
#include "stack_reclaim_test.h"
#include "elf_reclaim_test.h"
#include "vmm_reclaim_test.h"
#include "constructor_test.h"
#include "user_test_fixture.h"
#include "user_process_cleanup_test.h"
#include "capability_test.h"
#include "ipc_wait_order_test.h"
#include "peer_death_test.h"
#include "ipc_timeout_order_test.h"
#include "lifetime_ipc_acceptance_test.h"
#include "test_registry.h"
#include "editor.h"
#include "input.h"
#include "userspace_shell.h"


static VfsNode *g_cwd;

static const BootInfo *g_boot;
static ShellEditor g_editor;

static volatile u64 g_schedtest_a_count;
static volatile u64 g_schedtest_b_count;
static volatile bool g_schedtest_failed;

static volatile bool g_exittest_started;

static volatile bool g_blocktest_started;
static volatile bool g_blocktest_resumed;

static volatile u64 g_preempttest_a_count;
static volatile u64 g_preempttest_b_count;

static volatile u64 g_reschedtest_a_count;
static volatile u64 g_reschedtest_b_count;
static volatile bool g_reschedtest_failed;

static volatile bool g_ipcblock_started;
static volatile bool g_ipcblock_received;
static volatile bool g_ipcblock_failed;

static volatile u32 g_ipcblock_word_count;
static volatile u64 g_ipcblock_words[IPC_MESSAGE_MAX_WORDS];

static CapabilityHandle g_ipcblock_receive_handle;

static volatile bool g_ipcsend_started;
static volatile bool g_ipcsend_completed;
static volatile bool g_ipcsend_failed;

static CapabilityHandle g_ipcsend_handle;

static void shell_print_help(const char *topic);
static void command_test(const char *args);

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
    terminal_putchar(digits[(value >> 4) & 0x0F]);
    terminal_putchar(digits[value & 0x0F]);
}

static void shell_emit_u32_le(u8 **cursor, u32 value) {
    if (!cursor || !*cursor) return;
    for (u32 i = 0; i < 4U; ++i) *(*cursor)++ = (u8)(value >> (i * 8U));
}

static void shell_emit_u64_le(u8 **cursor, u64 value) {
    if (!cursor || !*cursor) return;
    for (u32 i = 0; i < 8U; ++i) *(*cursor)++ = (u8)(value >> (i * 8U));
}

static void prompt(void) {
    char path[VFS_PATH_MAX];
    terminal_set_color(terminal_accent_color());
    terminal_write("JA:");
    if (vfs_get_path(g_cwd, path, sizeof(path))) terminal_write(path);
    else terminal_write("?");
    terminal_write("> ");
    terminal_set_color(terminal_default_color());
}

static void print_mib(u64 pages) {
    terminal_write_u64((pages * 4096ULL) / (1024ULL * 1024ULL));
    terminal_write(" MiB");
}

static void command_help(const char *args) {
    shell_print_help(args);
}

static void command_about(void) {
    terminal_writeln("JA OS V5 IS A FREESTANDING X86_64 UEFI KERNEL CREATED BY JACOB CROSBIE.");
    terminal_writeln("THE EFI LOADER LOADS KERNEL.ELF, CAPTURES GOP + ACPI + MEMORY MAP,");
    terminal_writeln("CALLS EXITBOOTSERVICES(), THEN AN ASSEMBLY SHIM ENTERS THIS C KERNEL.");
    terminal_writeln("INPUT: PS/2 SET-1 KEYBOARD WITH IRQ + POLLING, OR COM1 SERIAL.");
}

static void command_memory(void) {
    PmmStats stats = pmm_stats();
    terminal_write("UEFI DESCRIPTORS: ");
    terminal_write_u64(g_boot->memory_map_descriptor_size ? g_boot->memory_map_size / g_boot->memory_map_descriptor_size : 0);
    terminal_putchar('\n');
    terminal_write("ALLOCATOR RANGES: ");
    terminal_write_u64(stats.range_count);
    terminal_putchar('\n');
    terminal_write("MANAGED MEMORY: ");
    print_mib(stats.total_pages);
    terminal_putchar('\n');
    terminal_write("TOTAL FRAMES: ");
    terminal_write_u64(stats.total_pages);
    terminal_putchar('\n');
    terminal_write("FREE FRAME MEMORY: ");
    print_mib(stats.free_pages);
    terminal_putchar('\n');
    terminal_write("FREE FRAMES: ");
    terminal_write_u64(stats.free_pages);
    terminal_putchar('\n');
    terminal_write("USED/RESERVED FRAMES: ");
    terminal_write_u64(stats.total_pages - stats.free_pages);
    terminal_putchar('\n');
    terminal_write("PMM BITMAP: ");
    terminal_write_hex(stats.bitmap_physical);
    terminal_write("  PAGES: ");
    terminal_write_u64(stats.bitmap_pages);
    terminal_putchar('\n');
    terminal_write("KERNEL BASE: ");
    terminal_write_hex(g_boot->kernel_base);
    terminal_putchar('\n');
    terminal_write("KERNEL SIZE: ");
    terminal_write_u64(g_boot->kernel_size);
    terminal_writeln(" bytes");

    if (stats.discarded_ranges) {
        terminal_write("DISCARDED EXTRA RANGES: ");
        terminal_write_u64(stats.discarded_ranges);
        terminal_putchar('\n');
    }
}

static void command_alloc(void) {
    frame_t frame = frame_alloc();

    if (frame == FRAME_INVALID) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("OUT OF PHYSICAL FRAMES.");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("ALLOCATED FRAME: ");
    terminal_write_u64(frame);
    terminal_write("  PHYSICAL: ");
    terminal_write_hex(frame_to_phys(frame));
    terminal_putchar('\n');
    terminal_writeln("FRAME REMAINS ALLOCATED.");
}

static void command_frametest(void) {
    terminal_writeln("PMM FRAME TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

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

    terminal_write("  A: FRAME ");
    terminal_write_u64(a);
    terminal_write("  PHYS=");
    terminal_write_hex(frame_to_phys(a));
    terminal_putchar('\n');
    terminal_write("  B: FRAME ");
    terminal_write_u64(b);
    terminal_write("  PHYS=");
    terminal_write_hex(frame_to_phys(b));
    terminal_putchar('\n');
    terminal_write("  C: FRAME ");
    terminal_write_u64(c);
    terminal_write("  PHYS=");
    terminal_write_hex(frame_to_phys(c));
    terminal_putchar('\n');

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
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');

    bool count_restored = before.free_pages == after.free_pages;
    bool pass = distinct && reused && freed_a && freed_c && freed_d && double_free_rejected && count_restored;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PMM FRAME TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_vmmtest(void) {
    terminal_writeln("VMM PAGE TABLE TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    VmPageMap map;

    if (!vmm_page_map_create(&map)) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("  CREATE PAGE MAP: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("  ROOT FRAME: ");
    terminal_write_u64(map.root_frame);
    terminal_write("  PHYS=");
    terminal_write_hex(frame_to_phys(map.root_frame));
    terminal_putchar('\n');

    frame_t data = frame_alloc();

    if (data == FRAME_INVALID) {
        vmm_page_map_destroy(&map);
        terminal_set_color(terminal_error_color());
        terminal_writeln("  DATA FRAME: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("  DATA FRAME: ");
    terminal_write_u64(data);
    terminal_write("  PHYS=");
    terminal_write_hex(frame_to_phys(data));
    terminal_putchar('\n');

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
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');

    bool count_restored = before.free_pages == after.free_pages;
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = mapped && queried && frame_match && flags_match && duplicate_rejected && unmapped &&
        old_frame_match && query_after_unmap_rejected && data_freed && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("VMM PAGE TABLE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_astest(void) {
    terminal_writeln("ADDRESS SPACE TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    AddressSpace space;
    bool created = address_space_create(&space);
    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) return;

    terminal_write("  ID: ");
    terminal_write_u64(space.id);
    terminal_write("  CR3: ");
    terminal_write_hex(address_space_cr3(&space));
    terminal_putchar('\n');

    /* The low kernel mapping must be shared but supervisor-only. */
    u64 kernel_page = g_boot->kernel_base & ~(VM_PAGE_SIZE - 1ULL);
    frame_t kernel_frame = FRAME_INVALID;
    vm_flags_t kernel_flags = 0;
    bool kernel_shared = address_space_query_page(&space, kernel_page, &kernel_frame, &kernel_flags) &&
        kernel_frame == phys_to_frame(kernel_page) && !(kernel_flags & VM_USER);

    terminal_write("  KERNEL MAPPING SHARED: ");
    terminal_writeln(kernel_shared ? "PASS" : "FAILED");

    /* The physical direct map must also be shared and supervisor-only. */
    u64 bitmap_direct = 0;
    bool physmap_shared = false;

    if (physmap_virtual_address(before.bitmap_physical, &bitmap_direct)) {
        frame_t mapped = FRAME_INVALID;
        vm_flags_t flags = 0;

        physmap_shared = address_space_query_page(&space, bitmap_direct, &mapped, &flags) &&
            mapped == phys_to_frame(before.bitmap_physical) && !(flags & VM_USER);
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
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = created && kernel_shared && physmap_shared && data_ok && mapped && queried && flags_match &&
        low_rejected && high_rejected && unmapped && old_match && data_freed && count_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("ADDRESS SPACE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_supervisortest(void) {
    terminal_writeln("PERSISTENT USER SUPERVISOR TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    bool running_before = supervisor_running();
    terminal_write("  RUNNING: ");
    terminal_writeln(running_before ? "PASS" : "FAILED");

    if (!running_before) return;

    u64 process_id_before = supervisor_process_id();
    u64 thread_id_before = supervisor_thread_id();
    bool ids_valid = process_id_before != 0ULL && thread_id_before != 0ULL;
    terminal_write("  PROCESS ID: ");
    terminal_write_u64(process_id_before);
    terminal_putchar('\n');
    terminal_write("  THREAD ID: ");
    terminal_write_u64(thread_id_before);
    terminal_putchar('\n');
    terminal_write("  IDS VALID: ");
    terminal_writeln(ids_valid ? "PASS" : "FAILED");

    u64 reply1 = 0;
    bool ping1 = supervisor_ping(0x1122334455667788ULL, &reply1);
    bool ping1_ok = ping1 && reply1 == 0x1122334455667788ULL;
    terminal_write("  PING 1: ");
    terminal_writeln(ping1_ok ? "PASS" : "FAILED");

    u64 reply2 = 0;
    bool ping2 = supervisor_ping(0xAABBCCDDEEFF0011ULL, &reply2);
    bool ping2_ok = ping2 && reply2 == 0xAABBCCDDEEFF0011ULL;
    terminal_write("  PING 2: ");
    terminal_writeln(ping2_ok ? "PASS" : "FAILED");

    bool still_running = ping2_ok && supervisor_running();
    terminal_write("  STILL RUNNING: ");
    terminal_writeln(still_running ? "PASS" : "FAILED");

    bool same_process = supervisor_process_id() == process_id_before;
    bool same_thread = supervisor_thread_id() == thread_id_before;
    terminal_write("  PROCESS PERSISTED: ");
    terminal_writeln(same_process ? "PASS" : "FAILED");
    terminal_write("  THREAD PERSISTED: ");
    terminal_writeln(same_thread ? "PASS" : "FAILED");

    bool queue_restored = scheduler_thread_count() == 1ULL;
    terminal_write("  RUN QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_stable = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT STABLE: ");
    terminal_writeln(frames_stable ? "PASS" : "FAILED");

    bool pass = running_before && ids_valid && ping1_ok && ping2_ok && still_running && same_process &&
        same_thread && queue_restored && frames_stable;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PERSISTENT USER SUPERVISOR TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_captest(void) {
    capability_table_test_run();
}

static void command_ipctest(void) {
    terminal_writeln("IPC TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS CREATE: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    CapabilityTable *caps = process_capabilities(&process);
    bool caps_ok = caps && capability_table_count(caps) == 0U;
    terminal_write("  CAP TABLE: ");
    terminal_writeln(caps_ok ? "PASS" : "FAILED");

    Endpoint endpoint;
    bool endpoint_created = caps_ok && endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) {
        (void)process_destroy(&process);
        return;
    }

    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    bool send_cap = capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    bool receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    terminal_write("  SEND CAP: ");
    terminal_writeln(send_cap ? "PASS" : "FAILED");
    terminal_write("  RECEIVE CAP: ");
    terminal_writeln(receive_cap ? "PASS" : "FAILED");

    IpcMessage received;
    bool empty_receive_rejected = receive_cap && !ipc_try_receive(&process, receive_handle, &received);
    terminal_write("  EMPTY RECEIVE REJECTED: ");
    terminal_writeln(empty_receive_rejected ? "PASS" : "FAILED");

    /* SEND-only capability must not permit RECEIVE. */
    bool send_cannot_receive = send_cap && !ipc_try_receive(&process, send_handle, &received);
    terminal_write("  SEND CAP CANNOT RECEIVE: ");
    terminal_writeln(send_cannot_receive ? "PASS" : "FAILED");

    IpcMessage message;

    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) message.words[i] = 0;

    message.word_count = 3U;
    message.words[0] = 0x1111222233334444ULL;
    message.words[1] = 0x5555666677778888ULL;
    message.words[2] = 0xAABBCCDDEEFF0011ULL;

    bool sent = receive_cap && ipc_try_send(&process, send_handle, &message);
    terminal_write("  SEND: ");
    terminal_writeln(sent ? "PASS" : "FAILED");

    bool ready_after_send = sent && endpoint_message_ready(&endpoint);
    terminal_write("  MESSAGE PENDING: ");
    terminal_writeln(ready_after_send ? "PASS" : "FAILED");

    /*
     * One-message mailbox:
     *
     * another SEND while occupied must fail.
     */
    bool full_send_rejected = sent && !ipc_try_send(&process, send_handle, &message);
    terminal_write("  FULL SEND REJECTED: ");
    terminal_writeln(full_send_rejected ? "PASS" : "FAILED");

    /*
     * RECEIVE-only capability must not permit
     * SEND even while testing a full endpoint.
     *
     * We'll test it again while empty below so
     * failure cannot be attributed only to FULL.
     */
    bool received_ok = ipc_try_receive(&process, receive_handle, &received);
    terminal_write("  RECEIVE: ");
    terminal_writeln(received_ok ? "PASS" : "FAILED");

    bool payload_ok = received_ok && received.word_count == 3U &&
        received.words[0] == 0x1111222233334444ULL && received.words[1] == 0x5555666677778888ULL &&
        received.words[2] == 0xAABBCCDDEEFF0011ULL;

    terminal_write("  PAYLOAD MATCH: ");
    terminal_writeln(payload_ok ? "PASS" : "FAILED");

    bool empty_after_receive = received_ok && !endpoint_message_ready(&endpoint);
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(empty_after_receive ? "PASS" : "FAILED");

    bool receive_cannot_send = empty_after_receive && !ipc_try_send(&process, receive_handle, &message);
    terminal_write("  RECEIVE CAP CANNOT SEND: ");
    terminal_writeln(receive_cannot_send ? "PASS" : "FAILED");

    /* Prove the mailbox can be reused after RECEIVE consumes the pending message. */
    message.word_count = 1U;
    message.words[0] = 0x4A434F5349504301ULL;

    bool resend = receive_cannot_send && ipc_try_send(&process, send_handle, &message);
    terminal_write("  RESEND AFTER RECEIVE: ");
    terminal_writeln(resend ? "PASS" : "FAILED");

    IpcMessage second;
    bool second_receive = resend && ipc_try_receive(&process, receive_handle, &second);
    bool second_payload = second_receive && second.word_count == 1U && second.words[0] == 0x4A434F5349504301ULL;
    terminal_write("  SECOND RECEIVE: ");
    terminal_writeln(second_receive ? "PASS" : "FAILED");
    terminal_write("  SECOND PAYLOAD: ");
    terminal_writeln(second_payload ? "PASS" : "FAILED");

    bool second_empty_receive = second_receive && !ipc_try_receive(&process, receive_handle, &second);
    terminal_write("  SECOND EMPTY RECEIVE REJECTED: ");
    terminal_writeln(second_empty_receive ? "PASS" : "FAILED");

    bool send_revoked = capability_revoke(caps, send_handle);
    bool receive_revoked = capability_revoke(caps, receive_handle);
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(send_revoked && receive_revoked ? "PASS" : "FAILED");

    bool caps_empty = capability_table_count(caps) == 0U;
    terminal_write("  CAP TABLE EMPTY: ");
    terminal_writeln(caps_empty ? "PASS" : "FAILED");

    bool endpoint_destroyed = caps_empty && endpoint_destroy(&endpoint);
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");

    bool process_destroyed = endpoint_destroyed && process_destroy(&process);
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = process_created && caps_ok && endpoint_created && send_cap && receive_cap &&
        empty_receive_rejected && send_cannot_receive && sent && ready_after_send && full_send_rejected &&
        received_ok && payload_ok && empty_after_receive && receive_cannot_send && resend &&
        second_receive && second_payload && second_empty_receive && send_revoked && receive_revoked &&
        caps_empty && endpoint_destroyed && process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("IPC TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void ipcblock_receiver_worker(void *argument) {
    (void)argument;

    g_ipcblock_started = true;

    IpcMessage message;
    bool received = ipc_receive_blocking(process_kernel(), g_ipcblock_receive_handle, &message);

    if (!received) {
        g_ipcblock_failed = true;

        interrupts_disable();
        scheduler_exit_current();
    }

    g_ipcblock_word_count = message.word_count;

    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        g_ipcblock_words[i] = message.words[i];
    }

    g_ipcblock_received = true;

    interrupts_disable();
    scheduler_exit_current();
}

static void command_ipcblocktest(void) {
    terminal_writeln("BLOCKING IPC TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();

    bool main_ok = main_thread && kernel_process && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled();

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    CapabilityTable *caps = process_capabilities(kernel_process);
    bool caps_ready = caps != 0;
    u32 caps_before =
    caps ? capability_table_count(caps): 0U;

    terminal_write("  KERNEL CAP TABLE: ");
    terminal_writeln(caps_ready ? "PASS" : "FAILED");
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(caps_before);
    terminal_putchar('\n');

    if (!caps_ready) return;

    Endpoint endpoint;
    bool endpoint_created = endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) return;

    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    bool send_cap = capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    bool receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    terminal_write("  SEND CAP: ");
    terminal_writeln(send_cap ? "PASS" : "FAILED");
    terminal_write("  RECEIVE CAP: ");
    terminal_writeln(receive_cap ? "PASS" : "FAILED");

    if (!receive_cap) {
        if (send_cap) (void)capability_revoke(caps, send_handle);

        (void)endpoint_destroy(&endpoint);
        return;
    }

    Thread receiver;
    bool receiver_created = thread_create(&receiver, kernel_process);
    terminal_write("  RECEIVER CREATE: ");
    terminal_writeln(receiver_created ? "PASS" : "FAILED");

    if (!receiver_created) {
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        return;
    }

    g_ipcblock_started = false;
    g_ipcblock_received = false;
    g_ipcblock_failed = false;
    g_ipcblock_word_count = 0;

    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        g_ipcblock_words[i] = 0;
    }

    g_ipcblock_receive_handle = receive_handle;

    bool prepared = thread_prepare_kernel(&receiver, ipcblock_receiver_worker, 0);
    terminal_write("  RECEIVER PREPARE: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&receiver);
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        return;
    }

    bool queued = scheduler_add(&receiver);
    terminal_write("  RECEIVER QUEUE: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&receiver);
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        return;
    }

    /*
     * main -> receiver
     *
     * Endpoint is empty, so receiver installs
     * itself as waiting_receiver and BLOCKs.
     * Scheduler then restores main.
     */
    interrupts_disable();

    bool first_yield = scheduler_yield();

    interrupts_enable();

    bool receiver_started = g_ipcblock_started;
    bool receiver_blocked = first_yield && receiver_started && !g_ipcblock_failed &&
        receiver.state == THREAD_STATE_BLOCKED && !receiver.on_run_queue &&
        receiver.interrupt_context_ready && receiver.interrupt_rsp;

    bool waiter_installed = receiver_blocked && endpoint_receiver_waiting(&endpoint);
    bool queue_one = scheduler_thread_count() == 1;
    terminal_write("  FIRST YIELD: ");
    terminal_writeln(first_yield ? "PASS" : "FAILED");
    terminal_write("  RECEIVER STARTED: ");
    terminal_writeln(receiver_started ? "PASS" : "FAILED");
    terminal_write("  RECEIVER BLOCKED: ");
    terminal_writeln(receiver_blocked ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT WAITER: ");
    terminal_writeln(waiter_installed ? "PASS" : "FAILED");
    terminal_write("  RUN QUEUE COUNT 1: ");
    terminal_writeln(queue_one ? "PASS" : "FAILED");

    if (!receiver_blocked || !waiter_installed || !queue_one) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("BLOCKING IPC TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    message.word_count = 3U;
    message.words[0] = 0x1122334455667788ULL;
    message.words[1] = 0x8877665544332211ULL;
    message.words[2] = 0x4A434F5349504302ULL;

    /* SEND stores the message and wakes the blocked receiver. */
    bool sent = ipc_try_send(kernel_process, send_handle, &message);
    bool message_pending = sent && endpoint_message_ready(&endpoint);

    bool receiver_woken = sent && receiver.state == THREAD_STATE_READY && receiver.on_run_queue &&
        receiver.interrupt_context_ready && receiver.interrupt_rsp && scheduler_thread_count() == 2;

    terminal_write("  SEND: ");
    terminal_writeln(sent ? "PASS" : "FAILED");
    terminal_write("  MESSAGE PENDING: ");
    terminal_writeln(message_pending ? "PASS" : "FAILED");
    terminal_write("  RECEIVER WOKEN: ");
    terminal_writeln(receiver_woken ? "PASS" : "FAILED");

    if (!receiver_woken) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("BLOCKING IPC TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * main -> resumed receiver
     *
     * ipc_receive_blocking() consumes the
     * message, clears waiting_receiver and
     * returns to the worker.
     *
     * Worker records the payload and EXITs,
     * restoring main.
     */
    interrupts_disable();

    bool second_yield = scheduler_yield();

    interrupts_enable();

    bool receiver_received = second_yield && g_ipcblock_received && !g_ipcblock_failed;

    bool payload_ok = receiver_received && g_ipcblock_word_count == 3U &&
        g_ipcblock_words[0] == 0x1122334455667788ULL && g_ipcblock_words[1] == 0x8877665544332211ULL &&
        g_ipcblock_words[2] == 0x4A434F5349504302ULL;

    bool receiver_dead = receiver.state == THREAD_STATE_DEAD && !receiver.on_run_queue &&
        !receiver.interrupt_context_ready && !receiver.interrupt_rsp;

    bool endpoint_empty = !endpoint_message_ready(&endpoint);
    bool waiter_cleared = !endpoint_receiver_waiting(&endpoint);
    bool queue_restored = scheduler_thread_count() == 1 && thread_current() == main_thread;
    terminal_write("  SECOND YIELD: ");
    terminal_writeln(second_yield ? "PASS" : "FAILED");
    terminal_write("  RECEIVER RECEIVED: ");
    terminal_writeln(receiver_received ? "PASS" : "FAILED");
    terminal_write("  PAYLOAD MATCH: ");
    terminal_writeln(payload_ok ? "PASS" : "FAILED");
    terminal_write("  RECEIVER DEAD: ");
    terminal_writeln(receiver_dead ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(endpoint_empty ? "PASS" : "FAILED");
    terminal_write("  WAITER CLEARED: ");
    terminal_writeln(waiter_cleared ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    bool reaped = receiver_dead && thread_destroy(&receiver);
    bool thread_count_restored = process_thread_count(kernel_process) == 1ULL;
    terminal_write("  RECEIVER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");
    terminal_write("  KERNEL THREAD COUNT RESTORED: ");
    terminal_writeln(thread_count_restored ? "PASS" : "FAILED");

    bool send_revoked = capability_revoke(caps, send_handle);
    bool receive_revoked = capability_revoke(caps, receive_handle);
    bool caps_restored = send_revoked && receive_revoked && capability_table_count(caps) == caps_before;
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(caps_restored ? "PASS" : "FAILED");

    bool endpoint_destroyed = caps_restored && waiter_cleared && endpoint_empty && endpoint_destroy(&endpoint);
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && caps_ready && endpoint_created && send_cap && receive_cap && receiver_created &&
        prepared && queued && first_yield && receiver_started && receiver_blocked && waiter_installed &&
        queue_one && sent && message_pending && receiver_woken && second_yield && receiver_received &&
        payload_ok && receiver_dead && endpoint_empty && waiter_cleared && queue_restored && reaped &&
        thread_count_restored && caps_restored && endpoint_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("BLOCKING IPC TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void ipcsend_sender_worker(void *argument) {
    (void)argument;

    g_ipcsend_started = true;
    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    message.word_count = 2U;
    message.words[0] = 0xAABBCCDDEEFF0011ULL;
    message.words[1] = 0x4A434F5349504303ULL;

    bool sent = ipc_send_blocking(process_kernel(), g_ipcsend_handle, &message);

    if (!sent) {
        g_ipcsend_failed = true;

        interrupts_disable();
        scheduler_exit_current();
    }

    g_ipcsend_completed = true;

    interrupts_disable();
    scheduler_exit_current();
}

static void command_ipcsendblocktest(void) {
    terminal_writeln("BLOCKING IPC SEND TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();

    bool main_ok = main_thread && kernel_process && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled();

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    CapabilityTable *caps = process_capabilities(kernel_process);
    bool caps_ready = caps != 0;
    u32 caps_before = caps ? capability_table_count(caps) : 0U;
    terminal_write("  KERNEL CAP TABLE: ");
    terminal_writeln(caps_ready ? "PASS" : "FAILED");
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(caps_before);
    terminal_putchar('\n');

    if (!caps_ready) return;

    Endpoint endpoint;
    bool endpoint_created = endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) return;

    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    bool send_cap = capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    bool receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    terminal_write("  SEND CAP: ");
    terminal_writeln(send_cap ? "PASS" : "FAILED");
    terminal_write("  RECEIVE CAP: ");
    terminal_writeln(receive_cap ? "PASS" : "FAILED");

    if (!receive_cap) {
        if (send_cap) (void)capability_revoke(caps, send_handle);

        (void)endpoint_destroy(&endpoint);
        return;
    }

    /* Fill the mailbox before starting the sender thread. */
    IpcMessage first;

    k_memset(&first, 0, sizeof(first));

    first.word_count = 2U;
    first.words[0] = 0x1122334455667788ULL;
    first.words[1] = 0x0102030405060708ULL;

    bool prefilled = ipc_try_send(kernel_process, send_handle, &first);
    terminal_write("  PREFILL ENDPOINT: ");
    terminal_writeln(prefilled ? "PASS" : "FAILED");

    if (!prefilled) {
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        return;
    }

    Thread sender;
    bool sender_created = thread_create(&sender, kernel_process);
    terminal_write("  SENDER CREATE: ");
    terminal_writeln(sender_created ? "PASS" : "FAILED");

    if (!sender_created) return;

    g_ipcsend_started = false;
    g_ipcsend_completed = false;
    g_ipcsend_failed = false;
    g_ipcsend_handle = send_handle;

    bool prepared = thread_prepare_kernel(&sender, ipcsend_sender_worker, 0);
    terminal_write("  SENDER PREPARE: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&sender);
        return;
    }

    bool queued = scheduler_add(&sender);
    terminal_write("  SENDER QUEUE: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&sender);
        return;
    }

    /*
     * main -> sender
     *
     * Mailbox is already full, so blocking SEND
     * stages its second message and BLOCKs.
     */
    interrupts_disable();

    bool first_yield = scheduler_yield();

    interrupts_enable();

    bool sender_started = g_ipcsend_started;
    bool sender_blocked = first_yield && sender_started && !g_ipcsend_completed && !g_ipcsend_failed &&
        sender.state == THREAD_STATE_BLOCKED && !sender.on_run_queue && sender.interrupt_context_ready &&
        sender.interrupt_rsp;

    bool sender_waiting = sender_blocked && endpoint_sender_waiting(&endpoint);
    bool first_still_pending = endpoint_message_ready(&endpoint);
    bool queue_one = scheduler_thread_count() == 1;
    terminal_write("  FIRST YIELD: ");
    terminal_writeln(first_yield ? "PASS" : "FAILED");
    terminal_write("  SENDER STARTED: ");
    terminal_writeln(sender_started ? "PASS" : "FAILED");
    terminal_write("  SENDER BLOCKED: ");
    terminal_writeln(sender_blocked ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT SENDER WAITER: ");
    terminal_writeln(sender_waiting ? "PASS" : "FAILED");
    terminal_write("  ORIGINAL MESSAGE PENDING: ");
    terminal_writeln(first_still_pending ? "PASS" : "FAILED");
    terminal_write("  RUN QUEUE COUNT 1: ");
    terminal_writeln(queue_one ? "PASS" : "FAILED");

    if (!sender_blocked || !sender_waiting || !first_still_pending || !queue_one) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("BLOCKING IPC SEND TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * Consuming the first message creates one
     * free mailbox slot.
     *
     * ipc_try_receive() must immediately:
     *
     *   1. return first message
     *   2. promote blocked sender's message
     *   3. wake sender
     */
    IpcMessage first_received;
    bool first_received_ok = ipc_try_receive(kernel_process, receive_handle, &first_received);

    bool first_payload_ok = first_received_ok && first_received.word_count == 2U &&
        first_received.words[0] == 0x1122334455667788ULL && first_received.words[1] == 0x0102030405060708ULL;

    bool second_pending = first_received_ok && endpoint_message_ready(&endpoint);

    bool sender_woken = first_received_ok && sender.state == THREAD_STATE_READY && sender.on_run_queue &&
        sender.interrupt_context_ready && sender.interrupt_rsp && scheduler_thread_count() == 2;

    /* Sender reservation remains until sender's saved context actually resumes. */
    bool sender_reservation_kept = sender_woken && endpoint_sender_waiting(&endpoint);
    terminal_write("  RECEIVE FIRST: ");
    terminal_writeln(first_received_ok ? "PASS" : "FAILED");
    terminal_write("  FIRST PAYLOAD MATCH: ");
    terminal_writeln(first_payload_ok ? "PASS" : "FAILED");
    terminal_write("  SECOND MESSAGE PROMOTED: ");
    terminal_writeln(second_pending ? "PASS" : "FAILED");
    terminal_write("  SENDER WOKEN: ");
    terminal_writeln(sender_woken ? "PASS" : "FAILED");
    terminal_write("  SENDER RESERVATION KEPT: ");
    terminal_writeln(sender_reservation_kept ? "PASS" : "FAILED");

    if (!first_payload_ok || !second_pending || !sender_woken || !sender_reservation_kept) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("BLOCKING IPC SEND TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * main -> resumed sender
     *
     * ipc_send_blocking() observes that its
     * staged message was promoted, clears its
     * Endpoint reservation and returns true.
     *
     * Worker then exits.
     */
    interrupts_disable();

    bool second_yield = scheduler_yield();

    interrupts_enable();

    bool sender_completed = second_yield && g_ipcsend_completed && !g_ipcsend_failed;
    bool sender_dead = sender.state == THREAD_STATE_DEAD && !sender.on_run_queue &&
        !sender.interrupt_context_ready && !sender.interrupt_rsp;

    bool sender_waiter_cleared = !endpoint_sender_waiting(&endpoint);
    bool queue_restored = scheduler_thread_count() == 1 && thread_current() == main_thread;
    terminal_write("  SECOND YIELD: ");
    terminal_writeln(second_yield ? "PASS" : "FAILED");
    terminal_write("  BLOCKING SEND RETURNED: ");
    terminal_writeln(sender_completed ? "PASS" : "FAILED");
    terminal_write("  SENDER DEAD: ");
    terminal_writeln(sender_dead ? "PASS" : "FAILED");
    terminal_write("  SENDER WAITER CLEARED: ");
    terminal_writeln(sender_waiter_cleared ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    /* Sender's second message must still be in the Endpoint even though send() has now returned. */
    IpcMessage second_received;
    bool second_received_ok = ipc_try_receive(kernel_process, receive_handle, &second_received);
    bool second_payload_ok = second_received_ok && second_received.word_count == 2U &&
        second_received.words[0] == 0xAABBCCDDEEFF0011ULL &&
        second_received.words[1] == 0x4A434F5349504303ULL;

    bool endpoint_empty = second_received_ok && !endpoint_message_ready(&endpoint);
    terminal_write("  RECEIVE SECOND: ");
    terminal_writeln(second_received_ok ? "PASS" : "FAILED");
    terminal_write("  SECOND PAYLOAD MATCH: ");
    terminal_writeln(second_payload_ok ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(endpoint_empty ? "PASS" : "FAILED");

    bool reaped = sender_dead && thread_destroy(&sender);
    bool thread_count_restored = process_thread_count(kernel_process) == 1ULL;
    terminal_write("  SENDER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");
    terminal_write("  KERNEL THREAD COUNT RESTORED: ");
    terminal_writeln(thread_count_restored ? "PASS" : "FAILED");

    bool send_revoked = capability_revoke(caps, send_handle);
    bool receive_revoked = capability_revoke(caps, receive_handle);
    bool caps_restored = send_revoked && receive_revoked && capability_table_count(caps) == caps_before;
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(caps_restored ? "PASS" : "FAILED");

    bool endpoint_destroyed = caps_restored && endpoint_empty && sender_waiter_cleared && endpoint_destroy(&endpoint);
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && caps_ready && endpoint_created && send_cap && receive_cap && prefilled &&
        sender_created && prepared && queued && first_yield && sender_started && sender_blocked &&
        sender_waiting && first_still_pending && queue_one && first_received_ok && first_payload_ok &&
        second_pending && sender_woken && sender_reservation_kept && second_yield && sender_completed &&
        sender_dead && sender_waiter_cleared && queue_restored && second_received_ok && second_payload_ok &&
        endpoint_empty && reaped && thread_count_restored && caps_restored && endpoint_destroyed &&
        frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("BLOCKING IPC SEND TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_useripctest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    const u64 user_result = ADDRESS_SPACE_USER_BASE + 2ULL * VM_PAGE_SIZE;
    const u64 word0 = 0x1122334455667788ULL;
    const u64 word1 = 0x8877665544332211ULL;
    const u64 word2 = 0x4A434F5355534552ULL;
    terminal_writeln("USER IPC SYSCALL TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS CREATE: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        return;
    }

    CapabilityTable *caps = process_capabilities(&process);
    Endpoint endpoint;
    bool endpoint_created = caps && endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) {
        (void)process_destroy(&process);
        return;
    }

    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    bool send_cap = capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    bool receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    terminal_write("  SEND CAP: ");
    terminal_writeln(send_cap ? "PASS" : "FAILED");
    terminal_write("  RECEIVE CAP: ");
    terminal_writeln(receive_cap ? "PASS" : "FAILED");

    if (!receive_cap) {
        if (send_cap) (void)capability_revoke(caps, send_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    Thread user;
    bool thread_created = thread_create(&user, &process);
    terminal_write("  USER THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    frame_t result_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID && result_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);
        if (result_frame != FRAME_INVALID) (void)frame_free(result_frame);

        (void)thread_destroy(&user);
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool result_mapped = address_space_map_page(space, user_result, result_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped && result_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);
        if (result_mapped) (void)address_space_unmap_page(space, user_result, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(result_frame);
        (void)thread_destroy(&user);
        (void)capability_revoke(caps, send_handle);
        (void)capability_revoke(caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));
    u64 *result = (u64 *)phys_to_virt(frame_to_phys(result_frame));
    bool direct_ok = code && result;
    terminal_write("  PHYSMAP ACCESS: ");
    terminal_writeln(direct_ok ? "PASS" : "FAILED");

    if (!direct_ok) {
        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, user_code, &ignored);

        (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)address_space_unmap_page(space, user_result, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(result_frame);

        (void)thread_destroy(&user);

        (void)capability_revoke(caps, send_handle);

        (void)capability_revoke(caps, receive_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    k_memset(code, 0, (usize)VM_PAGE_SIZE);
    k_memset(result, 0, (usize)VM_PAGE_SIZE);

    /*
     * Result layout:
     *
     * +0   wrong-right SEND result
     * +8   valid SEND result
     * +16  RECEIVE result
     * +24  received word count
     * +32  received word 0
     * +40  received word 1
     * +48  received word 2
     * +56  received word 3
     */

    u8 *p = code;

    /*
     * r9 = result page
     *
     * 49 B9 imm64
     */
    *p++ = 0x49;
    *p++ = 0xB9;

    shell_emit_u64_le(&p, user_result);

    /*
     * Prepare a 3-word message.
     *
     * ecx = 3
     */
    *p++ = 0xB9;

    shell_emit_u32_le(&p, 3U);

    /* rdx = word0 */
    *p++ = 0x48;
    *p++ = 0xBA;

    shell_emit_u64_le(&p, word0);

    /* rsi = word1 */
    *p++ = 0x48;
    *p++ = 0xBE;

    shell_emit_u64_le(&p, word1);

    /* rdi = word2 */
    *p++ = 0x48;
    *p++ = 0xBF;

    shell_emit_u64_le(&p, word2);

    /* xor r8d, r8d */
    *p++ = 0x45;
    *p++ = 0x31;
    *p++ = 0xC0;

    /*
     * First deliberately use the RECEIVE-only
     * handle for SEND.
     *
     * rbx = receive_handle
     */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, receive_handle);

    /* eax = SYSCALL_IPC_TRY_SEND */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_IPC_TRY_SEND);

    /* int 0x80 */
    *p++ = 0xCD;
    *p++ = 0x80;

    /*
     * mov [r9], rax
     *
     * wrong-right result
     */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x01;

    /*
     * Valid SEND.
     *
     * rbx = send_handle
     */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, send_handle);

    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_IPC_TRY_SEND);

    *p++ = 0xCD;
    *p++ = 0x80;

    /* mov [r9 + 8], rax */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x41;
    *p++ = 0x08;

    /*
     * Valid RECEIVE.
     *
     * rbx = receive_handle
     */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, receive_handle);

    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_IPC_TRY_RECEIVE);

    *p++ = 0xCD;
    *p++ = 0x80;

    /* mov [r9 + 16], rax */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x41;
    *p++ = 0x10;

    /* mov [r9 + 24], rcx */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x49;
    *p++ = 0x18;

    /* mov [r9 + 32], rdx */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x51;
    *p++ = 0x20;

    /* mov [r9 + 40], rsi */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x71;
    *p++ = 0x28;

    /* mov [r9 + 48], rdi */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x79;
    *p++ = 0x30;

    /* mov [r9 + 56], r8 */
    *p++ = 0x4D;
    *p++ = 0x89;
    *p++ = 0x41;
    *p++ = 0x38;

    /* eax = SYSCALL_THREAD_EXIT */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_THREAD_EXIT);

    /*
    * int 0x80
    *
    * Success never returns here.
    */
    *p++ = 0xCD;
    *p++ = 0x80;

    /*
    * Guard.
    *
    * If THREAD_EXIT ever returns unexpectedly,
    * UD2 must terminate the user thread and make
    * this test fail because a user fault will have
    * been recorded.
    */
    *p++ = 0x0F;
    *p++ = 0x0B;

    bool prepared = thread_prepare_user(&user, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) return;

    bool queued = scheduler_add(&user);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) return;

    interrupt_clear_user_fault();

    interrupts_disable();

    bool yielded = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);
    bool shell_restored = yielded && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;

    bool user_exited = shell_restored && user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    bool clean_syscall_exit = user_exited && !fault_captured;
    terminal_write("  USER RETURNED TO SHELL: ");
    terminal_writeln(shell_restored ? "PASS" : "FAILED");
    terminal_write("  THREAD EXIT SYSCALL: ");
    terminal_writeln(clean_syscall_exit ? "PASS" : "FAILED");
    terminal_write("  NO USER FAULT: ");
    terminal_writeln(!fault_captured ? "PASS" : "FAILED");

    bool wrong_right_rejected = result[0] == SYSCALL_RESULT_FAILED;
    bool send_ok = result[1] == SYSCALL_RESULT_OK;
    bool receive_ok = result[2] == SYSCALL_RESULT_OK;

    bool payload_ok = result[3] == 3ULL && result[4] == word0 && result[5] == word1 && result[6] == word2 &&
        result[7] == 0ULL;

    terminal_write("  WRONG-RIGHT SEND REJECTED: ");
    terminal_writeln(wrong_right_rejected ? "PASS" : "FAILED");
    terminal_write("  SEND SYSCALL: ");
    terminal_writeln(send_ok ? "PASS" : "FAILED");
    terminal_write("  RECEIVE SYSCALL: ");
    terminal_writeln(receive_ok ? "PASS" : "FAILED");
    terminal_write("  PAYLOAD MATCH: ");
    terminal_writeln(payload_ok ? "PASS" : "FAILED");

    bool endpoint_empty = !endpoint_message_ready(&endpoint);
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(endpoint_empty ? "PASS" : "FAILED");

    bool reaped = clean_syscall_exit && thread_destroy(&user);
    terminal_write("  USER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    frame_t old_result = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool result_unmapped = address_space_unmap_page(space, user_result, &old_result) && old_result == result_frame;
    bool code_freed = code_unmapped && frame_free(code_frame);
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool result_freed = result_unmapped && frame_free(result_frame);
    bool send_revoked = capability_revoke(caps, send_handle);
    bool receive_revoked = capability_revoke(caps, receive_handle);
    bool caps_empty = send_revoked && receive_revoked && capability_table_count(caps) == 0U;
    bool endpoint_destroyed = caps_empty && endpoint_empty && endpoint_destroy(&endpoint);
    bool process_destroyed = endpoint_destroyed && process_destroy(&process);
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped && result_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed && result_freed ? "PASS" : "FAILED");
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(caps_empty ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && process_created && space_ok && endpoint_created && send_cap && receive_cap &&
        thread_created && frames_ok && mappings_ok && direct_ok && prepared && queued && yielded &&
        shell_restored && clean_syscall_exit && !fault_captured && wrong_right_rejected && send_ok && 
        receive_ok && payload_ok && endpoint_empty && reaped && code_unmapped && stack_unmapped && 
        result_unmapped && code_freed && stack_freed && result_freed && caps_empty && endpoint_destroyed && 
        process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER IPC SYSCALL TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_useripcblocktest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    const u64 user_result = ADDRESS_SPACE_USER_BASE + 2ULL * VM_PAGE_SIZE;
    const u64 word0 = 0x1122334455667788ULL;
    const u64 word1 = 0x8877665544332211ULL;
    const u64 word2 = 0x4A434F53424C4B52ULL;
    terminal_writeln("USER BLOCKING IPC RECEIVE TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();

    bool main_ok = main_thread && kernel_process && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled();

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    CapabilityTable *kernel_caps = process_capabilities(kernel_process);
    bool kernel_caps_ready = kernel_caps != 0;
    u32 kernel_caps_before = kernel_caps ? capability_table_count(kernel_caps) : 0U;
    terminal_write("  KERNEL CAP TABLE: ");
    terminal_writeln(kernel_caps_ready ? "PASS" : "FAILED");
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(kernel_caps_before);
    terminal_putchar('\n');

    if (!kernel_caps_ready) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  USER PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    CapabilityTable *user_caps = process_capabilities(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    bool user_caps_ok = user_caps && capability_table_count(user_caps) == 0U;
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");
    terminal_write("  USER CAP TABLE EMPTY: ");
    terminal_writeln(user_caps_ok ? "PASS" : "FAILED");

    if (!space_ok || !user_caps_ok) {
        (void)process_destroy(&process);
        return;
    }

    Endpoint endpoint;
    bool endpoint_created = endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) {
        (void)process_destroy(&process);
        return;
    }

    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;

    /* Kernel gets SEND authority. */
    bool send_cap =
        capability_insert(kernel_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    /* User process gets RECEIVE authority. */
    bool receive_cap = send_cap &&
        capability_insert(user_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    terminal_write("  KERNEL SEND CAP: ");
    terminal_writeln(send_cap ? "PASS" : "FAILED");
    terminal_write("  USER RECEIVE CAP: ");
    terminal_writeln(receive_cap ? "PASS" : "FAILED");

    if (!receive_cap) {
        if (send_cap) (void)capability_revoke(kernel_caps, send_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    Thread user;
    bool thread_created = thread_create(&user, &process);
    terminal_write("  USER THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)capability_revoke(kernel_caps, send_handle);
        (void)capability_revoke(user_caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    frame_t result_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID && result_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);
        if (result_frame != FRAME_INVALID) (void)frame_free(result_frame);

        (void)thread_destroy(&user);
        (void)capability_revoke(kernel_caps, send_handle);
        (void)capability_revoke(user_caps, receive_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool result_mapped = address_space_map_page(space, user_result, result_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped && result_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);
        if (result_mapped) (void)address_space_unmap_page(space, user_result, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(result_frame);

        (void)thread_destroy(&user);

        (void)capability_revoke(kernel_caps, send_handle);

        (void)capability_revoke(user_caps, receive_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));
    u64 *result = (u64 *)phys_to_virt(frame_to_phys(result_frame));
    bool direct_ok = code && result;
    terminal_write("  PHYSMAP ACCESS: ");
    terminal_writeln(direct_ok ? "PASS" : "FAILED");

    if (!direct_ok) {
        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, user_code, &ignored);

        (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)address_space_unmap_page(space, user_result, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(result_frame);

        (void)thread_destroy(&user);

        (void)capability_revoke(kernel_caps, send_handle);

        (void)capability_revoke(user_caps, receive_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    k_memset(code, 0, (usize)VM_PAGE_SIZE);
    k_memset(result, 0, (usize)VM_PAGE_SIZE);

    /*
     * Result page:
     *
     * +0   receive syscall result
     * +8   word count
     * +16  word 0
     * +24  word 1
     * +32  word 2
     * +40  word 3
     */

    u8 *p = code;

    /* r9 = result page */
    *p++ = 0x49;
    *p++ = 0xB9;

    shell_emit_u64_le(&p, user_result);

    /* rbx = user's RECEIVE capability */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, receive_handle);

    /* eax = blocking RECEIVE syscall */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_IPC_RECEIVE_BLOCKING);

    /*
     * int 0x80
     *
     * This must initially BLOCK.
     */
    *p++ = 0xCD;
    *p++ = 0x80;

    /* mov [r9], rax */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x01;

    /* mov [r9 + 8], rcx */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x49;
    *p++ = 0x08;

    /* mov [r9 + 16], rdx */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x51;
    *p++ = 0x10;

    /* mov [r9 + 24], rsi */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x71;
    *p++ = 0x18;

    /* mov [r9 + 32], rdi */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x79;
    *p++ = 0x20;

    /* mov [r9 + 40], r8 */
    *p++ = 0x4D;
    *p++ = 0x89;
    *p++ = 0x41;
    *p++ = 0x28;

    /* Exit cleanly after RECEIVE returns. */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_THREAD_EXIT);

    *p++ = 0xCD;
    *p++ = 0x80;

    /* Tripwire only. */
    *p++ = 0x0F;
    *p++ = 0x0B;

    bool prepared = thread_prepare_user(&user, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) return;

    bool queued = scheduler_add(&user);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) return;

    interrupt_clear_user_fault();

    /*
     * main -> user
     *
     * User enters INT 0x80 and blocking RECEIVE.
     * RECEIVE internally uses INT 0x81 to block,
     * which must restore this main context.
     */
    interrupts_disable();

    bool first_yield = scheduler_yield();

    interrupts_enable();

    bool main_after_block = first_yield && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING;

    bool user_blocked = user.state == THREAD_STATE_BLOCKED && !user.on_run_queue &&
        user.interrupt_context_ready && user.interrupt_rsp;

    bool waiter_installed = user_blocked && endpoint_receiver_waiting(&endpoint);
    bool queue_one = scheduler_thread_count() == 1;
    terminal_write("  FIRST YIELD: ");
    terminal_writeln(first_yield ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED AFTER BLOCK: ");
    terminal_writeln(main_after_block ? "PASS" : "FAILED");
    terminal_write("  USER BLOCKED: ");
    terminal_writeln(user_blocked ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT WAITER: ");
    terminal_writeln(waiter_installed ? "PASS" : "FAILED");
    terminal_write("  RUN QUEUE COUNT 1: ");
    terminal_writeln(queue_one ? "PASS" : "FAILED");

    if (!main_after_block || !user_blocked || !waiter_installed || !queue_one) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER BLOCKING IPC RECEIVE TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    message.word_count = 3U;
    message.words[0] = word0;
    message.words[1] = word1;
    message.words[2] = word2;

    /* Kernel Process exercises its SEND capability to wake the user Process. */
    bool sent = ipc_try_send(kernel_process, send_handle, &message);
    bool message_pending = sent && endpoint_message_ready(&endpoint);

    bool user_woken = sent && user.state == THREAD_STATE_READY && user.on_run_queue &&
        user.interrupt_context_ready && user.interrupt_rsp && scheduler_thread_count() == 2;

    bool waiter_reserved = user_woken && endpoint_receiver_waiting(&endpoint);
    terminal_write("  KERNEL SEND: ");
    terminal_writeln(sent ? "PASS" : "FAILED");
    terminal_write("  MESSAGE PENDING: ");
    terminal_writeln(message_pending ? "PASS" : "FAILED");
    terminal_write("  USER WOKEN: ");
    terminal_writeln(user_woken ? "PASS" : "FAILED");
    terminal_write("  RECEIVER RESERVATION KEPT: ");
    terminal_writeln(waiter_reserved ? "PASS" : "FAILED");

    if (!sent || !message_pending || !user_woken || !waiter_reserved) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER BLOCKING IPC RECEIVE TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * main -> resumed user kernel context
     *
     * ipc_receive_blocking() finishes,
     * syscall_dispatch returns the original
     * INT 0x80 frame, IRETQ returns to Ring3,
     * results are stored, then THREAD_EXIT
     * restores main again.
     */
    interrupts_disable();

    bool second_yield = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);

    bool main_restored = second_yield && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1;

    bool user_dead = user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    bool syscall_ok = result[0] == SYSCALL_RESULT_OK;

    bool payload_ok = result[1] == 3ULL && result[2] == word0 && result[3] == word1 && result[4] == word2 &&
        result[5] == 0ULL;

    bool endpoint_empty = !endpoint_message_ready(&endpoint);
    bool waiter_cleared = !endpoint_receiver_waiting(&endpoint);
    bool clean_exit = main_restored && user_dead && !fault_captured;
    terminal_write("  SECOND YIELD: ");
    terminal_writeln(second_yield ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(main_restored ? "PASS" : "FAILED");
    terminal_write("  BLOCKING RECEIVE RETURNED: ");
    terminal_writeln(syscall_ok ? "PASS" : "FAILED");
    terminal_write("  PAYLOAD MATCH: ");
    terminal_writeln(payload_ok ? "PASS" : "FAILED");
    terminal_write("  USER EXITED: ");
    terminal_writeln(clean_exit ? "PASS" : "FAILED");
    terminal_write("  NO USER FAULT: ");
    terminal_writeln(!fault_captured ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(endpoint_empty ? "PASS" : "FAILED");
    terminal_write("  WAITER CLEARED: ");
    terminal_writeln(waiter_cleared ? "PASS" : "FAILED");

    bool reaped = clean_exit && thread_destroy(&user);
    terminal_write("  USER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    frame_t old_result = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool result_unmapped = address_space_unmap_page(space, user_result, &old_result) && old_result == result_frame;
    bool code_freed = code_unmapped && frame_free(code_frame);
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool result_freed = result_unmapped && frame_free(result_frame);
    bool send_revoked = capability_revoke(kernel_caps, send_handle);
    bool receive_revoked = capability_revoke(user_caps, receive_handle);

    bool caps_restored = send_revoked && receive_revoked && capability_table_count(kernel_caps) == kernel_caps_before &&
        capability_table_count(user_caps) == 0U;

    bool endpoint_destroyed = caps_restored && endpoint_empty && waiter_cleared && endpoint_destroy(&endpoint);
    bool process_destroyed = endpoint_destroyed && reaped && process_destroy(&process);
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped && result_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed && result_freed ? "PASS" : "FAILED");
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(caps_restored ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && kernel_caps_ready && process_created && space_ok && user_caps_ok &&
        endpoint_created && send_cap && receive_cap && thread_created && frames_ok && mappings_ok &&
        direct_ok && prepared && queued && first_yield && main_after_block && user_blocked &&
        waiter_installed && queue_one && sent && message_pending && user_woken && waiter_reserved &&
        second_yield && main_restored && syscall_ok && payload_ok && clean_exit && !fault_captured &&
        endpoint_empty && waiter_cleared && reaped && code_unmapped && stack_unmapped && result_unmapped &&
        code_freed && stack_freed && result_freed && caps_restored && endpoint_destroyed &&
        process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER BLOCKING IPC RECEIVE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_useripcsendblocktest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    const u64 user_result = ADDRESS_SPACE_USER_BASE + 2ULL * VM_PAGE_SIZE;
    const u64 first_word0 = 0x1122334455667788ULL;
    const u64 first_word1 = 0x0102030405060708ULL;
    const u64 second_word0 = 0xAABBCCDDEEFF0011ULL;
    const u64 second_word1 = 0x4A434F5355534253ULL;
    terminal_writeln("USER BLOCKING IPC SEND TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();

    bool main_ok = main_thread && kernel_process && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled();

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    CapabilityTable *kernel_caps = process_capabilities(kernel_process);
    bool kernel_caps_ready = kernel_caps != 0;
    u32 kernel_caps_before = kernel_caps ? capability_table_count(kernel_caps) : 0U;
    terminal_write("  KERNEL CAP TABLE: ");
    terminal_writeln(kernel_caps_ready ? "PASS" : "FAILED");
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(kernel_caps_before);
    terminal_putchar('\n');

    if (!kernel_caps_ready) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  USER PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    CapabilityTable *user_caps = process_capabilities(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    bool user_caps_ok = user_caps && capability_table_count(user_caps) == 0U;
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");
    terminal_write("  USER CAP TABLE EMPTY: ");
    terminal_writeln(user_caps_ok ? "PASS" : "FAILED");

    if (!space_ok || !user_caps_ok) {
        (void)process_destroy(&process);
        return;
    }

    Endpoint endpoint;
    bool endpoint_created = endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    if (!endpoint_created) {
        (void)process_destroy(&process);
        return;
    }

    CapabilityHandle kernel_send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_receive_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle user_send_handle = CAPABILITY_INVALID_HANDLE;

    bool kernel_send_cap =
        capability_insert(kernel_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &kernel_send_handle);

    bool kernel_receive_cap = kernel_send_cap &&
        capability_insert(kernel_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
            &kernel_receive_handle);

    bool user_send_cap = kernel_receive_cap &&
        capability_insert(user_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &user_send_handle);

    terminal_write("  KERNEL SEND CAP: ");
    terminal_writeln(kernel_send_cap ? "PASS" : "FAILED");
    terminal_write("  KERNEL RECEIVE CAP: ");
    terminal_writeln(kernel_receive_cap ? "PASS" : "FAILED");
    terminal_write("  USER SEND CAP: ");
    terminal_writeln(user_send_cap ? "PASS" : "FAILED");

    if (!user_send_cap) {
        if (kernel_send_cap) (void)capability_revoke(kernel_caps, kernel_send_handle);
        if (kernel_receive_cap) (void)capability_revoke(kernel_caps, kernel_receive_handle);

        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    /* Fill the mailbox before the user runs. */
    IpcMessage first_message;

    k_memset(&first_message, 0, sizeof(first_message));

    first_message.word_count = 2U;
    first_message.words[0] = first_word0;
    first_message.words[1] = first_word1;

    bool prefilled = ipc_try_send(kernel_process, kernel_send_handle, &first_message);
    terminal_write("  PREFILL ENDPOINT: ");
    terminal_writeln(prefilled ? "PASS" : "FAILED");

    if (!prefilled) {
        (void)capability_revoke(kernel_caps, kernel_send_handle);
        (void)capability_revoke(kernel_caps, kernel_receive_handle);
        (void)capability_revoke(user_caps, user_send_handle);
        (void)endpoint_destroy(&endpoint);
        (void)process_destroy(&process);
        return;
    }

    Thread user;
    bool thread_created = thread_create(&user, &process);
    terminal_write("  USER THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) return;

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    frame_t result_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID && result_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) return;

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool result_mapped = address_space_map_page(space, user_result, result_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped && result_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) return;

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));
    u64 *result = (u64 *)phys_to_virt(frame_to_phys(result_frame));
    bool direct_ok = code && result;
    terminal_write("  PHYSMAP ACCESS: ");
    terminal_writeln(direct_ok ? "PASS" : "FAILED");

    if (!direct_ok) return;

    k_memset(code, 0, (usize)VM_PAGE_SIZE);
    k_memset(result, 0, (usize)VM_PAGE_SIZE);

    /*
     * Ring3 program:
     *
     * r9 = result page
     *
     * SEND second message using blocking SEND.
     * Store RAX after it eventually returns.
     * Then terminate through THREAD_EXIT.
     */

    u8 *p = code;

    /* mov r9, user_result */
    *p++ = 0x49;
    *p++ = 0xB9;

    shell_emit_u64_le(&p, user_result);

    /* mov rbx, user_send_handle */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, user_send_handle);

    /* mov ecx, 2 */
    *p++ = 0xB9;

    shell_emit_u32_le(&p, 2U);

    /* mov rdx, second_word0 */
    *p++ = 0x48;
    *p++ = 0xBA;

    shell_emit_u64_le(&p, second_word0);

    /* mov rsi, second_word1 */
    *p++ = 0x48;
    *p++ = 0xBE;

    shell_emit_u64_le(&p, second_word1);

    /* xor edi, edi */
    *p++ = 0x31;
    *p++ = 0xFF;

    /* xor r8d, r8d */
    *p++ = 0x45;
    *p++ = 0x31;
    *p++ = 0xC0;

    /* eax = SYSCALL_IPC_SEND_BLOCKING */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_IPC_SEND_BLOCKING);

    /* This INT 0x80 must block until main receives the original message. */
    *p++ = 0xCD;
    *p++ = 0x80;

    /* mov [r9], rax */
    *p++ = 0x49;
    *p++ = 0x89;
    *p++ = 0x01;

    /* eax = SYSCALL_THREAD_EXIT */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_THREAD_EXIT);

    *p++ = 0xCD;
    *p++ = 0x80;

    /* THREAD_EXIT tripwire. */
    *p++ = 0x0F;
    *p++ = 0x0B;

    bool prepared = thread_prepare_user(&user, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) return;

    bool queued = scheduler_add(&user);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) return;

    interrupt_clear_user_fault();

    /*
     * main -> user
     *
     * Endpoint is full. User's blocking SEND
     * must stage the second message and block.
     */
    interrupts_disable();

    bool first_yield = scheduler_yield();

    interrupts_enable();

    bool main_after_block = first_yield && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING;

    bool user_blocked = user.state == THREAD_STATE_BLOCKED && !user.on_run_queue &&
        user.interrupt_context_ready && user.interrupt_rsp;

    bool sender_waiting = user_blocked && endpoint_sender_waiting(&endpoint);
    bool original_pending = endpoint_message_ready(&endpoint);
    bool queue_one = scheduler_thread_count() == 1;
    terminal_write("  FIRST YIELD: ");
    terminal_writeln(first_yield ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED AFTER BLOCK: ");
    terminal_writeln(main_after_block ? "PASS" : "FAILED");
    terminal_write("  USER BLOCKED: ");
    terminal_writeln(user_blocked ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT SENDER WAITER: ");
    terminal_writeln(sender_waiting ? "PASS" : "FAILED");
    terminal_write("  ORIGINAL MESSAGE PENDING: ");
    terminal_writeln(original_pending ? "PASS" : "FAILED");
    terminal_write("  RUN QUEUE COUNT 1: ");
    terminal_writeln(queue_one ? "PASS" : "FAILED");

    if (!main_after_block || !user_blocked || !sender_waiting || !original_pending || !queue_one) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER BLOCKING IPC SEND TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * Receiving message A frees the mailbox.
     *
     * IPC must then promote user message B and
     * wake the blocked user thread.
     */
    IpcMessage first_received;

    k_memset(&first_received, 0, sizeof(first_received));

    bool first_received_ok = ipc_try_receive(kernel_process, kernel_receive_handle, &first_received);

    bool first_payload_ok = first_received_ok && first_received.word_count == 2U &&
        first_received.words[0] == first_word0 && first_received.words[1] == first_word1;

    bool second_pending = first_received_ok && endpoint_message_ready(&endpoint);

    bool user_woken = first_received_ok && user.state == THREAD_STATE_READY && user.on_run_queue &&
        user.interrupt_context_ready && user.interrupt_rsp && scheduler_thread_count() == 2;

    bool sender_reserved = user_woken && endpoint_sender_waiting(&endpoint);
    terminal_write("  RECEIVE FIRST: ");
    terminal_writeln(first_received_ok ? "PASS" : "FAILED");
    terminal_write("  FIRST PAYLOAD MATCH: ");
    terminal_writeln(first_payload_ok ? "PASS" : "FAILED");
    terminal_write("  SECOND MESSAGE PROMOTED: ");
    terminal_writeln(second_pending ? "PASS" : "FAILED");
    terminal_write("  USER WOKEN: ");
    terminal_writeln(user_woken ? "PASS" : "FAILED");
    terminal_write("  SENDER RESERVATION KEPT: ");
    terminal_writeln(sender_reserved ? "PASS" : "FAILED");

    if (!first_payload_ok || !second_pending || !user_woken || !sender_reserved) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER BLOCKING IPC SEND TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    /*
     * Resume the user.
     *
     * ipc_send_blocking() completes, INT 0x80
     * returns to Ring3, RAX is stored, then
     * THREAD_EXIT returns us here.
     */
    interrupts_disable();

    bool second_yield = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);

    bool main_restored = second_yield && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1;

    bool user_dead = user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    bool send_syscall_ok = result[0] == SYSCALL_RESULT_OK;
    bool sender_cleared = !endpoint_sender_waiting(&endpoint);
    bool clean_exit = main_restored && user_dead && !fault_captured;
    terminal_write("  SECOND YIELD: ");
    terminal_writeln(second_yield ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(main_restored ? "PASS" : "FAILED");
    terminal_write("  BLOCKING SEND RETURNED: ");
    terminal_writeln(send_syscall_ok ? "PASS" : "FAILED");
    terminal_write("  USER EXITED: ");
    terminal_writeln(clean_exit ? "PASS" : "FAILED");
    terminal_write("  NO USER FAULT: ");
    terminal_writeln(!fault_captured ? "PASS" : "FAILED");
    terminal_write("  SENDER WAITER CLEARED: ");
    terminal_writeln(sender_cleared ? "PASS" : "FAILED");

    /* Message B must still be pending after the sending syscall itself has returned. */
    IpcMessage second_received;

    k_memset(&second_received, 0, sizeof(second_received));

    bool second_received_ok = ipc_try_receive(kernel_process, kernel_receive_handle, &second_received);

    bool second_payload_ok = second_received_ok && second_received.word_count == 2U &&
        second_received.words[0] == second_word0 && second_received.words[1] == second_word1;

    bool endpoint_empty = second_received_ok && !endpoint_message_ready(&endpoint);
    terminal_write("  RECEIVE SECOND: ");
    terminal_writeln(second_received_ok ? "PASS" : "FAILED");
    terminal_write("  SECOND PAYLOAD MATCH: ");
    terminal_writeln(second_payload_ok ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT EMPTY: ");
    terminal_writeln(endpoint_empty ? "PASS" : "FAILED");

    bool reaped = clean_exit && thread_destroy(&user);
    terminal_write("  USER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    frame_t old_result = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool result_unmapped = address_space_unmap_page(space, user_result, &old_result) && old_result == result_frame;
    bool code_freed = code_unmapped && frame_free(code_frame);
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool result_freed = result_unmapped && frame_free(result_frame);
    bool kernel_send_revoked = capability_revoke(kernel_caps, kernel_send_handle);
    bool kernel_receive_revoked = capability_revoke(kernel_caps, kernel_receive_handle);
    bool user_send_revoked = capability_revoke(user_caps, user_send_handle);

    bool caps_restored = kernel_send_revoked && kernel_receive_revoked && user_send_revoked &&
        capability_table_count(kernel_caps) == kernel_caps_before && capability_table_count(user_caps) == 0U;

    bool endpoint_destroyed = caps_restored && endpoint_empty && sender_cleared && endpoint_destroy(&endpoint);
    bool process_destroyed = endpoint_destroyed && reaped && process_destroy(&process);
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped && result_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed && result_freed ? "PASS" : "FAILED");
    terminal_write("  REVOKE CAPS: ");
    terminal_writeln(caps_restored ? "PASS" : "FAILED");
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && kernel_caps_ready && process_created && space_ok && user_caps_ok &&
        endpoint_created && kernel_send_cap && kernel_receive_cap && user_send_cap && prefilled &&
        thread_created && frames_ok && mappings_ok && direct_ok && prepared && queued && first_yield &&
        main_after_block && user_blocked && sender_waiting && original_pending && queue_one &&
        first_received_ok && first_payload_ok && second_pending && user_woken && sender_reserved &&
        second_yield && main_restored && send_syscall_ok && clean_exit && !fault_captured &&
        sender_cleared && second_received_ok && second_payload_ok && endpoint_empty && reaped &&
        code_unmapped && stack_unmapped && result_unmapped && code_freed && stack_freed && result_freed &&
        caps_restored && endpoint_destroyed && process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER BLOCKING IPC SEND TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_endpointtest(void) {
    terminal_writeln("ENDPOINT TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS CREATE: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    CapabilityTable *caps = process_capabilities(&process);
    bool caps_ok = caps && capability_table_count(caps) == 0U;
    terminal_write("  CAP TABLE: ");
    terminal_writeln(caps_ok ? "PASS" : "FAILED");

    Endpoint endpoint;
    bool endpoint_created = caps_ok && endpoint_create(&endpoint);
    terminal_write("  ENDPOINT CREATE: ");
    terminal_writeln(endpoint_created ? "PASS" : "FAILED");

    bool metadata_ok = endpoint_created && endpoint.initialized && endpoint.id != 0ULL;
    terminal_write("  ENDPOINT METADATA: ");
    terminal_writeln(metadata_ok ? "PASS" : "FAILED");

    if (endpoint_created) {
        terminal_write("  ENDPOINT ID: ");
        terminal_write_u64(endpoint.id);
        terminal_putchar('\n');
    }

    CapabilityHandle handle = CAPABILITY_INVALID_HANDLE;

    bool cap_inserted = metadata_ok &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER, &handle);

    terminal_write("  INSERT ENDPOINT CAP: ");
    terminal_writeln(cap_inserted ? "PASS" : "FAILED");
    terminal_write("  CAP HANDLE: ");
    terminal_write_hex(handle);
    terminal_putchar('\n');

    void *resolved = 0;

    bool send_lookup = cap_inserted &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &resolved) &&
        resolved == &endpoint;

    terminal_write("  SEND RIGHT: ");
    terminal_writeln(send_lookup ? "PASS" : "FAILED");

    resolved = 0;

    bool receive_lookup = cap_inserted &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &resolved) &&
        resolved == &endpoint;

    terminal_write("  RECEIVE RIGHT: ");
    terminal_writeln(receive_lookup ? "PASS" : "FAILED");

    resolved = 0;

    bool combined_lookup = cap_inserted &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_RECEIVE, &resolved) &&
        resolved == &endpoint;

    terminal_write("  SEND/RECEIVE RIGHTS: ");
    terminal_writeln(combined_lookup ? "PASS" : "FAILED");

    resolved = 0;

    bool manage_rejected = cap_inserted &&
        !capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_MANAGE, &resolved);

    terminal_write("  MISSING MANAGE REJECTED: ");
    terminal_writeln(manage_rejected ? "PASS" : "FAILED");

    resolved = 0;

    bool wrong_type_rejected = cap_inserted && !capability_lookup(caps, handle, CAPABILITY_TYPE_THREAD, &resolved);
    terminal_write("  WRONG TYPE REJECTED: ");
    terminal_writeln(wrong_type_rejected ? "PASS" : "FAILED");

    /* The process still owns authority, so its destruction must fail. */
    bool live_cap_process_rejected = cap_inserted && !process_destroy(&process);
    terminal_write("  LIVE-CAP PROCESS DESTROY REJECTED: ");
    terminal_writeln(live_cap_process_rejected ? "PASS" : "FAILED");

    bool revoked = live_cap_process_rejected && capability_revoke(caps, handle);
    terminal_write("  REVOKE ENDPOINT CAP: ");
    terminal_writeln(revoked ? "PASS" : "FAILED");

    resolved = 0;

    bool stale_rejected = revoked && !capability_lookup(caps, handle, CAPABILITY_TYPE_ENDPOINT, &resolved);
    terminal_write("  STALE CAP REJECTED: ");
    terminal_writeln(stale_rejected ? "PASS" : "FAILED");

    bool caps_empty = revoked && capability_table_count(caps) == 0U;
    terminal_write("  CAP TABLE EMPTY: ");
    terminal_writeln(caps_empty ? "PASS" : "FAILED");

    /* Revoke authority before destroying the object itself. */
    bool endpoint_destroyed = caps_empty && endpoint_destroy(&endpoint);
    terminal_write("  ENDPOINT DESTROY: ");
    terminal_writeln(endpoint_destroyed ? "PASS" : "FAILED");

    bool endpoint_cleared = endpoint_destroyed && !endpoint.initialized && endpoint.id == 0ULL;
    terminal_write("  ENDPOINT CLEARED: ");
    terminal_writeln(endpoint_cleared ? "PASS" : "FAILED");

    bool double_destroy_rejected = endpoint_destroyed && !endpoint_destroy(&endpoint);
    terminal_write("  DOUBLE DESTROY REJECTED: ");
    terminal_writeln(double_destroy_rejected ? "PASS" : "FAILED");

    bool process_destroyed = endpoint_destroyed && process_destroy(&process);
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = process_created && caps_ok && endpoint_created && metadata_ok && cap_inserted &&
        send_lookup && receive_lookup && combined_lookup && manage_rejected && wrong_type_rejected &&
        live_cap_process_rejected && revoked && stale_rejected && caps_empty && endpoint_destroyed &&
        endpoint_cleared && double_destroy_rejected && process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("ENDPOINT TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_processtest(void) {
    terminal_writeln("PROCESS TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    /* Verify the adopted kernel process. */
    Process *kernel = process_kernel();

    bool kernel_ok = (kernel && kernel->initialized && kernel->id == 1ULL && kernel->kernel &&
        !kernel->owns_address_space && process_address_space(kernel) == address_space_kernel() &&
        process_thread_count(kernel) == 1ULL);

    terminal_write("  KERNEL PROCESS: ");
    terminal_writeln(kernel_ok ? "PASS" : "FAILED");

    CapabilityTable *kernel_caps = process_capabilities(kernel);
    bool kernel_caps_ok = kernel_caps != 0;
    u32 kernel_caps_before = kernel_caps ? capability_table_count(kernel_caps) : 0U;
    terminal_write("  KERNEL CAP TABLE: ");
    terminal_writeln(kernel_caps_ok ? "PASS" : "FAILED");
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(kernel_caps_before);
    terminal_putchar('\n');

    /* Kernel process may never be destroyed. */
    bool kernel_destroy_rejected = kernel_ok && !process_destroy(kernel);
    terminal_write("  KERNEL DESTROY REJECTED: ");
    terminal_writeln(kernel_destroy_rejected ? "PASS" : "FAILED");

    Process process;
    bool created = process_create(&process);
    terminal_write("  CREATE USER PROCESS: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) return;

    AddressSpace *space = process_address_space(&process);
    CapabilityTable *caps = process_capabilities(&process);
    bool metadata_ok = process.initialized && process.id != 0ULL && process.id != kernel->id &&
        !process.kernel && process.owns_address_space &&
        process.address_space == &process.owned_address_space;

    terminal_write("  METADATA: ");
    terminal_writeln(metadata_ok ? "PASS" : "FAILED");

    bool address_space_ok = space && !space->kernel && space->id != 0ULL &&
        address_space_cr3(space) != 0ULL &&
        address_space_cr3(space) != address_space_cr3(address_space_kernel());

    terminal_write("  USER ADDRESS SPACE: ");
    terminal_writeln(address_space_ok ? "PASS" : "FAILED");

    bool caps_empty = caps && capability_table_count(caps) == 0U;
    Thread owned_thread;
    bool owned_thread_created = caps_empty && thread_create(&owned_thread, &process);
    terminal_write("  CREATE OWNED THREAD: ");
    terminal_writeln(owned_thread_created ? "PASS" : "FAILED");

    bool thread_owner_ok = owned_thread_created && owned_thread.process == &process &&
        process_thread_count(&process) == 1ULL;

    terminal_write("  THREAD OWNER: ");
    terminal_writeln(thread_owner_ok ? "PASS" : "FAILED");

    bool live_thread_destroy_rejected = thread_owner_ok && !process_destroy(&process);
    terminal_write("  LIVE-THREAD DESTROY REJECTED: ");
    terminal_writeln(live_thread_destroy_rejected ? "PASS" : "FAILED");

    bool owned_thread_destroyed = live_thread_destroy_rejected && thread_destroy(&owned_thread);
    terminal_write("  DESTROY OWNED THREAD: ");
    terminal_writeln(owned_thread_destroyed ? "PASS" : "FAILED");

    bool thread_count_zero = owned_thread_destroyed && process_thread_count(&process) == 0ULL;
    terminal_write("  THREAD COUNT RESTORED: ");
    terminal_writeln(thread_count_zero ? "PASS" : "FAILED");
    terminal_write("  CAP TABLE EMPTY: ");
    terminal_writeln(caps_empty ? "PASS" : "FAILED");

    /* Give the process authority over its own AddressSpace as a test capability. */
    CapabilityHandle handle = CAPABILITY_INVALID_HANDLE;

    bool cap_inserted = thread_count_zero &&
        capability_insert(caps, space, CAPABILITY_TYPE_ADDRESS_SPACE,
            CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_MANAGE, &handle);

    terminal_write("  INSERT ADDRESS-SPACE CAP: ");
    terminal_writeln(cap_inserted ? "PASS" : "FAILED");
    terminal_write("  CAP HANDLE: ");
    terminal_write_hex(handle);
    terminal_putchar('\n');

    void *resolved = 0;

    bool cap_lookup = cap_inserted &&
        capability_lookup_rights(caps, handle, CAPABILITY_TYPE_ADDRESS_SPACE, CAPABILITY_RIGHT_MANAGE, &resolved) &&
        resolved == space;

    terminal_write("  CAP LOOKUP: ");
    terminal_writeln(cap_lookup ? "PASS" : "FAILED");

    /* A process with outstanding authority may not be destroyed. */
    u64 cr3_before_rejected_destroy = address_space_cr3(space);
    bool live_cap_destroy_rejected = cap_inserted && !process_destroy(&process);
    terminal_write("  LIVE-CAP DESTROY REJECTED: ");
    terminal_writeln(live_cap_destroy_rejected ? "PASS" : "FAILED");

    bool preserved = live_cap_destroy_rejected && process.initialized &&
        process_address_space(&process) == space &&
        address_space_cr3(space) == cr3_before_rejected_destroy && capability_table_count(caps) == 1U;

    terminal_write("  PROCESS PRESERVED: ");
    terminal_writeln(preserved ? "PASS" : "FAILED");

    bool revoked = preserved && capability_revoke(caps, handle);
    terminal_write("  REVOKE CAP: ");
    terminal_writeln(revoked ? "PASS" : "FAILED");

    resolved = 0;

    bool stale_rejected = revoked && !capability_lookup(caps, handle, CAPABILITY_TYPE_ADDRESS_SPACE, &resolved);
    terminal_write("  STALE CAP REJECTED: ");
    terminal_writeln(stale_rejected ? "PASS" : "FAILED");

    bool empty_before_destroy = revoked && capability_table_count(caps) == 0U;
    terminal_write("  CAP TABLE EMPTY AGAIN: ");
    terminal_writeln(empty_before_destroy ? "PASS" : "FAILED");

    bool destroyed = empty_before_destroy && process_destroy(&process);
    terminal_write("  DESTROY USER PROCESS: ");
    terminal_writeln(destroyed ? "PASS" : "FAILED");

    bool cleared = destroyed && !process.initialized && process.id == 0ULL && process.address_space == 0;
    terminal_write("  METADATA CLEARED: ");
    terminal_writeln(cleared ? "PASS" : "FAILED");

    bool kernel_caps_stable = kernel_caps_ok && capability_table_count(kernel_caps) == kernel_caps_before;
    terminal_write("  KERNEL CAPS STABLE: ");
    terminal_writeln(kernel_caps_stable ? "PASS" : "FAILED");

    bool double_destroy_rejected = destroyed && !process_destroy(&process);
    terminal_write("  DOUBLE DESTROY REJECTED: ");
    terminal_writeln(double_destroy_rejected ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = (kernel_ok && kernel_caps_ok && kernel_destroy_rejected && created && metadata_ok &&
        address_space_ok && caps_empty && cap_inserted && cap_lookup && live_cap_destroy_rejected &&
        preserved && revoked && stale_rejected && empty_before_destroy && destroyed && cleared &&
        double_destroy_rejected && frames_restored && owned_thread_created && thread_owner_ok &&
        live_thread_destroy_rejected && owned_thread_destroyed && thread_count_zero && kernel_caps_stable);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PROCESS TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_threadtest(void) {
    terminal_writeln("THREAD TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *current = thread_current();
    bool bootstrap_ok = current && current->id == 1 && current->state == THREAD_STATE_RUNNING &&
        current->process == process_kernel() &&
        process_address_space(current->process) == address_space_kernel() &&
        current->kernel_stack_top == gdt_rsp0() && !current->owns_kernel_stack;

    terminal_write("  BOOTSTRAP THREAD: ");
    terminal_writeln(bootstrap_ok ? "PASS" : "FAILED");

    Process process;
    bool process_result = process_create(&process);
    terminal_write("  CREATE USER PROCESS: ");
    terminal_writeln(process_result ? "PASS" : "FAILED");

    if (!process_result) return;

    AddressSpace *space = process_address_space(&process);
    terminal_write("  GET ADDRESS SPACE: ");
    terminal_writeln(space ? "PASS" : "FAILED");
    if (!space) {
        (void)process_destroy(&process);
        return;
    }

    Thread thread;
    bool created = thread_create(&thread, &process);
    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) {
        (void)process_destroy(&process);
        return;
    }

    bool space_ok = space != 0;
    terminal_write("  ID: ");
    terminal_write_u64(thread.id);
    terminal_write("  STACK PHYS: ");
    terminal_write_hex(thread.kernel_stack_physical);
    terminal_putchar('\n');
    terminal_write("  STACK BASE: ");
    terminal_write_hex(thread.kernel_stack_base);
    terminal_write("  TOP: ");
    terminal_write_hex(thread.kernel_stack_top);
    terminal_putchar('\n');

    bool metadata_ok = (thread.id != current->id && thread.process == &process &&
        process_thread_count(&process) == 1ULL && thread.state == THREAD_STATE_READY &&
        thread.kernel_stack_size == THREAD_KERNEL_STACK_SIZE && thread.owns_kernel_stack);

    terminal_write("  METADATA: ");
    terminal_writeln(metadata_ok ? "PASS" : "FAILED");

    /* The stack is reached through the shared supervisor-only physical direct map. */
    frame_t first_frame = FRAME_INVALID;
    vm_flags_t first_flags = 0;

    bool first_mapped =
        address_space_query_page(space, thread.kernel_stack_base, &first_frame, &first_flags) &&
        first_frame == phys_to_frame(thread.kernel_stack_physical) && (first_flags & VM_WRITE) &&
        !(first_flags & VM_USER);

    u64 last_virtual = thread.kernel_stack_top - FRAME_SIZE;
    u64 last_physical = thread.kernel_stack_physical + thread.kernel_stack_size - FRAME_SIZE;
    frame_t last_frame = FRAME_INVALID;
    vm_flags_t last_flags = 0;

    bool last_mapped = address_space_query_page(space, last_virtual, &last_frame, &last_flags) &&
        last_frame == phys_to_frame(last_physical) && (last_flags & VM_WRITE) && !(last_flags & VM_USER);
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

    /*
     * thread_activate() does not switch the live kernel RSP, so keep IRQs disabled while this structural activation
     * test is in progress.
     */
    interrupts_disable();

    bool activated = thread_activate(&thread);
    bool current_switched = activated && thread_current() == &thread;
    bool state_switched = activated && thread.state == THREAD_STATE_RUNNING && current->state == THREAD_STATE_READY;
    bool cr3_switched = activated && (arch_read_cr3() & ~0xFFFULL) == address_space_cr3(space);
    bool rsp0_switched = activated && gdt_rsp0() == thread.kernel_stack_top;
    bool restored = activated && thread_activate(current);
    bool current_restored = restored && thread_current() == current &&
        current->state == THREAD_STATE_RUNNING && thread.state == THREAD_STATE_READY;
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
    bool detached = destroyed && process_thread_count(&process) == 0ULL;
    bool process_destroyed = detached && process_destroy(&process);
    PmmStats after = pmm_stats();
    bool count_restored = before.free_pages == after.free_pages;
    terminal_write("  DESTROY: ");
    terminal_writeln(destroyed ? "PASS" : "FAILED");
    terminal_write("  THREAD DETACHED: ");
    terminal_writeln(detached ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROYED: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(count_restored ? "PASS" : "FAILED");

    bool pass = (bootstrap_ok && space_ok && created && metadata_ok && stack_mapped && stack_rw &&
        activated && current_switched && state_switched && cr3_switched && rsp0_switched && restored &&
        current_restored && cr3_restored && rsp0_restored && destroyed && count_restored &&
        process_result && detached && process_destroyed);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("THREAD TEST: ");
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
    for (; ;) {
        if (!scheduler_yield()) {
            g_schedtest_failed = true;
            cpu_halt_forever();
        }
    }
}

static void command_schedtest(void) {
    terminal_writeln("SCHEDULER TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN QUEUED: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread a;
    Thread b;
    Process *kernel_process = process_kernel();

    if (!kernel_process) return;

    bool created_a = thread_create(&a, kernel_process);
    bool created_b = created_a && thread_create(&b, kernel_process);
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
    terminal_write("  A COUNT: ");
    terminal_write_u64(g_schedtest_a_count);
    terminal_putchar('\n');
    terminal_write("  B COUNT: ");
    terminal_write_u64(g_schedtest_b_count);
    terminal_putchar('\n');
    terminal_write("  CURRENT RESTORED: ");
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
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = (main_ok && created_a && created_b && prepared_a && prepared_b && added_a && added_b &&
        queue_three && yielded && !g_schedtest_failed && counts_ok && current_restored && children_ready &&
        removed_a && removed_b && queue_restored && destroyed_a && destroyed_b && frames_restored);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("SCHEDULER TEST: ");
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
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread child;
    bool created = thread_create(&child, process_kernel());
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
    bool child_dead = child.state == THREAD_STATE_DEAD && !child.on_run_queue &&
        !child.interrupt_context_ready && !child.interrupt_rsp;
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

    bool pass = main_ok && created && prepared && queued && queue_two && yielded && worker_started &&
        current_restored && child_dead && queue_restored && reaped && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("THREAD EXIT TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_syscalltest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    terminal_writeln("RING3 SYSCALL TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS CREATE: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        return;
    }

    Thread user;
    bool thread_created = thread_create(&user, &process);
    terminal_write("  USER THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));
    u64 *stack_data = (u64 *)phys_to_virt(frame_to_phys(stack_frame));
    bool direct_ok = code && stack_data;
    terminal_write("  PHYSMAP ACCESS: ");
    terminal_writeln(direct_ok ? "PASS" : "FAILED");

    if (!direct_ok) {
        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, user_code, &ignored);

        (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    k_memset(code, 0, (usize)VM_PAGE_SIZE);
    k_memset(stack_data, 0, (usize)VM_PAGE_SIZE);

    u64 expected_thread_id = user.id;

    /*
     * Ring3 program:
     *
     *   mov eax, SYSCALL_THREAD_ID
     *   int 0x80
     *
     *   mov rbx, user_stack
     *   mov [rbx], rax
     *
     *   mov eax, SYSCALL_THREAD_EXIT
     *   int 0x80
     *
     *   ud2
     *
     * The UD2 is now only a tripwire.
     */

    u8 *p = code;

    /* mov eax, SYSCALL_THREAD_ID */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_THREAD_ID);

    /* int 0x80 */
    *p++ = 0xCD;
    *p++ = 0x80;

    /* mov rbx, user_stack */
    *p++ = 0x48;
    *p++ = 0xBB;

    shell_emit_u64_le(&p, user_stack);

    /* mov [rbx], rax */
    *p++ = 0x48;
    *p++ = 0x89;
    *p++ = 0x03;

    /* mov eax, SYSCALL_THREAD_EXIT */
    *p++ = 0xB8;

    shell_emit_u32_le(&p, (u32)SYSCALL_THREAD_EXIT);

    /* int 0x80 */
    *p++ = 0xCD;
    *p++ = 0x80;

    /* Must never execute if THREAD_EXIT successfully switches back to main. */
    *p++ = 0x0F;
    *p++ = 0x0B;

    bool prepared = thread_prepare_user(&user, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&user);

        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, user_code, &ignored);

        (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)process_destroy(&process);
        return;
    }

    bool queued = scheduler_add(&user);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&user);

        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, user_code, &ignored);

        (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)process_destroy(&process);
        return;
    }

    interrupt_clear_user_fault();

    /*
     * main -> Ring3 user
     *
     * SYSCALL_THREAD_EXIT returns a completely
     * different InterruptFrame: main's saved
     * scheduler frame.
     */
    interrupts_disable();

    bool yielded = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);

    bool shell_restored = yielded && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1;

    bool user_dead = user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    bool clean_exit = shell_restored && user_dead && !fault_captured;
    u64 returned_thread_id = stack_data[0];
    bool thread_id_ok = returned_thread_id == expected_thread_id;
    terminal_write("  SHELL RESTORED: ");
    terminal_writeln(shell_restored ? "PASS" : "FAILED");
    terminal_write("  THREAD ID EXPECTED: ");
    terminal_write_u64(expected_thread_id);
    terminal_putchar('\n');
    terminal_write("  THREAD ID RETURNED: ");
    terminal_write_u64(returned_thread_id);
    terminal_putchar('\n');
    terminal_write("  THREAD ID SYSCALL: ");
    terminal_writeln(thread_id_ok ? "PASS" : "FAILED");
    terminal_write("  THREAD EXIT SYSCALL: ");
    terminal_writeln(clean_exit ? "PASS" : "FAILED");
    terminal_write("  NO USER FAULT: ");
    terminal_writeln(!fault_captured ? "PASS" : "FAILED");

    /*
     * If something failed before the user exited
     * but we're safely back on main, remove it
     * from the run queue before destroying it.
     */
    if (user.on_run_queue && thread_current() != &user) {
        interrupts_disable();
        (void)scheduler_remove(&user);
        interrupts_enable();
    }

    bool reaped = thread_current() != &user && !user.on_run_queue && thread_destroy(&user);
    terminal_write("  USER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool code_freed = code_unmapped && frame_free(code_frame);
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool process_destroyed = reaped && process_destroy(&process);
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && process_created && space_ok && thread_created && frames_ok && mappings_ok &&
        direct_ok && prepared && queued && yielded && shell_restored && thread_id_ok && clean_exit &&
        !fault_captured && reaped && code_unmapped && stack_unmapped && code_freed && stack_freed &&
        process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("RING3 SYSCALL TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_userisotest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    terminal_writeln("USER FAULT ISOLATION TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_created = space != 0;
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_created ? "PASS" : "FAILED");

    if (!space_created) {
        (void)process_destroy(&process);
        return;
    }

    Thread thread;
    bool thread_created = thread_create(&thread, &process);
    terminal_write("  THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));

    if (!code) {
        terminal_writeln("  PHYSMAP: FAILED");
        return;
    }

    /* UD2 */
    code[0] = 0x0F;
    code[1] = 0x0B;

    bool prepared = thread_prepare_user(&thread, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    u64 user_thread_id = thread.id;
    bool queued = scheduler_add(&thread);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&thread);
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)process_destroy(&process);
        return;
    }

    interrupt_clear_user_fault();

    /*
     * main -> user kernel trampoline -> Ring3
     *
     * UD2 enters the user thread's RSP0.
     * The exception path kills that thread and
     * one-way enters this saved main context.
     */
    interrupts_disable();

    bool yielded = scheduler_yield();
    interrupts_enable();

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);
    bool current_restored = yielded && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool user_dead = thread.state == THREAD_STATE_DEAD && !thread.on_run_queue &&
        !thread.interrupt_context_ready && !thread.interrupt_rsp;
    bool fault_ok = fault_captured && fault.thread_id == user_thread_id && fault.vector == 6ULL &&
        fault.rip == user_code && fault.user_rsp == user_stack_top && fault.user_ss == 0x1BULL;
    bool queue_restored = scheduler_thread_count() == 1;
    terminal_write("  SHELL RESUMED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  USER DEAD: ");
    terminal_writeln(user_dead ? "PASS" : "FAILED");
    terminal_write("  FAULT CAPTURED: ");
    terminal_writeln(fault_ok ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    /* Executing on the bootstrap stack again, so dead kernel stack is safe to release. */
    bool reaped = thread_destroy(&thread);
    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool code_freed = frame_free(code_frame);
    bool stack_freed = frame_free(stack_frame);
    bool process_destroyed = process_destroy(&process);
    terminal_write("  PROCESS DESTROYED: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  THREAD REAPED: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && space_created && thread_created && frames_ok && mappings_ok && prepared &&
        queued && yielded && current_restored && user_dead && fault_ok && queue_restored && reaped &&
        code_unmapped && stack_unmapped && code_freed && stack_freed && frames_restored &&
        process_created && process_destroyed;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER FAULT ISOLATION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_userpftest(void) {
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;

    /* Deliberately leave this address   unmapped. */
    const u64 fault_address = ADDRESS_SPACE_USER_BASE + 0x00400000ULL;
    terminal_writeln("USER PAGE FAULT ISOLATION TEST:");
    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        return;
    }

    Thread thread;
    bool thread_created = thread_create(&thread, &process);
    terminal_write("  THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;
        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    /* Prove that the intended fault address really is absent before entering Ring3. */
    frame_t unexpected_frame = FRAME_INVALID;
    bool fault_unmapped = !address_space_query_page(space, fault_address, &unexpected_frame, 0);
    terminal_write("  FAULT ADDRESS UNMAPPED: ");
    terminal_writeln(fault_unmapped ? "PASS" : "FAILED");

    if (!fault_unmapped) {
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));

    if (!code) {
        terminal_writeln("  PHYSMAP: FAILED");
        return;
    }

    /*
     * mov rax, moffs64
     *
     *   48 A1 <64-bit address>
     *
     * The memory operand is deliberately
     * unmapped, so this instruction must #PF.
     *
     * UD2 follows as a guard and must never
     * execute.
     */
    code[0] = 0x48;
    code[1] = 0xA1;
    for (u32 i = 0; i < 8U; ++i) code[2U + i] = (u8)(fault_address >> (i * 8U));
    code[10] = 0x0F;
    code[11] = 0x0B;

    bool prepared = thread_prepare_user(&thread, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        return;
    }

    u64 user_thread_id = thread.id;
    bool queued = scheduler_add(&thread);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&thread);
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)process_destroy(&process);
        return;
    }

    interrupt_clear_user_fault();

    /*
     * main -> user -> #PF
     *
     * the recoverable exception path kills
     * the user thread and restores main.
     */
    interrupts_disable();

    bool yielded = scheduler_yield();
    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);
    bool current_restored = yielded && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool user_dead = thread.state == THREAD_STATE_DEAD && !thread.on_run_queue &&
        !thread.interrupt_context_ready && !thread.interrupt_rsp;
    bool fault_identity_ok = fault_captured && fault.thread_id == user_thread_id && fault.vector == 14ULL &&
        fault.rip == user_code && fault.cr2 == fault_address && fault.user_rsp == user_stack_top &&
        fault.user_ss == 0x1BULL;

    /*
     * Lower page-fault error-code bits:
     *
     * bit 0 P   = 0  not present
     * bit 1 W/R = 0  read
     * bit 2 U/S = 1  user
     */
    bool error_bits_ok = fault_captured && (fault.error_code & 0x7ULL) == 0x4ULL;
    bool queue_restored = scheduler_thread_count() == 1;
    terminal_write("  SHELL RESUMED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  USER DEAD: ");
    terminal_writeln(user_dead ? "PASS" : "FAILED");
    terminal_write("  PAGE FAULT CAPTURED: ");
    terminal_writeln(fault_identity_ok ? "PASS" : "FAILED");
    terminal_write("  CR2: ");
    terminal_write_hex(fault.cr2);
    terminal_putchar('\n');
    terminal_write("  ERROR CODE: ");
    terminal_write_hex(fault.error_code);
    terminal_putchar('\n');
    terminal_write("  PF ERROR BITS: ");
    terminal_writeln(error_bits_ok ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    bool reaped = thread_destroy(&thread);
    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool code_freed = frame_free(code_frame);
    bool stack_freed = frame_free(stack_frame);
    bool process_destroyed = process_destroy(&process);
    terminal_write("  PROCESS DESTROYED: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  THREAD REAPED: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && process_created && space_ok && thread_created && frames_ok && mappings_ok &&
        fault_unmapped && prepared && queued && yielded && current_restored && user_dead &&
        fault_identity_ok && error_bits_ok && queue_restored && reaped && code_unmapped && stack_unmapped &&
        code_freed && stack_freed && process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER PAGE FAULT ISOLATION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_timer(void) {
    terminal_writeln("TIMER:");
    terminal_write("  INITIALIZED: ");
    terminal_writeln(timer_initialized() ? "YES" : "NO");
    terminal_write("  SOURCE: ");
    terminal_writeln(timer_initialized() ? "8254 PIT / IRQ0" : "NONE");
    terminal_write("  FREQUENCY: ");
    terminal_write_u64(timer_frequency());
    terminal_writeln(" HZ");
    terminal_write("  TICKS: ");
    terminal_write_u64(timer_ticks());
    terminal_putchar('\n');
    terminal_write("  VECTOR 0x20 COUNT: ");
    terminal_write_u64(interrupt_count(0x20));
    terminal_putchar('\n');
}

static void command_timertest(void) {
    terminal_writeln("PIT TIMER TEST:");
    if (!timer_initialized()) {
        terminal_writeln("  INITIALIZED: FAILED");
        return;
    }

    /* Snapshot both IRQ0 counters atomically with respect to IRQ delivery. */
    interrupts_disable();

    u64 tick_before = timer_ticks();
    u64 irq_before = interrupt_count(0x20);

    interrupts_enable();

    /* Wait for at least one tick, but keep a finite timeout so a broken IRQ0 doesn't hang the shell forever. */
    u64 spins = 0;

    while (timer_ticks() == tick_before && spins < 50000000ULL) {
        arch_pause();
        ++spins;
    }

    interrupts_disable();
    u64 tick_after = timer_ticks();
    u64 irq_after = interrupt_count(0x20);
    interrupts_enable();
    bool tick_advanced = tick_after > tick_before;
    bool irq_advanced = irq_after > irq_before;
    terminal_write("  FREQUENCY: ");
    terminal_write_u64(timer_frequency());
    terminal_writeln(" HZ");
    terminal_write("  TICKS BEFORE: ");
    terminal_write_u64(tick_before);
    terminal_putchar('\n');
    terminal_write("  TICKS AFTER: ");
    terminal_write_u64(tick_after);
    terminal_putchar('\n');
    terminal_write("  TICK ADVANCED: ");
    terminal_writeln(tick_advanced ? "PASS" : "FAILED");
    terminal_write("  IRQ0 ADVANCED: ");
    terminal_writeln(irq_advanced ? "PASS" : "FAILED");

    bool pass = tick_advanced && irq_advanced;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PIT TIMER TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void blocktest_worker(void *argument) {
    (void)argument;
    g_blocktest_started = true;

    /* This call does not return until main wakes it and schedules it again. */
    if (!scheduler_block_current()) cpu_halt_forever();
    g_blocktest_resumed = true;

    /* Prove that a resumed blocked thread can subsequently terminate normally. */
    scheduler_exit_current();
}

static void command_blocktest(void) {
    terminal_writeln("THREAD BLOCK/WAKE TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread worker;
    bool created = thread_create(&worker, process_kernel());
    terminal_write("  CREATE: ");
    terminal_writeln(created ? "PASS" : "FAILED");

    if (!created) return;

    bool prepared = thread_prepare_kernel(&worker, blocktest_worker, 0);
    terminal_write("  PREPARE: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&worker);
        return;
    }

    bool queued = scheduler_add(&worker);
    terminal_write("  QUEUE: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&worker);
        return;
    }

    g_blocktest_started = false;
    g_blocktest_resumed = false;

    /*
     * main -> worker
     *
     * Worker blocks itself, which switches
     * straight back to this saved main context.
     */
    interrupts_disable();

    bool first_yield = scheduler_yield();
    interrupts_enable();

    bool current_after_block = first_yield && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING;
    bool worker_started = g_blocktest_started;
    bool worker_blocked = worker.state == THREAD_STATE_BLOCKED && !worker.on_run_queue &&
        worker.interrupt_context_ready && worker.interrupt_rsp;
    bool not_resumed_yet = !g_blocktest_resumed;
    bool queue_one = scheduler_thread_count() == 1;
    terminal_write("  FIRST YIELD: ");
    terminal_writeln(first_yield ? "PASS" : "FAILED");
    terminal_write("  WORKER STARTED: ");
    terminal_writeln(worker_started ? "PASS" : "FAILED");
    terminal_write("  WORKER BLOCKED: ");
    terminal_writeln(worker_blocked ? "PASS" : "FAILED");
    terminal_write("  NOT RESUMED YET: ");
    terminal_writeln(not_resumed_yet ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(current_after_block ? "PASS" : "FAILED");
    terminal_write("  RUN QUEUE COUNT 1: ");
    terminal_writeln(queue_one ? "PASS" : "FAILED");

    /* If the blocking phase failed, clean up without attempting the wake phase. */
    if (!first_yield || !worker_started || !worker_blocked || !not_resumed_yet || !current_after_block || !queue_one) {
        if (worker.on_run_queue) (void)scheduler_remove(&worker);
        (void)thread_destroy(&worker);
        return;
    }

    /*
     * Wake worker while scheduling remains
     * quiescent, then yield to it.
     *
     * Worker resumes after its block call,
     * records RESUMED, exits, and restores
     * main's saved context.
     */
    interrupts_disable();

    bool woke = scheduler_wake(&worker);
    bool wake_state_ok = woke && worker.state == THREAD_STATE_READY && worker.on_run_queue &&
        scheduler_thread_count() == 2;
    bool second_yield = woke && scheduler_yield();

    interrupts_enable();

    bool worker_resumed = g_blocktest_resumed;
    bool worker_dead = worker.state == THREAD_STATE_DEAD && !worker.on_run_queue &&
        !worker.interrupt_context_ready && !worker.interrupt_rsp;
    bool main_restored = second_yield && thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool queue_restored = scheduler_thread_count() == 1;
    terminal_write("  WAKE: ");
    terminal_writeln(woke ? "PASS" : "FAILED");
    terminal_write("  WAKE -> READY: ");
    terminal_writeln(wake_state_ok ? "PASS" : "FAILED");
    terminal_write("  SECOND YIELD: ");
    terminal_writeln(second_yield ? "PASS" : "FAILED");
    terminal_write("  WORKER RESUMED: ");
    terminal_writeln(worker_resumed ? "PASS" : "FAILED");
    terminal_write("  WORKER DEAD: ");
    terminal_writeln(worker_dead ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED AGAIN: ");
    terminal_writeln(main_restored ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");

    /*
     * Normal success path leaves the worker
     * DEAD and off the run queue.
     *
     * If the second phase failed before running
     * it, it may instead still be READY.
     */
    if (worker.on_run_queue && thread_current() != &worker) (void)scheduler_remove(&worker);

    bool reaped = thread_destroy(&worker);
    terminal_write("  REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = (main_ok && created && prepared && queued && first_yield && worker_started &&
        worker_blocked && not_resumed_yet && current_after_block && queue_one && woke && wake_state_ok &&
        second_yield && worker_resumed && worker_dead && main_restored && queue_restored && reaped &&
        frames_restored);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("THREAD BLOCK/WAKE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void preempttest_worker(void *argument) {
    volatile u64 *counter = (volatile u64 *)argument;

    if (!counter) cpu_halt_forever();

    /*
     * Intentionally:
     *
     *   no scheduler_yield()
     *   no scheduler_block_current()
     *   no explicit context switch
     */
    for (; ;) {
        ++(*counter);
    }
}

static void command_preempttest(void) {
    terminal_writeln("TIMER PREEMPTION TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');
    Thread *main_thread = thread_current();
    bool main_ok = timer_initialized() && main_thread && main_thread->state == THREAD_STATE_RUNNING &&
        main_thread->on_run_queue && scheduler_thread_count() == 1;

    terminal_write("  TIMER/MAIN: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread a;
    Thread b;
    bool created_a = thread_create(&a, process_kernel());
    bool created_b = created_a && thread_create(&b, process_kernel());
    terminal_write("  CREATE A: ");
    terminal_writeln(created_a ? "PASS" : "FAILED");
    terminal_write("  CREATE B: ");
    terminal_writeln(created_b ? "PASS" : "FAILED");

    if (!created_a || !created_b) {
        if (created_a) (void)thread_destroy(&a);
        return;
    }

    bool prepared_a = thread_prepare_kernel(&a, preempttest_worker, (void *)&g_preempttest_a_count);
    bool prepared_b = thread_prepare_kernel(&b, preempttest_worker, (void *)&g_preempttest_b_count);
    terminal_write("  PREPARE A: ");
    terminal_writeln(prepared_a ? "PASS" : "FAILED");
    terminal_write("  PREPARE B: ");
    terminal_writeln(prepared_b ? "PASS" : "FAILED");

    if (!prepared_a || !prepared_b) {
        (void)thread_destroy(&a);
        (void)thread_destroy(&b);
        return;
    }

    bool queued_a = scheduler_add(&a);
    bool queued_b = queued_a && scheduler_add(&b);
    terminal_write("  QUEUE A: ");
    terminal_writeln(queued_a ? "PASS" : "FAILED");
    terminal_write("  QUEUE B: ");
    terminal_writeln(queued_b ? "PASS" : "FAILED");

    if (!queued_a || !queued_b) {
        if (queued_a) (void)scheduler_remove(&a);
        (void)thread_destroy(&a);
        (void)thread_destroy(&b);
        return;
    }

    InterruptFrame *a_frame = (InterruptFrame *)(u64) a.interrupt_rsp;
    InterruptFrame *b_frame = (InterruptFrame *)(u64) b.interrupt_rsp;
    terminal_write("  A FRAME: ");
    terminal_write_hex(a.interrupt_rsp);
    terminal_putchar('\n');
    terminal_write("  A RIP: ");
    terminal_write_hex(a_frame ? a_frame->rip : 0);
    terminal_putchar('\n');
    terminal_write("  A CS: ");
    terminal_write_hex(a_frame ? a_frame->cs : 0);
    terminal_putchar('\n');
    terminal_write("  A RFLAGS: ");
    terminal_write_hex(a_frame ? a_frame->rflags : 0);
    terminal_putchar('\n');
    u64 a_expected_rsp = a.kernel_stack_top - sizeof(u64);
    terminal_write("  A EXPECTED RSP: ");
    terminal_write_hex(a_expected_rsp);
    terminal_putchar('\n');
    terminal_write("  A DUMMY RETURN: ");
    terminal_write_hex(*(const u64 *)(u64) a_expected_rsp);
    terminal_putchar('\n');
    terminal_write("  B FRAME: ");
    terminal_write_hex(b.interrupt_rsp);
    terminal_putchar('\n');
    terminal_write("  B RIP: ");
    terminal_write_hex(b_frame ? b_frame->rip : 0);
    terminal_putchar('\n');
    terminal_write("  B CS: ");
    terminal_write_hex(b_frame ? b_frame->cs : 0);
    terminal_putchar('\n');
    terminal_write("  B RFLAGS: ");
    terminal_write_hex(b_frame ? b_frame->rflags : 0);
    terminal_putchar('\n');

    g_preempttest_a_count = 0;
    g_preempttest_b_count = 0;

    u64 preempt_before = scheduler_preemption_count();
    u64 start_tick = timer_ticks();
    u64 timeout_ticks = timer_frequency() ? (u64)timer_frequency() * 2ULL : 200ULL;

    /*
     * Queue is now:
     *
     *   main -> A -> B -> main
     *
     * A and B already have synthetic IRET
     * frames. Main receives a real frame on
     * the first timer interrupt.
     */
    interrupts_disable();

    bool enabled = scheduler_preemption_enable();
    if (!enabled) {
        bool removed_a = scheduler_remove(&a);
        bool removed_b = scheduler_remove(&b);
        bool destroyed_a = thread_destroy(&a);
        bool destroyed_b = thread_destroy(&b);

        interrupts_enable();
        terminal_writeln("  ENABLE PREEMPTION: FAILED");
        terminal_write("  CLEANUP: ");
        terminal_writeln(removed_a && removed_b && destroyed_a && destroyed_b ? "PASS" : "FAILED");
        return;
    }
    interrupts_enable();

    /*
     * From here until we disable preemption:
     *
     * DO NOT print.
     * DO NOT yield.
     * DO NOT block.
     *
     * A and B only increment volatile counters.
     */
    bool completed = false;
    while ((timer_ticks() - start_tick) < timeout_ticks) {
        u64 switches = scheduler_preemption_count() - preempt_before;
        if (g_preempttest_a_count > 0 && g_preempttest_b_count > 0 && switches >= 6ULL) {
            completed = true;
            break;
        }
        arch_pause();
    }

    /* We can only reach this code while main is the currently scheduled thread. */
    interrupts_disable();
    bool disabled = scheduler_preemption_disable();
    u64 preempt_after = scheduler_preemption_count();
    u64 a_count = g_preempttest_a_count;
    u64 b_count = g_preempttest_b_count;
    u64 switches = preempt_after - preempt_before;
    bool main_restored = thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;

    /* Because main is RUNNING, both workers must currently be suspended READY threads with saved interrupt frames. */
    bool workers_suspended = a.state == THREAD_STATE_READY && b.state == THREAD_STATE_READY &&
        a.on_run_queue && b.on_run_queue && a.interrupt_context_ready && b.interrupt_context_ready;
    bool removed_a = disabled && scheduler_remove(&a);
    bool removed_b = disabled && scheduler_remove(&b);
    bool queue_restored = scheduler_thread_count() == 1;
    bool destroyed_a = removed_a && thread_destroy(&a);
    bool destroyed_b = removed_b && thread_destroy(&b);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;

    interrupts_enable();

    /* Printing is safe again: preemption is off and the worker stacks have been reclaimed. */
    terminal_write("  ENABLE PREEMPTION: ");
    terminal_writeln(enabled ? "PASS" : "FAILED");
    terminal_write("  DISABLE PREEMPTION: ");
    terminal_writeln(disabled ? "PASS" : "FAILED");
    terminal_write("  PREEMPTIONS: ");
    terminal_write_u64(switches);
    terminal_putchar('\n');
    terminal_write("  A COUNT: ");
    terminal_write_u64(a_count);
    terminal_putchar('\n');
    terminal_write("  B COUNT: ");
    terminal_write_u64(b_count);
    terminal_putchar('\n');
    terminal_write("  A RAN WITHOUT YIELD: ");
    terminal_writeln(a_count > 0 ? "PASS" : "FAILED");
    terminal_write("  B RAN WITHOUT YIELD: ");
    terminal_writeln(b_count > 0 ? "PASS" : "FAILED");
    terminal_write("  MULTIPLE SWITCHES: ");
    terminal_writeln(switches >= 6ULL ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(main_restored ? "PASS" : "FAILED");
    terminal_write("  WORKERS SUSPENDED: ");
    terminal_writeln(workers_suspended ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");
    terminal_write("  DESTROY A: ");
    terminal_writeln(destroyed_a ? "PASS" : "FAILED");
    terminal_write("  DESTROY B: ");
    terminal_writeln(destroyed_b ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = enabled && disabled && completed && a_count > 0 && b_count > 0 && switches >= 6ULL &&
        main_restored && workers_suspended && removed_a && removed_b && queue_restored && destroyed_a &&
        destroyed_b && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("TIMER PREEMPTION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_userpreempttest(void) {
    /*
     * Layout:
     *
     *   +0x0000  user code
     *   +0x1000  user stack
     *   +0x3000  user counter
     *
     * The counter is deliberately separate from
     * the top of the user stack.
     */
    const u64 user_code = ADDRESS_SPACE_USER_BASE;
    const u64 user_stack = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 user_stack_top = user_stack + VM_PAGE_SIZE;
    const u64 user_counter = ADDRESS_SPACE_USER_BASE + 3ULL * VM_PAGE_SIZE;
    const u64 user_code_size = 15ULL;
    terminal_writeln("USER TIMER PREEMPTION TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = (timer_initialized() && main_thread && main_thread->id &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled());

    terminal_write("  TIMER/MAIN: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        return;
    }

    Thread user;
    bool thread_created = thread_create(&user, &process);
    terminal_write("  THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        (void)process_destroy(&process);
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();
    frame_t counter_frame = frame_alloc();
    bool frames_ok = code_frame != FRAME_INVALID && stack_frame != FRAME_INVALID && counter_frame != FRAME_INVALID;
    terminal_write("  USER FRAMES: ");
    terminal_writeln(frames_ok ? "PASS" : "FAILED");

    if (!frames_ok) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);
        if (counter_frame != FRAME_INVALID) (void)frame_free(counter_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);
    bool counter_mapped = address_space_map_page(space, user_counter, counter_frame, VM_WRITE);
    bool mappings_ok = code_mapped && stack_mapped && counter_mapped;
    terminal_write("  USER MAPPINGS: ");
    terminal_writeln(mappings_ok ? "PASS" : "FAILED");

    if (!mappings_ok) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);
        if (counter_mapped) (void)address_space_unmap_page(space, user_counter, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(counter_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    u8 *code = (u8 *)phys_to_virt(frame_to_phys(code_frame));
    volatile u64 *counter = (volatile u64 *)phys_to_virt(frame_to_phys(counter_frame));
    bool direct_ok = code && counter;
    terminal_write("  PHYSMAP ACCESS: ");
    terminal_writeln(direct_ok ? "PASS" : "FAILED");

    if (!direct_ok) {
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)address_space_unmap_page(space, user_counter, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(counter_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    k_memset(code, 0, (usize)VM_PAGE_SIZE);
    *(volatile u64 *)counter = 0;

    /*
     * Ring3 program:
     *
     *   mov rax, user_counter
     *
     * loop:
     *   inc qword [rax]
     *   jmp loop
     *
     * Encoding:
     *
     *   48 B8 imm64       mov rax, imm64
     *   48 FF 00          inc qword [rax]
     *   EB FB             jmp -5
     *
     * There is deliberately:
     *
     *   no INT 0x80
     *   no INT 0x81
     *   no fault
     *   no HLT
     *   no yield
     */
    code[0] = 0x48;
    code[1] = 0xB8;
    for (u32 i = 0; i < 8U; ++i) code[2U + i] = (u8)(user_counter >> (i * 8U));
    code[10] = 0x48;
    code[11] = 0xFF;
    code[12] = 0x00;
    code[13] = 0xEB;
    code[14] = 0xFB;

    bool prepared = thread_prepare_user(&user, user_code, user_stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)address_space_unmap_page(space, user_counter, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(counter_frame);
        (void)thread_destroy(&user);
        (void)process_destroy(&process);
        return;
    }

    interrupts_disable();
    bool queued = scheduler_add(&user);
    interrupts_enable();
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&user);
        frame_t ignored = FRAME_INVALID;
        (void)address_space_unmap_page(space, user_code, &ignored);
        (void)address_space_unmap_page(space, user_stack, &ignored);
        (void)address_space_unmap_page(space, user_counter, &ignored);
        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);
        (void)frame_free(counter_frame);
        (void)process_destroy(&process);
        return;
    }

    /*
     * At this point:
     *
     *   main = RUNNING
     *   user = READY with synthetic Ring3 frame
     *
     * The first PIT interrupt must switch:
     *
     *   main -> user
     *
     * and the next PIT interrupt must enter
     * Ring0 through user's TSS.RSP0 and switch:
     *
     *   user -> main
     */
    interrupts_disable();

    u64 preempt_before = scheduler_preemption_count();
    u64 irq_before = interrupt_count(0x20);
    u64 start_tick = timer_ticks();
    bool enabled = scheduler_preemption_enable();

    if (!enabled) {
        bool removed = scheduler_remove(&user);
        bool destroyed = removed && thread_destroy(&user);
        interrupts_enable();
        terminal_writeln("  ENABLE PREEMPTION: FAILED");
        frame_t old_code = FRAME_INVALID;
        frame_t old_stack = FRAME_INVALID;
        frame_t old_counter = FRAME_INVALID;
        bool code_unmapped = address_space_unmap_page(space, user_code, &old_code);
        bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack);
        bool counter_unmapped = address_space_unmap_page(space, user_counter, &old_counter);
        if (code_unmapped) (void)frame_free(code_frame);
        if (stack_unmapped) (void)frame_free(stack_frame);
        if (counter_unmapped) (void)frame_free(counter_frame);
        (void)process_destroy(&process);
        terminal_write("  CLEANUP: ");
        terminal_writeln(
            removed && destroyed && code_unmapped && stack_unmapped && counter_unmapped ? "PASS" : "FAILED");
        return;
    }

    interrupts_enable();

    /*
     * Give the timer up to two seconds.
     *
     * While this C code is executing we are
     * necessarily on main. Whenever PIT chooses
     * the user thread this loop simply stops
     * executing until a later PIT returns main.
     */
    u64 timeout_ticks = timer_frequency() ? (u64)timer_frequency() * 2ULL : 200ULL;
    bool completed = false;

    while ((timer_ticks() - start_tick) < timeout_ticks) {
        u64 switches = scheduler_preemption_count() - preempt_before;
        u64 user_count = *counter;
        if (user_count > 0 && switches >= 4ULL) {
            completed = true;
            break;
        }
        arch_pause();
    }

    /*
     * This instruction only executes while main
     * is the current thread.
     *
     * Once CLI completes, user can no longer be
     * selected by another PIT before inspection.
     */
    interrupts_disable();

    bool disabled = scheduler_preemption_disable();
    u64 preempt_after = scheduler_preemption_count();
    u64 irq_after = interrupt_count(0x20);
    u64 tick_after = timer_ticks();
    u64 user_count = *counter;
    u64 switches = preempt_after - preempt_before;
    u64 irq_delta = irq_after - irq_before;
    u64 tick_delta = tick_after - start_tick;
    bool main_restored = thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;

    /*
     * Because main is RUNNING, the user thread
     * should now be the suspended READY thread.
     *
     * Its interrupt_rsp must point at the REAL
     * frame created when PIT interrupted Ring3,
     * not its original synthetic startup frame.
     */
    bool user_suspended = user.state == THREAD_STATE_READY && user.on_run_queue &&
        user.interrupt_context_ready && user.interrupt_rsp;
    InterruptFrame *saved = user_suspended ? (InterruptFrame *)(u64) user.interrupt_rsp : 0;
    const InterruptStackFrame *saved_user = saved ? interrupt_user_stack(saved) : 0;
    bool vector_ok = saved && saved->vector == 0x20ULL && saved->error_code == 0;
    bool privilege_ok = saved && saved->cs == GDT_USER_CODE_SELECTOR && (saved->cs & 3ULL) == 3ULL;
    bool rip_ok = saved && saved->rip >= user_code && saved->rip < user_code + user_code_size;
    bool user_stack_ok = saved_user && saved_user->rsp == user_stack_top && saved_user->ss == GDT_USER_DATA_SELECTOR;
    bool if_ok = saved && (saved->rflags & (1ULL << 9)) != 0;
    bool rax_ok = saved && saved->rax == user_counter;
    bool counter_ok = user_count > 0;
    bool switch_ok = completed && switches >= 4ULL;

    /* With exactly main + user runnable, every IRQ0 while preemption is enabled should cause one scheduler switch. */
    bool irq_switch_match = irq_delta == switches && irq_delta >= 4ULL;
    bool timer_frame_ok = vector_ok && privilege_ok && rip_ok && user_stack_ok && if_ok && rax_ok;

    /* User is suspended and preemption is now disabled, so it is safe to remove/reap it. */
    u64 saved_vector = saved ? saved->vector : 0;
    u64 saved_rip = saved ? saved->rip : 0;
    u64 saved_cs = saved ? saved->cs : 0;
    u64 saved_user_rsp = saved_user ? saved_user->rsp : 0;
    u64 saved_user_ss = saved_user ? saved_user->ss : 0;
    bool removed = disabled && user_suspended && scheduler_remove(&user);
    bool queue_restored = scheduler_thread_count() == 1;
    bool destroyed = removed && thread_destroy(&user);
    frame_t old_code = FRAME_INVALID;
    frame_t old_stack = FRAME_INVALID;
    frame_t old_counter = FRAME_INVALID;
    bool code_unmapped = address_space_unmap_page(space, user_code, &old_code) && old_code == code_frame;
    bool stack_unmapped = address_space_unmap_page(space, user_stack, &old_stack) && old_stack == stack_frame;
    bool counter_unmapped = address_space_unmap_page(space, user_counter, &old_counter) && old_counter == counter_frame;
    bool code_freed = code_unmapped && frame_free(code_frame);
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool counter_freed = counter_unmapped && frame_free(counter_frame);
    bool process_destroyed = process_destroy(&process);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    interrupts_enable();

    /* Printing starts only after preemption is off and the user kernel stack is gone. */
    terminal_write("  ENABLE PREEMPTION: ");
    terminal_writeln(enabled ? "PASS" : "FAILED");
    terminal_write("  DISABLE PREEMPTION: ");
    terminal_writeln(disabled ? "PASS" : "FAILED");
    terminal_write("  USER COUNTER: ");
    terminal_write_u64(user_count);
    terminal_putchar('\n');
    terminal_write("  USER RAN WITHOUT YIELD: ");
    terminal_writeln(counter_ok ? "PASS" : "FAILED");
    terminal_write("  TIMER TICKS: ");
    terminal_write_u64(tick_delta);
    terminal_putchar('\n');
    terminal_write("  IRQ0: ");
    terminal_write_u64(irq_delta);
    terminal_putchar('\n');
    terminal_write("  PREEMPTIONS: ");
    terminal_write_u64(switches);
    terminal_putchar('\n');
    terminal_write("  USER FRAME RESUMED: ");
    terminal_writeln(switch_ok ? "PASS" : "FAILED");
    terminal_write("  IRQ/SWITCH MATCH: ");
    terminal_writeln(irq_switch_match ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(main_restored ? "PASS" : "FAILED");
    terminal_write("  USER SUSPENDED: ");
    terminal_writeln(user_suspended ? "PASS" : "FAILED");
    terminal_write("  SAVED VECTOR: ");
    terminal_write_hex(saved ? saved_vector : 0);
    terminal_putchar('\n');
    terminal_write("  SAVED RIP: ");
    terminal_write_hex(saved ? saved_rip : 0);
    terminal_putchar('\n');
    terminal_write("  SAVED CS: ");
    terminal_write_hex(saved ? saved_cs : 0);
    terminal_putchar('\n');
    terminal_write("  SAVED USER RSP: ");
    terminal_write_hex(saved_user ? saved_user_rsp : 0);
    terminal_putchar('\n');
    terminal_write("  SAVED USER SS: ");
    terminal_write_hex(saved_user ? saved_user_ss : 0);
    terminal_putchar('\n');
    terminal_write("  RING3 TIMER FRAME: ");
    terminal_writeln(timer_frame_ok ? "PASS" : "FAILED");
    terminal_write("  REMOVE USER: ");
    terminal_writeln(removed ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");
    terminal_write("  DESTROY USER: ");
    terminal_writeln(destroyed ? "PASS" : "FAILED");
    terminal_write("  DESTROY PROCESS: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");
    terminal_write("  USER UNMAP: ");
    terminal_writeln(code_unmapped && stack_unmapped && counter_unmapped ? "PASS" : "FAILED");
    terminal_write("  USER FRAMES FREED: ");
    terminal_writeln(code_freed && stack_freed && counter_freed ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = (main_ok && process_created && space_ok && thread_created && frames_ok && mappings_ok &&
        direct_ok && prepared && queued && enabled && disabled && completed && counter_ok && switch_ok &&
        irq_switch_match && main_restored && user_suspended && timer_frame_ok && removed &&
        queue_restored && destroyed && code_unmapped && stack_unmapped && counter_unmapped && code_freed &&
        stack_freed && counter_freed && process_destroyed && frames_restored);

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER TIMER PREEMPTION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void command_userelftest(void) {
    const u64 result_address = ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE;
    const u64 stack_address = ADDRESS_SPACE_USER_BASE + 0x100000ULL;
    const u64 stack_top = stack_address + VM_PAGE_SIZE;
    const u64 expected_magic = 0x4A434F53454C4631ULL;
    terminal_writeln("USER ELF LOADER TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL;

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    VfsNode *file = vfs_resolve(vfs_root(), "/bin/elftest.elf");
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    terminal_write("  ELF FILE: ");
    terminal_writeln(file_ok ? "PASS" : "FAILED");

    if (!file_ok) return;

    terminal_write("  ELF SIZE: ");
    terminal_write_u64(file->size);
    terminal_putchar('\n');

    Process process;

    k_memset(&process, 0, sizeof(process));

    bool process_created = process_create(&process);
    terminal_write("  PROCESS CREATE: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        return;
    }

    UserElfImage image;

    k_memset(&image, 0, sizeof(image));

    bool loaded = user_elf_load(&process, file, &image);
    terminal_write("  ELF LOAD: ");
    terminal_writeln(loaded ? "PASS" : "FAILED");

    if (!loaded) {
        (void)process_destroy(&process);
        return;
    }

    bool entry_ok = image.entry == ADDRESS_SPACE_USER_BASE;
    terminal_write("  ENTRY POINT: ");
    terminal_writeln(entry_ok ? "PASS" : "FAILED");
    terminal_write("  LOAD PAGES: ");
    terminal_write_u64(image.page_count);
    terminal_putchar('\n');

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    vm_flags_t data_flags = 0;
    bool text_mapping = address_space_query_page(space, ADDRESS_SPACE_USER_BASE, &text_frame, &text_flags);
    bool data_mapping = address_space_query_page(space, result_address, &data_frame, &data_flags);
    bool text_readonly = text_mapping && !(text_flags & VM_WRITE);
    bool data_writable = data_mapping && (data_flags & VM_WRITE);
    terminal_write("  TEXT MAPPING: ");
    terminal_writeln(text_readonly ? "PASS" : "FAILED");
    terminal_write("  DATA MAPPING: ");
    terminal_writeln(data_writable ? "PASS" : "FAILED");

    u64 *result = 0;

    if (data_mapping) result = (u64 *)phys_to_virt(frame_to_phys(data_frame));

    bool result_access = result != 0;
    bool initial_zero = result_access && result[0] == 0ULL && result[1] == 0ULL;
    terminal_write("  DATA PHYSMAP: ");
    terminal_writeln(result_access ? "PASS" : "FAILED");
    terminal_write("  DATA INITIAL ZERO: ");
    terminal_writeln(initial_zero ? "PASS" : "FAILED");

    frame_t stack_frame = frame_alloc();
    bool stack_allocated = stack_frame != FRAME_INVALID;
    bool stack_mapped = stack_allocated && address_space_map_page(space, stack_address, stack_frame, VM_WRITE);
    terminal_write("  USER STACK: ");
    terminal_writeln(stack_mapped ? "PASS" : "FAILED");

    if (!stack_mapped) {
        if (stack_allocated) (void)frame_free(stack_frame);

        (void)user_elf_unload(&process, &image);

        (void)process_destroy(&process);
        return;
    }

    Thread user;

    k_memset(&user, 0, sizeof(user));

    bool thread_created = thread_create(&user, &process);
    terminal_write("  USER THREAD CREATE: ");
    terminal_writeln(thread_created ? "PASS" : "FAILED");

    if (!thread_created) {
        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, stack_address, &ignored);

        (void)frame_free(stack_frame);

        (void)user_elf_unload(&process, &image);

        (void)process_destroy(&process);
        return;
    }

    u64 expected_thread_id = user.id;
    bool prepared = thread_prepare_user(&user, image.entry, stack_top);
    terminal_write("  PREPARE USER: ");
    terminal_writeln(prepared ? "PASS" : "FAILED");

    if (!prepared) {
        (void)thread_destroy(&user);

        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, stack_address, &ignored);

        (void)frame_free(stack_frame);

        (void)user_elf_unload(&process, &image);

        (void)process_destroy(&process);
        return;
    }

    bool queued = scheduler_add(&user);
    terminal_write("  QUEUE USER: ");
    terminal_writeln(queued ? "PASS" : "FAILED");

    if (!queued) {
        (void)thread_destroy(&user);

        frame_t ignored = FRAME_INVALID;

        (void)address_space_unmap_page(space, stack_address, &ignored);

        (void)frame_free(stack_frame);

        (void)user_elf_unload(&process, &image);

        (void)process_destroy(&process);
        return;
    }

    interrupt_clear_user_fault();

    interrupts_disable();

    bool yielded = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    bool fault_captured = interrupt_last_user_fault(&fault);

    bool shell_restored = yielded && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1ULL;

    bool user_dead = user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    bool magic_ok = result_access && result[0] == expected_magic;
    bool thread_id_ok = result_access && result[1] == expected_thread_id;
    bool clean_exit = shell_restored && user_dead && !fault_captured;
    terminal_write("  SHELL RESTORED: ");
    terminal_writeln(shell_restored ? "PASS" : "FAILED");
    terminal_write("  ELF CODE EXECUTED: ");
    terminal_writeln(magic_ok ? "PASS" : "FAILED");
    terminal_write("  THREAD ID SYSCALL: ");
    terminal_writeln(thread_id_ok ? "PASS" : "FAILED");
    terminal_write("  THREAD EXIT: ");
    terminal_writeln(clean_exit ? "PASS" : "FAILED");
    terminal_write("  NO USER FAULT: ");
    terminal_writeln(!fault_captured ? "PASS" : "FAILED");

    /* Defensive cleanup if execution returned without killing the user thread. */
    if (user.on_run_queue && thread_current() != &user) {
        interrupts_disable();

        (void)scheduler_remove(&user);

        interrupts_enable();
    }

    bool reaped = thread_current() != &user && !user.on_run_queue && thread_destroy(&user);
    terminal_write("  USER REAP: ");
    terminal_writeln(reaped ? "PASS" : "FAILED");

    frame_t old_stack = FRAME_INVALID;
    bool stack_unmapped = address_space_unmap_page(space, stack_address, &old_stack) && old_stack == stack_frame;
    bool stack_freed = stack_unmapped && frame_free(stack_frame);
    bool image_unloaded = user_elf_unload(&process, &image);
    bool process_destroyed = image_unloaded && reaped && process_destroy(&process);
    terminal_write("  STACK CLEANUP: ");
    terminal_writeln(stack_freed ? "PASS" : "FAILED");
    terminal_write("  ELF UNLOAD: ");
    terminal_writeln(image_unloaded ? "PASS" : "FAILED");
    terminal_write("  PROCESS DESTROY: ");
    terminal_writeln(process_destroyed ? "PASS" : "FAILED");

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && file_ok && process_created && space_ok && loaded && entry_ok && text_readonly &&
        data_writable && result_access && initial_zero && stack_mapped && thread_created && prepared &&
        queued && yielded && shell_restored && magic_ok && thread_id_ok && clean_exit && !fault_captured &&
        reaped && stack_unmapped && stack_freed && image_unloaded && process_destroyed && frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER ELF LOADER TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

static void reschedtest_worker(void *argument) {
    volatile u64 *counter = (volatile u64 *)argument;
    if (!counter) cpu_halt_forever();
    for (u32 i = 0; i < 3U; ++i) {
        ++(*counter);
        if (!scheduler_yield()) {
            g_reschedtest_failed = true;
            cpu_halt_forever();
        }
    }
    for (; ;) {
        if (!scheduler_yield()) {
            g_reschedtest_failed = true;
            cpu_halt_forever();
        }
    }
}

static void command_reschedtest(void) {
    terminal_writeln("INTERRUPT RESCHEDULE TEST:");
    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled();

    terminal_write("  MAIN THREAD: ");
    terminal_writeln(main_ok ? "PASS" : "FAILED");

    if (!main_ok) return;

    Thread a;
    Thread b;
    bool created_a = thread_create(&a, process_kernel());
    bool created_b = created_a && thread_create(&b, process_kernel());
    terminal_write("  CREATE A: ");
    terminal_writeln(created_a ? "PASS" : "FAILED");
    terminal_write("  CREATE B: ");
    terminal_writeln(created_b ? "PASS" : "FAILED");

    if (!created_a || !created_b) {
        if (created_a) (void)thread_destroy(&a);
        return;
    }

    bool prepared_a = thread_prepare_kernel(&a, reschedtest_worker, (void *)&g_reschedtest_a_count);
    bool prepared_b = thread_prepare_kernel(&b, reschedtest_worker, (void *)&g_reschedtest_b_count);
    terminal_write("  PREPARE A: ");
    terminal_writeln(prepared_a ? "PASS" : "FAILED");
    terminal_write("  PREPARE B: ");
    terminal_writeln(prepared_b ? "PASS" : "FAILED");

    if (!prepared_a || !prepared_b) {
        (void)thread_destroy(&a);
        (void)thread_destroy(&b);
        return;
    }

    bool queued_a = scheduler_add(&a);
    bool queued_b = queued_a && scheduler_add(&b);
    terminal_write("  QUEUE A: ");
    terminal_writeln(queued_a ? "PASS" : "FAILED");
    terminal_write("  QUEUE B: ");
    terminal_writeln(queued_b ? "PASS" : "FAILED");

    if (!queued_a || !queued_b) {
        if (queued_a) (void)scheduler_remove(&a);
        (void)thread_destroy(&a);
        (void)thread_destroy(&b);
        return;
    }

    g_reschedtest_a_count = 0;
    g_reschedtest_b_count = 0;
    g_reschedtest_failed = false;

    u64 reschedule_before = scheduler_reschedule_count();
    u64 vector_before = interrupt_count(RESCHEDULE_VECTOR);

    /*
     * Queue:
     *
     *   main -> A -> B -> main
     *
     * One call from main produces:
     *
     *   main --INT81--> A
     *      A --INT81--> B
     *      B --INT81--> main
     *
     * Then the original call in main returns.
     */
    interrupts_disable();

    bool sequence_ok = true;
    for (u32 round = 0; round < 3U; ++round) {
        if (!scheduler_yield()) {
            sequence_ok = false;
            break;
        }
        if (thread_current() != main_thread || main_thread->state != THREAD_STATE_RUNNING) {
            sequence_ok = false;
            break;
        }
    }
    interrupts_enable();

    u64 reschedule_after = scheduler_reschedule_count();
    u64 vector_after = interrupt_count(RESCHEDULE_VECTOR);
    u64 reschedules = reschedule_after - reschedule_before;
    u64 vector_delta = vector_after - vector_before;
    bool counts_ok = g_reschedtest_a_count == 3ULL && g_reschedtest_b_count == 3ULL;
    bool current_restored = thread_current() == main_thread && main_thread->state == THREAD_STATE_RUNNING;
    bool children_suspended = (a.state == THREAD_STATE_READY && b.state == THREAD_STATE_READY &&
        a.on_run_queue && b.on_run_queue && a.interrupt_context_ready && b.interrupt_context_ready &&
        a.interrupt_rsp && b.interrupt_rsp);

    /*
     * Three complete:
     *
     *   main -> A -> B -> main
     *
     * cycles = 9 actual INT 0x81 switches.
     */
    bool switch_count_ok = reschedules == 9ULL && vector_delta == 9ULL;
    terminal_write("  SEQUENCE: ");
    terminal_writeln(sequence_ok ? "PASS" : "FAILED");
    terminal_write("  A COUNT: ");
    terminal_write_u64(g_reschedtest_a_count);
    terminal_putchar('\n');
    terminal_write("  B COUNT: ");
    terminal_write_u64(g_reschedtest_b_count);
    terminal_putchar('\n');
    terminal_write("  COUNTS 3/3: ");
    terminal_writeln(counts_ok ? "PASS" : "FAILED");
    terminal_write("  INT 0x81 COUNT: ");
    terminal_write_u64(vector_delta);
    terminal_putchar('\n');
    terminal_write("  RESCHEDULES: ");
    terminal_write_u64(reschedules);
    terminal_putchar('\n');
    terminal_write("  NINE SWITCHES: ");
    terminal_writeln(switch_count_ok ? "PASS" : "FAILED");
    terminal_write("  MAIN RESTORED: ");
    terminal_writeln(current_restored ? "PASS" : "FAILED");
    terminal_write("  CHILDREN SUSPENDED: ");
    terminal_writeln(children_suspended ? "PASS" : "FAILED");

    /* Stop queue mutation from overlapping any ordinary IRQ handler while cleaning up. */
    interrupts_disable();

    bool removed_a = scheduler_remove(&a);
    bool removed_b = scheduler_remove(&b);
    bool queue_restored = scheduler_thread_count() == 1;
    bool destroyed_a = removed_a && thread_destroy(&a);
    bool destroyed_b = removed_b && thread_destroy(&b);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;

    interrupts_enable();
    terminal_write("  REMOVE A: ");
    terminal_writeln(removed_a ? "PASS" : "FAILED");
    terminal_write("  REMOVE B: ");
    terminal_writeln(removed_b ? "PASS" : "FAILED");
    terminal_write("  QUEUE RESTORED: ");
    terminal_writeln(queue_restored ? "PASS" : "FAILED");
    terminal_write("  DESTROY A: ");
    terminal_writeln(destroyed_a ? "PASS" : "FAILED");
    terminal_write("  DESTROY B: ");
    terminal_writeln(destroyed_b ? "PASS" : "FAILED");
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME COUNT RESTORED: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");

    bool pass = main_ok && created_a && created_b && prepared_a && prepared_b && queued_a && queued_b &&
        sequence_ok && !g_reschedtest_failed && counts_ok && switch_count_ok && current_restored &&
        children_suspended && removed_a && removed_b && queue_restored && destroyed_a && destroyed_b &&
        frames_restored;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("INTERRUPT RESCHEDULE TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
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
        for (u32 leaf = 0; leaf < 3; ++leaf) {
            arch_cpuid(0x80000002U + leaf, 0, &words[leaf * 4], &words[leaf * 4 + 1], &words[leaf * 4 + 2],
                &words[leaf * 4 + 3]);
        }
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
    if (brand[0]) {
        terminal_write("BRAND: ");
        terminal_writeln(brand);
    }
    terminal_write("FAMILY: ");
    terminal_write_u64(family);
    terminal_write(" MODEL: ");
    terminal_write_u64(model);
    terminal_write(" STEPPING: ");
    terminal_write_u64(stepping);
    terminal_putchar('\n');
    terminal_write("APIC: ");
    terminal_write((d & (1U << 9)) ? "YES" : "NO");
    terminal_write("  X2APIC: ");
    terminal_write((c & (1U << 21)) ? "YES" : "NO");
    terminal_write("  SSE2: ");
    terminal_writeln((d & (1U << 26)) ? "YES" : "NO");
    bool long_mode = false;
    if (max_extended >= 0x80000001U) {
        arch_cpuid(0x80000001U, 0, &a, &b, &c, &d);
        long_mode = (d & (1U << 29)) != 0;
    }
    terminal_write("LONG MODE: ");
    terminal_writeln(long_mode ? "YES" : "NO");
    terminal_write("MAX BASIC CPUID LEAF: ");
    terminal_write_hex(max_basic);
    terminal_putchar('\n');
}

static void command_interrupts(void) {
    InterruptControllerInfo info = interrupt_controller_info();
    terminal_write("CONTROLLER: ");
    terminal_writeln(interrupt_controller_name());
    terminal_write("KEYBOARD GSI: ");
    terminal_write_u64(info.keyboard_gsi);
    terminal_putchar('\n');
    if (info.mode == INTERRUPT_CONTROLLER_APIC) {
        terminal_write("LOCAL APIC ID: ");
        terminal_write_u64(info.local_apic_id);
        terminal_putchar('\n');
    }
    terminal_write("VECTOR 0x21 COUNT: ");
    terminal_write_u64(interrupt_count(0x21));
    terminal_putchar('\n');
    terminal_write("PS/2 IRQ HANDLER COUNT: ");
    terminal_write_u64(ps2_irq_count());
    terminal_putchar('\n');
    terminal_write("PS/2 SCANCODE BYTES: ");
    terminal_write_u64(ps2_scancode_count());
    terminal_putchar('\n');
    terminal_write("DROPPED KEY EVENTS: ");
    terminal_write_u64(ps2_dropped_count());
    terminal_putchar('\n');
    terminal_write("LOCAL APIC SPURIOUS: ");
    terminal_write_u64(interrupt_spurious_count());
    terminal_putchar('\n');
}

static void command_acpi(void) {
    const AcpiInfo *info = acpi_get();
    terminal_write("RSDP: ");
    terminal_write_hex(info->rsdp_address);
    terminal_putchar('\n');
    terminal_write("ACPI VALID: ");
    terminal_writeln(info->valid ? "YES" : "NO");
    terminal_write("MADT VALID: ");
    terminal_writeln(info->madt_valid ? "YES" : "NO");
    terminal_write("LOCAL APIC ADDRESS: ");
    terminal_write_hex(info->lapic_address);
    terminal_putchar('\n');
    terminal_write("IO APIC COUNT: ");
    terminal_write_u64(info->io_apic_count);
    terminal_putchar('\n');
    terminal_write("KEYBOARD GSI: ");
    terminal_write_u64(info->keyboard_gsi);
    terminal_putchar('\n');
    terminal_write("8042 CONTROLLER: ");
    terminal_writeln(!info->i8042_known ? "UNKNOWN" : (info->i8042_present ? "PRESENT" : "ABSENT"));
    terminal_write("ACPI RESET REGISTER: ");
    terminal_writeln(info->reset_supported ? "YES" : "NO");
    terminal_write("ACPI S5 TABLES: ");
    terminal_writeln(info->poweroff_supported ? "YES" : "NO");
    terminal_write("ACPI S5 READY: ");
    terminal_writeln(acpi_poweroff_supported() ? "YES" : "NO");
    if (info->poweroff_supported) {
        terminal_write("PM1A CONTROL: ");
        terminal_write(info->pm1a_control.address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO ? "IO " : "MMIO ");
        terminal_write_hex(info->pm1a_control.address);
        terminal_putchar('\n');
        if (info->pm1b_control.address) {
            terminal_write("PM1B CONTROL: ");
            terminal_write(info->pm1b_control.address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO ? "IO " : "MMIO ");
            terminal_write_hex(info->pm1b_control.address);
            terminal_putchar('\n');
        }
        terminal_write("S5 TYPE A: ");
        terminal_write_u64(info->s5_type_a);
        terminal_write("  TYPE B: ");
        terminal_write_u64(info->s5_type_b);
        terminal_putchar('\n');
    }
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
    terminal_write("PCI DEVICES: ");
    terminal_write_u64(count);
    terminal_putchar('\n');

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
        terminal_write_u64(device->address.bus);
        terminal_putchar(':');
        terminal_write_u64(device->address.device);
        terminal_putchar('.');
        terminal_write_u64(device->address.function);
        terminal_write("  VEN=");
        terminal_write_hex(device->vendor_id);
        terminal_write(" DEV=");
        terminal_write_hex(device->device_id);
        terminal_write("  CLASS=");
        terminal_write_hex(device->class_code);
        terminal_write(" SUB=");
        terminal_write_hex(device->subclass);
        terminal_write(" IF=");
        terminal_write_hex(device->prog_if);
        terminal_write("  ");
        terminal_writeln(pci_device_kind(device));

        if (device->class_code == PCI_CLASS_MASS_STORAGE) {
            /* AHCI uses BAR5 as the ABAR. */
            if (device->subclass == PCI_SUBCLASS_SATA && device->prog_if == PCI_PROGIF_AHCI) {
                PciBar bar;
                if (pci_read_bar(device, 5, &bar) && bar.type != PCI_BAR_NONE) {
                    terminal_write("    ABAR/BAR5: ");
                    if (bar.type == PCI_BAR_MMIO32) terminal_write("MMIO32 ");
                    else if (bar.type == PCI_BAR_MMIO64) terminal_write("MMIO64 ");
                    else terminal_write("INVALID ");
                    terminal_write_hex(bar.base);
                    terminal_putchar('\n');
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
    terminal_write("  PCI: ");
    terminal_write_u64(info->pci_address.bus);
    terminal_putchar(':');
    terminal_write_u64(info->pci_address.device);
    terminal_putchar('.');
    terminal_write_u64(info->pci_address.function);
    terminal_putchar('\n');
    terminal_write("  ABAR: ");
    terminal_write_hex(info->abar);
    terminal_putchar('\n');
    terminal_write("  CAP: ");
    terminal_write_hex(info->capabilities);
    terminal_putchar('\n');
    terminal_write("  VERSION: ");
    terminal_write_hex(info->version);
    terminal_putchar('\n');
    terminal_write("  PORTS IMPLEMENTED: ");
    terminal_write_hex(info->ports_implemented);
    terminal_putchar('\n');
    terminal_write("  HARDWARE PORT COUNT: ");
    terminal_write_u64(info->hardware_port_count);
    terminal_putchar('\n');
    terminal_write("  ACTIVE DEVICES: ");
    terminal_write_u64(info->active_port_count);
    terminal_putchar('\n');
    terminal_writeln("PORTS:");

    bool any = false;

    for (u32 i = 0; i < AHCI_MAX_PORTS; ++i) {
        const AhciPortInfo *port = &info->ports[i];

        if (!port->implemented) continue;

        any = true;
        terminal_write("  PORT ");
        terminal_write_u64(port->port_number);
        terminal_write(": ");
        if (!port->present) terminal_write("NO DEVICE");
        else terminal_write(ahci_device_type_name(port->type));
        terminal_write("  SIG=");
        terminal_write_hex(port->signature);
        terminal_write("  SSTS=");
        terminal_write_hex(port->sata_status);

        /* Only SATA disks currently get their AHCI command engine/DMA memory initialized. */
        if (port->type == AHCI_DEVICE_SATA) {
            terminal_write("  ENGINE=");
            terminal_write(port->command_engine_ready ? "READY" : "FAILED");
        }

        terminal_putchar('\n');

        /* Show the physical DMA structures for initialized SATA ports. */
        if (port->type == AHCI_DEVICE_SATA && port->command_engine_ready) {
            u64 clb = 0;
            u64 fis = 0;
            u64 table = 0;
            u64 identify = 0;

            if (ahci_port_dma_info(port->port_number, &clb, &fis, &table, &identify)) {
                terminal_write("    CLB=");
                terminal_write_hex(clb);
                terminal_write("  FIS=");
                terminal_write_hex(fis);
                terminal_putchar('\n');
                terminal_write("    CMD TABLE=");
                terminal_write_hex(table);
                terminal_write("  IDENTIFY=");
                terminal_write_hex(identify);
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
                terminal_write("    LOGICAL SECTOR: ");
                terminal_write_u64(port->logical_sector_size);
                terminal_writeln(" BYTES");
                terminal_write("    SECTORS: ");
                terminal_write_u64(port->sector_count);
                terminal_putchar('\n');

                if (port->logical_sector_size && port->sector_count <= (~0ULL / port->logical_sector_size)) {
                    u64 bytes = port->sector_count * port->logical_sector_size;
                    terminal_write("    CAPACITY: ");
                    terminal_write_u64(bytes / (1024ULL * 1024ULL));
                    terminal_writeln(" MiB");
                }
            }
        }
    }

    if (!any) terminal_writeln("  NO PORTS IMPLEMENTED.");
}

static void command_disks(void) {
    u32 count = block_device_count();
    terminal_write("BLOCK DEVICES: ");
    terminal_write_u64(count);
    terminal_putchar('\n');

    for (u32 i = 0; i < count; ++i) {
        BlockDevice *device = block_device(i);

        if (!device) continue;

        terminal_write("  ");
        terminal_write(device->name);
        terminal_write("  TYPE=");
        terminal_write(block_type_name(device->type));
        terminal_putchar('\n');
        terminal_write("    BLOCK SIZE: ");
        terminal_write_u64(device->block_size);
        terminal_writeln(" BYTES");
        terminal_write("    BLOCKS: ");
        terminal_write_u64(device->block_count);
        terminal_putchar('\n');

        u64 bytes = block_capacity_bytes(device);
        terminal_write("    CAPACITY: ");
        terminal_write_u64(bytes / (1024ULL * 1024ULL));
        terminal_writeln(" MiB");
        terminal_write("    MODE: ");
        terminal_writeln(device->read_only ? "READ-ONLY" : "READ-WRITE");
    }
}

static void command_sector(const char *argument) {
    u64 lba = 0;

    if (!parse_u64(argument, &lba)) {
        terminal_writeln("usage: sector LBA");
        return;
    }

    BlockDevice *device = block_find("sda1");

    if (!device) {
        terminal_writeln("NO BLOCK DEVICE.");
        return;
    }
    if (device->block_size > 4096U) {
        terminal_writeln("BLOCK TOO LARGE.");
        return;
    }
    if (lba >= device->block_count) {
        terminal_writeln("LBA OUT OF RANGE.");
        return;
    }

    static u8 buffer[4096];

    if (!block_read(device, lba, 1, buffer)) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("SECTOR READ FAILED.");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("LBA ");
    terminal_write_u64(lba);
    terminal_write(" FROM ");
    terminal_writeln(device->name);

    /* First 128 bytes for now. */
    for (u32 row = 0; row < 8U; ++row) {
        terminal_write_hex(row * 16U);
        terminal_write(": ");

        for (u32 col = 0; col < 16U; ++col) {
            terminal_hex_byte(buffer[row * 16U + col]);
            terminal_putchar(' ');
        }

        terminal_putchar('\n');
    }

    if (lba == 0 && device->block_size >= 512U) {
        terminal_write("MBR SIGNATURE: ");
        terminal_hex_byte(buffer[510]);
        terminal_putchar(' ');
        terminal_hex_byte(buffer[511]);
        terminal_putchar('\n');
    }

    if (lba == 1 && device->block_size >= 8U) {
        bool gpt = buffer[0] == 'E' && buffer[1] == 'F' && buffer[2] == 'I' && buffer[3] == ' ' &&
            buffer[4] == 'P' && buffer[5] == 'A' && buffer[6] == 'R' && buffer[7] == 'T';

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

        terminal_write("  sda");
        terminal_write_u64(part->index);
        if (part->name[0]) {
            terminal_write("  ");
            terminal_write(part->name);
        }

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
    Process process;
    bool process_created = process_create(&process);
    terminal_write("  PROCESS: ");
    terminal_writeln(process_created ? "PASS" : "FAILED");

    if (!process_created) return;

    AddressSpace *space = process_address_space(&process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    terminal_write("  ADDRESS SPACE: ");
    terminal_writeln(space_ok ? "PASS" : "FAILED");

    if (!space_ok) {
        (void)process_destroy(&process);
        terminal_writeln("USERFAULT: ADDRESS SPACE CREATION FAILED.");
        return;
    }

    Thread thread;
    bool thread_created = thread_create(&thread, &process);

    if (!thread_created) {
        (void)process_destroy(&process);
        terminal_writeln("USERFAULT: THREAD FAILED.");
        return;
    }

    frame_t code_frame = frame_alloc();
    frame_t stack_frame = frame_alloc();

    if (code_frame == FRAME_INVALID || stack_frame == FRAME_INVALID) {
        if (code_frame != FRAME_INVALID) (void)frame_free(code_frame);
        if (stack_frame != FRAME_INVALID) (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
        terminal_writeln("USERFAULT: FRAME ALLOCATION FAILED.");
        return;
    }

    bool code_mapped = address_space_map_page(space, user_code, code_frame, VM_EXEC);
    bool stack_mapped = address_space_map_page(space, user_stack, stack_frame, VM_WRITE);

    if (!code_mapped || !stack_mapped) {
        frame_t ignored = FRAME_INVALID;

        if (code_mapped) (void)address_space_unmap_page(space, user_code, &ignored);
        if (stack_mapped) (void)address_space_unmap_page(space, user_stack, &ignored);

        (void)frame_free(code_frame);
        (void)frame_free(stack_frame);

        (void)thread_destroy(&thread);
        (void)process_destroy(&process);
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
    terminal_write("  USER RIP: ");
    terminal_write_hex(user_code);
    terminal_putchar('\n');
    terminal_write("  USER RSP: ");
    terminal_write_hex(user_stack_top);
    terminal_putchar('\n');
    terminal_write("  KERNEL RSP0: ");
    terminal_write_hex(thread.kernel_stack_top);
    terminal_putchar('\n');

    /* No interrupt may observe the transitional kernel context before IRETQ enters this thread. */
    interrupts_disable();

    if (!thread_activate(&thread)) {
        interrupts_enable();
        terminal_writeln("USERFAULT: THREAD ACTIVATION FAILED.");
        return;
    }

    terminal_write("  ACTIVE THREAD: ");
    terminal_write_u64(thread_current()->id);
    terminal_putchar('\n');

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
    terminal_write("  BYTES/SECTOR: ");
    terminal_write_u64(info->bytes_per_sector);
    terminal_putchar('\n');
    terminal_write("  SECTORS/CLUSTER: ");
    terminal_write_u64(info->sectors_per_cluster);
    terminal_putchar('\n');
    terminal_write("  RESERVED SECTORS: ");
    terminal_write_u64(info->reserved_sectors);
    terminal_putchar('\n');
    terminal_write("  FAT COUNT: ");
    terminal_write_u64(info->fat_count);
    terminal_putchar('\n');
    terminal_write("  SECTORS/FAT: ");
    terminal_write_u64(info->sectors_per_fat);
    terminal_putchar('\n');
    terminal_write("  TOTAL SECTORS: ");
    terminal_write_u64(info->total_sectors);
    terminal_putchar('\n');
    terminal_write("  CLUSTERS: ");
    terminal_write_u64(info->cluster_count);
    terminal_putchar('\n');
    terminal_write("  ROOT CLUSTER: ");
    terminal_write_u64(info->root_cluster);
    terminal_putchar('\n');
    terminal_write("  FIRST FAT SECTOR: ");
    terminal_write_u64(info->first_fat_sector);
    terminal_putchar('\n');
    terminal_write("  FIRST DATA SECTOR: ");
    terminal_write_u64(info->first_data_sector);
    terminal_putchar('\n');
}

static void command_fatls(void) {
    const Fat32Info *info = fat32_get();

    if (!info || !info->valid) {
        terminal_writeln("FAT32 NOT INITIALIZED.");
        return;
    }

    static Fat32DirectoryEntry entries[64];

    u32 count = 0;

    if (!fat32_read_root(entries, ARRAY_COUNT(entries), &count)) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("FAT32 ROOT DIRECTORY READ FAILED.");
        terminal_set_color(terminal_default_color());
        return;
    }

    terminal_write("FAT32 ROOT: ");
    terminal_write_u64(count);
    terminal_writeln(" ENTRIES");

    for (u32 i = 0; i < count; ++i) {
        Fat32DirectoryEntry *entry = &entries[i];
        terminal_write("  ");
        terminal_write(entry->name);
        if (entry->attributes & FAT32_ATTR_DIRECTORY) terminal_putchar('/');
        terminal_putchar('\n');
        terminal_write("    CLUSTER: ");
        terminal_write_u64(entry->first_cluster);

        if (!(entry->attributes & FAT32_ATTR_DIRECTORY)) {
            terminal_write("  SIZE: ");
            terminal_write_u64(entry->size);
            terminal_write(" BYTES");
        }

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
    terminal_write("SIZE: ");
    terminal_write_u64(entry.size);
    terminal_writeln(" BYTES");
    terminal_write("OFFSET: ");
    terminal_write_u64(offset);
    terminal_putchar('\n');
    terminal_write("FIRST CLUSTER: ");
    terminal_write_u64(entry.first_cluster);
    terminal_putchar('\n');
    terminal_write("TEST READ: ");
    terminal_write_u64(read);
    terminal_writeln(" BYTES");
    terminal_writeln("FIRST 64 BYTES OF READ:");

    u64 dump = read < 64ULL ? read : 64ULL;

    for (u64 offset = 0; offset < dump; offset += 16ULL) {
        terminal_write_hex(offset);
        terminal_write(": ");

        u64 row = dump - offset;

        if (row > 16ULL) row = 16ULL;
        for (u64 i = 0; i < row; ++i) {
            terminal_hex_byte(buffer[offset + i]);
            terminal_putchar(' ');
        }

        terminal_putchar('\n');
    }

    /* ELF64 signature: 7F 'E' 'L' 'F' */
    if (read >= 4 && buffer[0] == 0x7FU && buffer[1] == 'E' && buffer[2] == 'L' && buffer[3] == 'F') {
        terminal_writeln("ELF SIGNATURE: VALID");
    }

    /* USTAR magic starts at byte 257 of the first TAR header. */
    if (read >= 262 && buffer[257] == 'u' && buffer[258] == 's' && buffer[259] == 't' &&
        buffer[260] == 'a' && buffer[261] == 'r') {
        terminal_writeln("USTAR SIGNATURE: VALID");
    }
}

static void command_power_result(const char *action, PowerResult result) {
    terminal_set_color(terminal_error_color());
    terminal_write(action);
    terminal_write(" REFUSED: ");
    terminal_writeln(power_result_name(result));
    terminal_set_color(terminal_default_color());
}

static void command_power(void) {
    PowerResult shutdown = power_check_shutdown();
    PowerResult reboot = power_check_reboot();

    terminal_write("SHUTDOWN READY: ");
    terminal_writeln(shutdown == POWER_RESULT_OK ? "YES" : "NO");
    if (shutdown != POWER_RESULT_OK) {
        terminal_write("  REASON: ");
        terminal_writeln(power_result_name(shutdown));
    }
    terminal_write("REBOOT READY: ");
    terminal_writeln(reboot == POWER_RESULT_OK ? "YES" : "NO");
    if (reboot != POWER_RESULT_OK) {
        terminal_write("  REASON: ");
        terminal_writeln(power_result_name(reboot));
    }
    terminal_write("STORAGE SAFE: ");
    terminal_writeln(power_storage_safe() ? "YES (READ-ONLY / NO DIRTY CACHE)" : "NO");
}

static void command_shutdown(void) {
    PowerResult ready = power_check_shutdown();
    if (ready != POWER_RESULT_OK) {
        command_power_result("SHUTDOWN", ready);
        return;
    }

    terminal_set_color(terminal_accent_color());
    terminal_writeln("SHUTTING DOWN...");
    terminal_set_color(terminal_default_color());
    PowerResult result = power_shutdown();
    command_power_result("SHUTDOWN", result);
}

static void command_reboot(void) {
    PowerResult ready = power_check_reboot();
    if (ready != POWER_RESULT_OK) {
        command_power_result("REBOOT", ready);
        return;
    }

    terminal_set_color(terminal_accent_color());
    terminal_writeln("REBOOTING...");
    terminal_set_color(terminal_default_color());
    PowerResult result = power_reboot();
    command_power_result("REBOOT", result);
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
    if (!path || !*path) {
        g_cwd = vfs_root();
        return;
    }

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

    if (!node) {
        terminal_writeln("cat: file not found");
        return;
    }
    if (node->type != VFS_FILE) {
        terminal_writeln("cat: not a file");
        return;
    }
    for (u64 i = 0; i < node->size; ++i) terminal_putchar((char)node->data[i]);
    if (node->size && node->data[node->size - 1] != '\n') terminal_putchar('\n');
}


typedef enum {
    SHELL_GROUP_GENERAL = 0,
    SHELL_GROUP_FILESYSTEM,
    SHELL_GROUP_SYSTEM,
    SHELL_GROUP_HARDWARE,
    SHELL_GROUP_STORAGE,
    SHELL_GROUP_DEVELOPMENT,
    SHELL_GROUP_COUNT
} ShellCommandGroup;

typedef void (*ShellCommandNoArgs)(void);
typedef void (*ShellCommandArgs)(const char *args);

typedef struct {
    const char *name;
    const char *usage;
    const char *description;
    ShellCommandGroup group;
    ShellCommandNoArgs no_args;
    ShellCommandArgs with_args;
    bool visible;
} ShellCommand;

typedef struct {
    const char *name;
    const char *description;
    KernelTestGroup group;
    void (*run)(void);
} ShellLocalTest;

static void command_clear(void) { terminal_clear(); }

static void command_usershell(void) {
    if (userspace_shell_run()) return;
    terminal_set_color(terminal_error_color());
    terminal_writeln("USERSPACE SHELL HANDOFF FAILED; KERNEL MONITOR REMAINS ACTIVE.");
    terminal_set_color(terminal_default_color());
}

static void command_fault(void) { __asm__ volatile ("ud2"); }

#define SHELL_COMMAND_CAPACITY 96U
#define SHELL_LOCAL_TEST_CAPACITY 32U

static ShellCommand g_commands[SHELL_COMMAND_CAPACITY];
static u32 g_command_count;
static ShellLocalTest g_shell_tests[SHELL_LOCAL_TEST_CAPACITY];
static u32 g_shell_test_count;
static bool g_shell_registry_initialized;

static void shell_add_command(const char *name, const char *usage, const char *description,
    ShellCommandGroup group, ShellCommandNoArgs no_args, ShellCommandArgs with_args, bool visible) {
    if (g_command_count >= SHELL_COMMAND_CAPACITY) return;
    ShellCommand *command = &g_commands[g_command_count++];
    command->name = name;
    command->usage = usage;
    command->description = description;
    command->group = group;
    command->no_args = no_args;
    command->with_args = with_args;
    command->visible = visible;
}

static void shell_add_local_test(const char *name, const char *description,
    KernelTestGroup group, void (*run)(void)) {
    if (g_shell_test_count >= SHELL_LOCAL_TEST_CAPACITY) return;
    ShellLocalTest *test = &g_shell_tests[g_shell_test_count++];
    test->name = name;
    test->description = description;
    test->group = group;
    test->run = run;
}

static void shell_registry_init(void) {
    if (g_shell_registry_initialized) return;
    g_shell_registry_initialized = true;

    shell_add_local_test("frame", "PMM allocation/free/reuse", KERNEL_TEST_MEMORY, command_frametest);
    shell_add_local_test("vmm-basic", "x86-64 page-table operations", KERNEL_TEST_MEMORY, command_vmmtest);
    shell_add_local_test("address-space", "address-space ownership and sharing", KERNEL_TEST_MEMORY, command_astest);
    shell_add_local_test("supervisor", "long-lived Ring3 supervisor", KERNEL_TEST_USERSPACE, command_supervisortest);
    shell_add_local_test("endpoint", "endpoint objects and capability rights", KERNEL_TEST_IPC, command_endpointtest);
    shell_add_local_test("ipc", "non-blocking capability IPC", KERNEL_TEST_IPC, command_ipctest);
    shell_add_local_test("ipc-receive-block", "blocking IPC receive/wakeup", KERNEL_TEST_IPC, command_ipcblocktest);
    shell_add_local_test("ipc-send-block", "blocking IPC send/wakeup", KERNEL_TEST_IPC, command_ipcsendblocktest);
    shell_add_local_test("user-ipc", "Ring3 capability IPC syscalls", KERNEL_TEST_USERSPACE, command_useripctest);
    shell_add_local_test("user-ipc-receive-block", "Ring3 blocking IPC receive", KERNEL_TEST_USERSPACE, command_useripcblocktest);
    shell_add_local_test("user-ipc-send-block", "Ring3 blocking IPC send", KERNEL_TEST_USERSPACE, command_useripcsendblocktest);
    shell_add_local_test("process", "process/address-space/capability ownership", KERNEL_TEST_TASK, command_processtest);
    shell_add_local_test("thread", "thread lifecycle and kernel stacks", KERNEL_TEST_TASK, command_threadtest);
    shell_add_local_test("scheduler", "round-robin full-frame scheduling", KERNEL_TEST_SCHEDULING, command_schedtest);
    shell_add_local_test("block", "thread blocking and wakeup", KERNEL_TEST_SCHEDULING, command_blocktest);
    shell_add_local_test("exit", "permanent current-thread termination", KERNEL_TEST_TASK, command_exittest);
    shell_add_local_test("syscall", "Ring3 syscall round-trip", KERNEL_TEST_USERSPACE, command_syscalltest);
    shell_add_local_test("user-isolation", "recoverable Ring3 fault isolation", KERNEL_TEST_USERSPACE, command_userisotest);
    shell_add_local_test("user-page-fault", "recoverable Ring3 page fault", KERNEL_TEST_USERSPACE, command_userpftest);
    shell_add_local_test("timer-irq", "periodic IRQ0 ticks", KERNEL_TEST_SCHEDULING, command_timertest);
    shell_add_local_test("preemption", "timer-driven involuntary switching", KERNEL_TEST_SCHEDULING, command_preempttest);
    shell_add_local_test("user-preemption", "PIT preemption from Ring3", KERNEL_TEST_SCHEDULING, command_userpreempttest);
    shell_add_local_test("user-elf", "filesystem ELF Ring3 loader", KERNEL_TEST_USERSPACE, command_userelftest);
    shell_add_local_test("reschedule", "INT 0x81 full-frame scheduling", KERNEL_TEST_SCHEDULING, command_reschedtest);

    shell_add_command("help", "help [COMMAND]", "show command help", SHELL_GROUP_GENERAL, 0, command_help, true);
    shell_add_command("about", "about", "describe this kernel", SHELL_GROUP_GENERAL, command_about, 0, true);
    shell_add_command("usershell", "usershell", "return to the normal Ring3 shell; monitor/Escape returns",
        SHELL_GROUP_GENERAL, command_usershell, 0, true);
    shell_add_command("clear", "clear", "clear the terminal", SHELL_GROUP_GENERAL, command_clear, 0, true);
    shell_add_command("shutdown", "shutdown", "gracefully power off the system", SHELL_GROUP_GENERAL, command_shutdown, 0, true);
    shell_add_command("reboot", "reboot", "gracefully restart the system", SHELL_GROUP_GENERAL, command_reboot, 0, true);
    shell_add_command("pwd", "pwd", "print the current directory", SHELL_GROUP_FILESYSTEM, command_pwd, 0, true);
    shell_add_command("ls", "ls", "list files in the current directory", SHELL_GROUP_FILESYSTEM, command_ls, 0, true);
    shell_add_command("cd", "cd PATH", "change the current directory", SHELL_GROUP_FILESYSTEM, 0, command_cd, true);
    shell_add_command("cat", "cat FILE", "print a file", SHELL_GROUP_FILESYSTEM, 0, command_cat, true);
    shell_add_command("memory", "memory", "show memory-map and allocator state", SHELL_GROUP_SYSTEM, command_memory, 0, true);
    shell_add_command("cpu", "cpu", "show CPUID information", SHELL_GROUP_SYSTEM, command_cpu, 0, true);
    shell_add_command("interrupts", "interrupts", "show interrupt-controller and keyboard state", SHELL_GROUP_SYSTEM, command_interrupts, 0, true);
    shell_add_command("timer", "timer", "show PIT timer state", SHELL_GROUP_SYSTEM, command_timer, 0, true);
    shell_add_command("power", "power", "show shutdown/reboot readiness", SHELL_GROUP_SYSTEM, command_power, 0, true);
    shell_add_command("acpi", "acpi", "show ACPI discovery results", SHELL_GROUP_HARDWARE, command_acpi, 0, true);
    shell_add_command("pci", "pci", "list discovered PCI devices", SHELL_GROUP_HARDWARE, command_pci, 0, true);
    shell_add_command("ahci", "ahci", "show AHCI controller and SATA ports", SHELL_GROUP_HARDWARE, command_ahci, 0, true);
    shell_add_command("disks", "disks", "list block devices", SHELL_GROUP_STORAGE, command_disks, 0, true);
    shell_add_command("sector", "sector LBA", "dump a raw disk sector", SHELL_GROUP_STORAGE, 0, command_sector, true);
    shell_add_command("partitions", "partitions", "list GPT partitions", SHELL_GROUP_STORAGE, command_partitions, 0, true);
    shell_add_command("fat32", "fat32", "show FAT32 filesystem information", SHELL_GROUP_STORAGE, command_fat32, 0, true);
    shell_add_command("fatls", "fatls", "list the FAT32 root directory", SHELL_GROUP_STORAGE, command_fatls, 0, true);
    shell_add_command("fatread", "fatread FILE [OFFSET] [COUNT]", "read/test a FAT32 root file", SHELL_GROUP_STORAGE, 0, command_fatread, true);
    shell_add_command("test", "test list [GROUP] | test NAME [cleanup]", "list or run kernel diagnostics", SHELL_GROUP_DEVELOPMENT, 0, command_test, true);
    shell_add_command("alloc", "alloc", "allocate one physical 4 KiB frame", SHELL_GROUP_DEVELOPMENT, command_alloc, 0, true);
    shell_add_command("fault", "fault", "deliberately execute UD2 in the kernel", SHELL_GROUP_DEVELOPMENT, command_fault, 0, true);
    shell_add_command("userfault", "userfault", "enter Ring3 and deliberately execute UD2", SHELL_GROUP_DEVELOPMENT, command_userfault, 0, true);
    shell_add_command("frametest", "frametest", "legacy alias for test frame", SHELL_GROUP_DEVELOPMENT, command_frametest, 0, false);
    shell_add_command("vmmtest", "vmmtest", "legacy alias for test vmm-basic", SHELL_GROUP_DEVELOPMENT, command_vmmtest, 0, false);
    shell_add_command("astest", "astest", "legacy alias for test address-space", SHELL_GROUP_DEVELOPMENT, command_astest, 0, false);
    shell_add_command("threadtest", "threadtest", "legacy alias for test thread", SHELL_GROUP_DEVELOPMENT, command_threadtest, 0, false);
    shell_add_command("forcethreadtest", "forcethreadtest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, force_thread_test_run, 0, false);
    shell_add_command("supervisortest", "supervisortest", "legacy alias for test supervisor", SHELL_GROUP_DEVELOPMENT, command_supervisortest, 0, false);
    shell_add_command("captest", "captest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, command_captest, 0, false);
    shell_add_command("caplifetimetest", "caplifetimetest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, capability_lifetime_test_run, 0, false);
    shell_add_command("endpointtest", "endpointtest", "legacy alias for test endpoint", SHELL_GROUP_DEVELOPMENT, command_endpointtest, 0, false);
    shell_add_command("ipctest", "ipctest", "legacy alias for test ipc", SHELL_GROUP_DEVELOPMENT, command_ipctest, 0, false);
    shell_add_command("ipcblocktest", "ipcblocktest", "legacy receive-block test alias", SHELL_GROUP_DEVELOPMENT, command_ipcblocktest, 0, false);
    shell_add_command("ipcsendblocktest", "ipcsendblocktest", "legacy send-block test alias", SHELL_GROUP_DEVELOPMENT, command_ipcsendblocktest, 0, false);
    shell_add_command("stackreclaimtest", "stackreclaimtest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, stack_reclaim_test_run, 0, false);
    shell_add_command("waitordertest", "waitordertest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, ipc_wait_order_test_run, 0, false);
    shell_add_command("waitcleanupretry", "waitcleanupretry", "legacy cleanup alias", SHELL_GROUP_DEVELOPMENT, ipc_wait_order_cleanup_run, 0, false);
    shell_add_command("useripctest", "useripctest", "legacy alias for test user-ipc", SHELL_GROUP_DEVELOPMENT, command_useripctest, 0, false);
    shell_add_command("useripccanceltest", "useripccanceltest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, user_ipc_cancel_test_run, 0, false);
    shell_add_command("useripcblocktest", "useripcblocktest", "legacy user receive-block alias", SHELL_GROUP_DEVELOPMENT, command_useripcblocktest, 0, false);
    shell_add_command("useripcsendblocktest", "useripcsendblocktest", "legacy user send-block alias", SHELL_GROUP_DEVELOPMENT, command_useripcsendblocktest, 0, false);
    shell_add_command("processtest", "processtest", "legacy alias for test process", SHELL_GROUP_DEVELOPMENT, command_processtest, 0, false);
    shell_add_command("processkilltest", "processkilltest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, process_terminate_test_run, 0, false);
    shell_add_command("peerdeathtest", "peerdeathtest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, peer_death_test_run, 0, false);
    shell_add_command("peerdeathcleanupretry", "peerdeathcleanupretry", "legacy cleanup alias", SHELL_GROUP_DEVELOPMENT, peer_death_cleanup_run, 0, false);
    shell_add_command("userprocesscleanuptest", "userprocesscleanuptest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, user_process_cleanup_test_run, 0, false);
    shell_add_command("usercleanupretry", "usercleanupretry", "legacy cleanup alias", SHELL_GROUP_DEVELOPMENT, user_fixture_cleanup_retry_run, 0, false);
    shell_add_command("schedtest", "schedtest", "legacy alias for test scheduler", SHELL_GROUP_DEVELOPMENT, command_schedtest, 0, false);
    shell_add_command("blocktest", "blocktest", "legacy alias for test block", SHELL_GROUP_DEVELOPMENT, command_blocktest, 0, false);
    shell_add_command("exittest", "exittest", "legacy alias for test exit", SHELL_GROUP_DEVELOPMENT, command_exittest, 0, false);
    shell_add_command("syscalltest", "syscalltest", "legacy alias for test syscall", SHELL_GROUP_DEVELOPMENT, command_syscalltest, 0, false);
    shell_add_command("timeouttest", "timeouttest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, ipc_timeout_order_test_run, 0, false);
    shell_add_command("timeoutcleanupretry", "timeoutcleanupretry", "legacy cleanup alias", SHELL_GROUP_DEVELOPMENT, ipc_timeout_order_cleanup_run, 0, false);
    shell_add_command("userisotest", "userisotest", "legacy alias for test user-isolation", SHELL_GROUP_DEVELOPMENT, command_userisotest, 0, false);
    shell_add_command("userpftest", "userpftest", "legacy alias for test user-page-fault", SHELL_GROUP_DEVELOPMENT, command_userpftest, 0, false);
    shell_add_command("userelftest", "userelftest", "legacy alias for test user-elf", SHELL_GROUP_DEVELOPMENT, command_userelftest, 0, false);
    shell_add_command("userruntimetest", "userruntimetest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, user_runtime_test_run, 0, false);
    shell_add_command("userruntimeblocktest", "userruntimeblocktest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, user_runtime_block_test_run, 0, false);
    shell_add_command("elfreclaimtest", "elfreclaimtest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, elf_reclaim_test_run, 0, false);
    shell_add_command("vmmreclaimtest", "vmmreclaimtest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, vmm_reclaim_test_run, 0, false);
    shell_add_command("constructortest", "constructortest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, constructor_test_run, 0, false);
    shell_add_command("publishedcleanuptest", "publishedcleanuptest", "legacy test alias", SHELL_GROUP_DEVELOPMENT, published_cleanup_test_run, 0, false);
    shell_add_command("forcecleanupretry", "forcecleanupretry", "legacy cleanup alias", SHELL_GROUP_DEVELOPMENT, force_thread_cleanup_run, 0, false);
    shell_add_command("timertest", "timertest", "legacy alias for test timer-irq", SHELL_GROUP_DEVELOPMENT, command_timertest, 0, false);
    shell_add_command("preempttest", "preempttest", "legacy alias for test preemption", SHELL_GROUP_DEVELOPMENT, command_preempttest, 0, false);
    shell_add_command("userpreempttest", "userpreempttest", "legacy alias for test user-preemption", SHELL_GROUP_DEVELOPMENT, command_userpreempttest, 0, false);
    shell_add_command("reschedtest", "reschedtest", "legacy alias for test reschedule", SHELL_GROUP_DEVELOPMENT, command_reschedtest, 0, false);
}

static const char *shell_group_name(ShellCommandGroup group) {
    switch (group) {
        case SHELL_GROUP_GENERAL: return "GENERAL";
        case SHELL_GROUP_FILESYSTEM: return "FILESYSTEM";
        case SHELL_GROUP_SYSTEM: return "SYSTEM";
        case SHELL_GROUP_HARDWARE: return "HARDWARE";
        case SHELL_GROUP_STORAGE: return "STORAGE";
        case SHELL_GROUP_DEVELOPMENT: return "DEVELOPMENT";
        default: return "OTHER";
    }
}

static const ShellCommand *shell_find_command(const char *name) {
    shell_registry_init();
    if (!name || !*name) return 0;
    for (u32 i = 0; i < g_command_count; ++i) {
        if (k_strieq(name, g_commands[i].name)) return &g_commands[i];
    }
    return 0;
}

static void shell_print_padded(const char *usage, const char *description) {
    const u32 width = 30U;
    terminal_write("  ");
    terminal_write(usage);
    u32 length = (u32)k_strlen(usage);
    do {
        terminal_putchar(' ');
        ++length;
    } while (length < width);
    terminal_writeln(description);
}

static void shell_print_help(const char *topic) {
    shell_registry_init();
    char name[64];
    char extra[64];
    const char *cursor = topic ? topic : "";
    bool has_name = next_argument(&cursor, name, sizeof(name));

    if (has_name) {
        if (next_argument(&cursor, extra, sizeof(extra))) {
            terminal_writeln("USAGE: help [COMMAND]");
            return;
        }

        const ShellCommand *command = shell_find_command(name);
        if (!command) {
            terminal_set_color(terminal_error_color());
            terminal_write("UNKNOWN COMMAND: ");
            terminal_writeln(name);
            terminal_set_color(terminal_default_color());
            return;
        }

        terminal_write("USAGE: ");
        terminal_writeln(command->usage);
        terminal_writeln(command->description);

        if (k_strieq(command->name, "test")) {
            terminal_writeln("  test list [GROUP]       list available diagnostics");
            terminal_writeln("  test NAME               run one diagnostic");
            terminal_writeln("  test NAME cleanup       retry that diagnostic's retained cleanup");
        }
        return;
    }

    terminal_writeln("COMMANDS:");
    for (u32 group = 0; group < SHELL_GROUP_COUNT; ++group) {
        terminal_putchar('\n');
        terminal_writeln(shell_group_name((ShellCommandGroup)group));
        for (u32 i = 0; i < g_command_count; ++i) {
            const ShellCommand *command = &g_commands[i];
            if (command->visible && command->group == (ShellCommandGroup)group)
                shell_print_padded(command->usage, command->description);
        }
    }

    terminal_writeln("\nUSE help COMMAND FOR DETAILS.");
    terminal_writeln("USE test list FOR AVAILABLE DIAGNOSTICS.");
    terminal_writeln("KEYS: UP/DOWN HISTORY, LEFT/RIGHT EDIT, HOME/END, PGUP/PGDN SCROLL.");
    terminal_writeln("CLIPBOARD: SHIFT+ARROWS SELECT, CTRL+SHIFT+C/X/V COPY/CUT/PASTE.");
}

static const ShellLocalTest *shell_find_local_test(const char *name) {
    shell_registry_init();
    if (!name || !*name) return 0;
    for (u32 i = 0; i < g_shell_test_count; ++i) {
        if (k_strieq(name, g_shell_tests[i].name)) return &g_shell_tests[i];
    }
    return 0;
}

static void shell_list_tests(const char *filter) {
    shell_registry_init();
    bool matched_group = !filter || !*filter;

    for (u32 group = 0; group < KERNEL_TEST_GROUP_COUNT; ++group) {
        KernelTestGroup current = (KernelTestGroup)group;
        if (filter && *filter && !k_strieq(filter, kernel_test_group_slug(current))) continue;

        matched_group = true;
        terminal_putchar('\n');
        terminal_writeln(kernel_test_group_title(current));

        for (u32 i = 0; i < g_shell_test_count; ++i) {
            if (g_shell_tests[i].group == current)
                shell_print_padded(g_shell_tests[i].name, g_shell_tests[i].description);
        }

        for (u32 i = 0; i < kernel_test_registry_count(); ++i) {
            const KernelTest *test = kernel_test_registry_at(i);
            if (test && test->group == current)
                shell_print_padded(test->name, test->description);
        }
    }

    if (!matched_group) {
        terminal_set_color(terminal_error_color());
        terminal_write("UNKNOWN TEST GROUP: ");
        terminal_writeln(filter);
        terminal_set_color(terminal_default_color());
        terminal_writeln("GROUPS: memory task ipc lifetime userspace scheduling acceptance");
    }
}

#define SHELL_TEST_RFLAGS_IF (1ULL << 9)

static u64 shell_test_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void shell_test_irq_restore(u64 flags) {
    if (flags & SHELL_TEST_RFLAGS_IF) interrupts_enable();
}

static bool shell_run_test_mode(void (*run)(void), bool live_preemption) {
    if (!run) return false;

    u64 flags = shell_test_irq_save();
    bool original = scheduler_preemption_enabled();
    bool ready = true;

    if (live_preemption) {
        ready = original;
    } else if (original) {
        ready = scheduler_preemption_disable();
    }
    shell_test_irq_restore(flags);

    if (!ready) {
        terminal_set_color(terminal_error_color());
        terminal_writeln(live_preemption ?
            "TEST REQUIRES NORMAL PREEMPTION POLICY ACTIVE." :
            "FAILED TO QUIESCE PREEMPTION FOR DETERMINISTIC TEST.");
        terminal_set_color(terminal_default_color());
        return false;
    }

    run();

    flags = shell_test_irq_save();
    bool current = scheduler_preemption_enabled();
    bool restored = true;
    if (original && !current) restored = scheduler_preemption_enable();
    else if (!original && current) restored = scheduler_preemption_disable();
    shell_test_irq_restore(flags);

    if (!restored) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("FAILED TO RESTORE SCHEDULER PREEMPTION POLICY AFTER TEST.");
        terminal_set_color(terminal_default_color());
    }
    return restored;
}

static void command_test(const char *args) {
    char first[64];
    char second[64];
    char extra[64];
    const char *cursor = args ? args : "";

    if (!next_argument(&cursor, first, sizeof(first))) {
        terminal_writeln("USAGE: test list [GROUP] | test NAME [cleanup]");
        return;
    }

    bool has_second = next_argument(&cursor, second, sizeof(second));
    if (next_argument(&cursor, extra, sizeof(extra))) {
        terminal_writeln("USAGE: test list [GROUP] | test NAME [cleanup]");
        return;
    }

    if (k_strieq(first, "list")) {
        shell_list_tests(has_second ? second : "");
        return;
    }

    const ShellLocalTest *local = shell_find_local_test(first);
    if (local) {
        if (has_second) {
            terminal_set_color(terminal_error_color());
            terminal_writeln("THIS TEST HAS NO RETAINED CLEANUP ACTION.");
            terminal_set_color(terminal_default_color());
            return;
        }
        (void)shell_run_test_mode(local->run, false);
        return;
    }

    const KernelTest *test = kernel_test_registry_find(first);
    if (!test) {
        terminal_set_color(terminal_error_color());
        terminal_write("UNKNOWN TEST: ");
        terminal_writeln(first);
        terminal_set_color(terminal_default_color());
        terminal_writeln("USE test list FOR AVAILABLE DIAGNOSTICS.");
        return;
    }

    if (!has_second) {
        (void)shell_run_test_mode(test->run, test->live_preemption);
        return;
    }

    if (!k_strieq(second, "cleanup")) {
        terminal_writeln("USAGE: test NAME [cleanup]");
        return;
    }

    if (!test->cleanup) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("THIS TEST HAS NO RETAINED CLEANUP ACTION.");
        terminal_set_color(terminal_default_color());
        return;
    }

    (void)shell_run_test_mode(test->cleanup, test->live_preemption);
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

    const ShellCommand *entry = shell_find_command(command);
    if (!entry) {
        terminal_set_color(terminal_error_color());
        terminal_write("UNKNOWN COMMAND: ");
        terminal_writeln(command);
        terminal_set_color(terminal_default_color());
        terminal_writeln("TYPE help FOR AVAILABLE COMMANDS.");
        return;
    }

    if (entry->with_args) entry->with_args(args);
    else if (entry->no_args) entry->no_args();
}

NORETURN void shell_run(const BootInfo *boot) {
    g_boot = boot;
    g_cwd = vfs_root();
    shell_editor_init(&g_editor);

    prompt();
    terminal_cursor_enable(true);
    terminal_cursor_set_visible(true);

    u64 blink_started = timer_ticks();

    for (;;) {
        KeyEvent event;

        if (!input_poll(&event)) {
            if (timer_initialized()) {
                u32 frequency = timer_frequency();
                u64 interval = frequency >= 2U ? (u64)(frequency / 2U) : 1ULL;
                u64 now = timer_ticks();

                if ((u64)(now - blink_started) >= interval) {
                    terminal_cursor_toggle();
                    blink_started = now;
                }
            }

            arch_pause();
            continue;
        }

        terminal_cursor_set_visible(true);
        blink_started = timer_ticks();

        if (shell_editor_handle(&g_editor, &event) != SHELL_EDITOR_SUBMIT)
            continue;

        terminal_cursor_enable(false);
        terminal_putchar('\n');

        execute((char *)shell_editor_line(&g_editor));

        shell_editor_reset_line(&g_editor);
        prompt();

        terminal_cursor_enable(true);
        terminal_cursor_set_visible(true);
        blink_started = timer_ticks();
    }
}
