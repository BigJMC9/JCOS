#include "elf_reclaim_test.h"
#include "user_elf.h"
#include "user_elf_test.h"
#include "pmm_test.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "test_output.h"

#define ELF_TEST_BASE ADDRESS_SPACE_USER_BASE
#define ELF_TEST_DATA (ELF_TEST_BASE + 2ULL * VM_PAGE_SIZE)
#define ELF_TEST_TAIL (ELF_TEST_BASE + 3ULL * VM_PAGE_SIZE)

/* Static fixtures retain ownership even if a diagnostic assertion fails. */
static Process g_process;
static Process g_other;
static UserElfImage g_image;
static UserElfImage g_snapshot;
static UserElfImage g_copy;
static UserElfImage g_fresh;
static Thread g_guard;
static VfsNode g_file;
static u8 g_bytes[272];
static u8 g_bytes_snapshot[272];
static frame_t g_extra = FRAME_INVALID;
static frame_t g_displaced = FRAME_INVALID;
static bool g_retained;

static bool check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool same_bytes(const void *left, const void *right, usize count) {
    const u8 *a = left;
    const u8 *b = right;
    for (usize i = 0; i < count; ++i) if (a[i] != b[i]) return false;
    return true;
}

static void put16(u32 offset, u16 value) {
    g_bytes[offset] = (u8)value;
    g_bytes[offset + 1U] = (u8)(value >> 8);
}

static void put32(u32 offset, u32 value) {
    for (u32 i = 0; i < 4U; ++i) g_bytes[offset + i] = (u8)(value >> (8U * i));
}

static void put64(u32 offset, u64 value) {
    for (u32 i = 0; i < 8U; ++i) g_bytes[offset + i] = (u8)(value >> (8U * i));
}

/* Data-only fixture for the real loader; no test Thread executes these bytes. */
static void make_fixture(void) {
    k_memset(&g_file, 0, sizeof(g_file));
    k_memset(g_bytes, 0, sizeof(g_bytes));
    g_bytes[0] = 0x7f; g_bytes[1] = 'E'; g_bytes[2] = 'L'; g_bytes[3] = 'F';
    g_bytes[4] = 2; g_bytes[5] = 1; g_bytes[6] = 1;
    put16(16, 2); put16(18, 62); put32(20, 1);
    put64(24, ELF_TEST_BASE); put64(32, 64);
    put16(52, 64); put16(54, 56); put16(56, 2);
    put32(64, 1); put32(68, 5);
    put64(72, 256); put64(80, ELF_TEST_BASE);
    put64(96, 8); put64(104, VM_PAGE_SIZE); put64(112, 1);
    put32(120, 1); put32(124, 6);
    put64(128, 264); put64(136, ELF_TEST_DATA);
    put64(152, 8); put64(160, 2ULL * VM_PAGE_SIZE); put64(168, 1);
    put64(256, 0x454C465445585431ULL);
    put64(264, 0x454C464441544131ULL);
    g_file.type = VFS_FILE;
    g_file.data = g_bytes;
    g_file.size = sizeof(g_bytes);
    k_memcpy(g_bytes_snapshot, g_bytes, sizeof(g_bytes));
}

static bool mapping_is(u64 address, frame_t expected) {
    frame_t frame = FRAME_INVALID;
    return address_space_query_page(process_address_space(&g_process), address, &frame, 0) &&
        frame == expected;
}

static bool no_mapping(u64 address) {
    return !address_space_query_page(process_address_space(&g_process), address, 0, 0);
}

static bool load_fixture(void) {
    return user_elf_load(&g_process, &g_file, &g_image) && g_image.loaded &&
        !g_image.cleanup_pending && g_image.page_count == 3U &&
        g_process.elf_image == &g_image;
}

static bool cleanup_to(u64 baseline) {
    return user_elf_unload(&g_process, &g_image) &&
        !user_elf_needs_cleanup(&g_image) && !g_process.elf_image &&
        pmm_stats().free_pages == baseline;
}

static bool image_frames_owned(void) {
    for (u32 i = 0; i < g_image.page_count; ++i) {
        if (!pmm_test_frame_releasable(g_image.pages[i].frame)) return false;
    }
    return true;
}

void elf_reclaim_test_run(void) {
    terminal_writeln("ELF RECLAIM FAILURE TEST:");
    if (!check("NO RETAINED FIXTURE", !g_retained)) return;
    if (!check("FAULT HOOKS IDLE", !user_elf_test_faults_armed() &&
        !pmm_test_free_failure_armed())) return;

    Thread *main = thread_current();
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!check("MAIN THREAD", main && kernel && caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled())) return;

    u64 reply = 0;
    if (!check("SUPERVISOR BEFORE", supervisor_ping(0x454C464245464F52ULL, &reply) &&
        reply == 0x454C464245464F52ULL)) return;
    u64 supervisor_pid = supervisor_process_id();
    u64 supervisor_tid = supervisor_thread_id();
    u32 caps_before = capability_table_count(caps);
    u64 threads_before = process_thread_count(kernel);
    u64 free_before = pmm_stats().free_pages;
    terminal_write("  FREE BEFORE: "); terminal_write_u64(free_before); terminal_putchar('\n');

    g_retained = true;
    make_fixture();
    if (!check("PROCESS FIXTURES", process_create(&g_process) && process_create(&g_other))) goto failed;
    u64 empty_baseline = pmm_stats().free_pages;
    AddressSpace *space = process_address_space(&g_process);

    if (!check("THREE-PAGE ELF LOAD", load_fixture())) goto failed;
    const u64 *data = phys_to_virt(frame_to_phys(g_image.pages[1].frame));
    const u64 *bss = phys_to_virt(frame_to_phys(g_image.pages[2].frame));
    if (!check("FILE DATA / BSS", data && bss && data[0] == 0x454C464441544131ULL && bss[0] == 0)) goto failed;
    k_memcpy(&g_snapshot, &g_image, sizeof(g_image));
    k_memcpy(&g_copy, &g_image, sizeof(g_image));
    u64 loaded_count = pmm_stats().free_pages;
    bool protected = !user_elf_load(&g_process, &g_file, &g_image) &&
        !user_elf_load(&g_process, &g_file, &g_fresh) &&
        !user_elf_unload(&g_other, &g_image) && !user_elf_unload(&g_process, &g_copy) &&
        !process_destroy(&g_process) && same_bytes(&g_snapshot, &g_image, sizeof(g_image)) &&
        pmm_stats().free_pages == loaded_count;
    if (!check("OWNER / COPY / RELOAD GUARDS", protected)) goto failed;
    k_memset(&g_copy, 0, sizeof(g_copy));

    if (!check("UNPREPARED THREAD CREATE", thread_create(&g_guard, &g_process))) goto failed;
    if (!check("UNLOAD WITH THREAD REJECTED", !user_elf_unload(&g_process, &g_image) &&
        same_bytes(&g_snapshot, &g_image, sizeof(g_image)))) goto failed;
    if (!check("GUARD THREAD REAP", thread_destroy(&g_guard))) goto failed;

    if (!check("ARM UNMAP FAILURE", user_elf_test_fail_once(&g_image, USER_ELF_TEST_UNMAP, ELF_TEST_TAIL))) goto failed;
    bool rejected = !user_elf_unload(&g_process, &g_image);
    if (!check("MAPPED PAGE RETAINED", rejected && !user_elf_test_faults_armed() &&
        !g_image.loaded && !g_image.entry && g_image.cleanup_pending && g_image.page_count == 3U &&
        g_image.pages[2].mapped && mapping_is(ELF_TEST_TAIL, g_image.pages[2].frame) &&
        image_frames_owned() && pmm_stats().free_pages == loaded_count)) goto failed;
    if (!check("PENDING PROCESS DESTROY REJECTED", !process_destroy(&g_process))) goto failed;
    if (!check("UNMAP RETRY", cleanup_to(empty_baseline))) goto failed;
    if (!check("DOUBLE UNLOAD REJECTED", !user_elf_unload(&g_process, &g_image) &&
        pmm_stats().free_pages == empty_baseline)) goto failed;

    if (!check("RELOAD FOR FREE FAILURE", load_fixture())) goto failed;
    frame_t retained = g_image.pages[1].frame;
    frame_t released = g_image.pages[2].frame;
    if (!check("ARM REAL PMM FREE FAILURE", pmm_test_fail_free_range_once(retained, 1))) goto failed;
    rejected = !user_elf_unload(&g_process, &g_image);
    if (!check("PARTIAL PROGRESS RETAINED", rejected && !pmm_test_free_failure_armed() &&
        g_image.page_count == 2U && !g_image.pages[1].mapped &&
        g_image.pages[1].frame == retained && no_mapping(ELF_TEST_DATA) &&
        !pmm_test_frame_releasable(released) && image_frames_owned() &&
        g_image.cleanup_pending && !process_destroy(&g_process))) goto failed;
    k_memcpy(&g_snapshot, &g_image, sizeof(g_image));
    u64 retained_count = pmm_stats().free_pages;
    if (!check("PENDING RELOAD PRESERVES LEDGER", !user_elf_load(&g_process, &g_file, &g_image) &&
        same_bytes(&g_snapshot, &g_image, sizeof(g_image)))) goto failed;
    if (!check("ARM SECOND FREE FAILURE", pmm_test_fail_free_range_once(retained, 1))) goto failed;
    if (!check("RETRY DOES NOT UNMAP AGAIN", !user_elf_unload(&g_process, &g_image) &&
        !pmm_test_free_failure_armed() && pmm_stats().free_pages == retained_count &&
        same_bytes(&g_snapshot, &g_image, sizeof(g_image)))) goto failed;

    /* A reused VA is not the image's mapping once its mapped bit is cleared. */
    g_extra = frame_alloc();
    if (!check("REUSE UNMAPPED VIRTUAL ADDRESS", g_extra != FRAME_INVALID &&
        address_space_map_page(space, ELF_TEST_DATA, g_extra, VM_WRITE))) goto failed;
    if (!check("FREE RETRY PRESERVES NEW MAPPING", user_elf_unload(&g_process, &g_image) &&
        !user_elf_needs_cleanup(&g_image) && mapping_is(ELF_TEST_DATA, g_extra))) goto failed;
    frame_t old = FRAME_INVALID;
    if (!check("EXTRA MAPPING CLEANUP", address_space_unmap_page(space, ELF_TEST_DATA, &old) &&
        old == g_extra && frame_free(g_extra))) goto failed;
    g_extra = FRAME_INVALID;
    if (!check("FREE RETRY BASELINE", pmm_stats().free_pages == empty_baseline)) goto failed;

    if (!check("RELOAD FOR MAPPING MISMATCH", load_fixture())) goto failed;
    g_displaced = g_image.pages[2].frame;
    g_extra = frame_alloc();
    old = FRAME_INVALID;
    if (!check("INSTALL FOREIGN MAPPING", g_extra != FRAME_INVALID &&
        address_space_unmap_page(space, ELF_TEST_TAIL, &old) && old == g_displaced &&
        address_space_map_page(space, ELF_TEST_TAIL, g_extra, VM_WRITE))) goto failed;
    u64 mismatch_count = pmm_stats().free_pages;
    if (!check("MISMATCH NOT UNMAPPED OR FREED", !user_elf_unload(&g_process, &g_image) &&
        mapping_is(ELF_TEST_TAIL, g_extra) && g_image.page_count == 3U &&
        g_image.pages[2].mapped && image_frames_owned() &&
        pmm_stats().free_pages == mismatch_count)) goto failed;
    old = FRAME_INVALID;
    if (!check("RESTORE OWNED MAPPING", address_space_unmap_page(space, ELF_TEST_TAIL, &old) &&
        old == g_extra && address_space_map_page(space, ELF_TEST_TAIL, g_displaced, VM_WRITE) &&
        frame_free(g_extra))) goto failed;
    g_extra = FRAME_INVALID; g_displaced = FRAME_INVALID;
    if (!check("MISMATCH RETRY", cleanup_to(empty_baseline))) goto failed;

    if (!check("ARM LOAD / ROLLBACK FAILURES",
        user_elf_test_fail_once(&g_image, USER_ELF_TEST_ALLOC, ELF_TEST_TAIL) &&
        user_elf_test_fail_once(&g_image, USER_ELF_TEST_UNMAP, ELF_TEST_DATA))) goto failed;
    rejected = !user_elf_load(&g_process, &g_file, &g_image);
    if (!check("FAILED LOAD RETAINS MAPPED PAGES", rejected && !user_elf_test_faults_armed() &&
        g_image.page_count == 2U && g_image.pages[0].mapped && g_image.pages[1].mapped &&
        g_image.cleanup_pending && !g_image.loaded && image_frames_owned() &&
        g_process.elf_image == &g_image && !process_destroy(&g_process))) goto failed;
    if (!check("FAILED-LOAD CLEANUP RETRY", cleanup_to(empty_baseline))) goto failed;

    if (!check("ARM MAP / FREE FAILURES",
        user_elf_test_fail_once(&g_image, USER_ELF_TEST_MAP, ELF_TEST_BASE) &&
        user_elf_test_fail_once(&g_image, USER_ELF_TEST_FREE, ELF_TEST_BASE))) goto failed;
    rejected = !user_elf_load(&g_process, &g_file, &g_image);
    if (!check("PRE-MAP ALLOCATION RETAINED", rejected && !user_elf_test_faults_armed() &&
        g_image.page_count == 1U && !g_image.pages[0].mapped && g_image.cleanup_pending &&
        no_mapping(ELF_TEST_BASE) && image_frames_owned() && !process_destroy(&g_process))) goto failed;
    if (!check("PRE-MAP CLEANUP RETRY", cleanup_to(empty_baseline))) goto failed;

    if (!check("SOURCE FILE UNCHANGED", same_bytes(g_bytes, g_bytes_snapshot, sizeof(g_bytes)))) goto failed;
    if (!check("PROCESS FIXTURES DESTROYED", process_destroy(&g_process) && process_destroy(&g_other))) goto failed;
    reply = 0;
    bool survivor = supervisor_ping(0x454C464146544552ULL, &reply) && reply == 0x454C464146544552ULL &&
        supervisor_process_id() == supervisor_pid && supervisor_thread_id() == supervisor_tid;
    if (!check("SUPERVISOR AFTER", survivor)) goto failed;
    if (!check("PERSISTENT BASELINES", thread_current() == main && scheduler_thread_count() == 1ULL &&
        capability_table_count(caps) == caps_before && process_thread_count(kernel) == threads_before)) goto failed;
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    if (!check("FRAME COUNT RESTORED", pmm_stats().free_pages == free_before)) goto failed;
    g_retained = false;
    test_output_final("ELF RECLAIM FAILURE TEST", true);
    return;

failed:
    user_elf_test_clear_faults();
    pmm_test_clear_free_failure();
    test_output_final("ELF RECLAIM FAILURE TEST", false);
    terminal_writeln("TEST FIXTURES RETAINED. REBOOT BEFORE FURTHER TESTS.");
}
