#include "vmm_reclaim_test.h"
#include "vmm_test.h"
#include "pmm_test.h"
#include "process.h"
#include "thread.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "physmap.h"
#include "lib.h"
#include "test_output.h"

#define TEST_VA ADDRESS_SPACE_USER_BASE
#define OTHER_VA (TEST_VA + 0x200000ULL)
#define TABLE_MASK 0x000FFFFFFFFFF000ULL

/* No published reference points at expiring diagnostic stack storage. */
static Process g_process;
static frame_t g_data = FRAME_INVALID;
static frame_t g_extra = FRAME_INVALID;
static bool g_retained;

static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static u64 *table(frame_t frame) { return phys_to_virt(frame_to_phys(frame)); }

static bool chain(frame_t frames[4]) {
    frames[0] = g_process.owned_address_space.page_map.root_frame;
    u64 *p = table(frames[0]);
    if (!p || !(p[1] & 1ULL)) return false;
    frames[1] = phys_to_frame(p[1] & TABLE_MASK);
    p = table(frames[1]);
    if (!p || !(p[0] & 1ULL)) return false;
    frames[2] = phys_to_frame(p[0] & TABLE_MASK);
    p = table(frames[2]);
    if (!p || !(p[0] & 1ULL)) return false;
    frames[3] = phys_to_frame(p[0] & TABLE_MASK);
    return true;
}

static bool reachable(frame_t target) {
    frame_t f = g_process.owned_address_space.page_map.root_frame;
    if (f == target) return true;
    for (u32 level = 0; level < 3U; ++level) {
        u64 *p = table(f);
        u32 index = level == 0 ? 1U : 0U;
        if (!p || !(p[index] & 1ULL)) return false;
        f = phys_to_frame(p[index] & TABLE_MASK);
        if (f == target) return true;
    }
    return false;
}

static bool mapping(u64 va, frame_t expected) {
    frame_t frame = FRAME_INVALID;
    return address_space_query_page(&g_process.owned_address_space, va, &frame, 0) && frame == expected;
}

static bool no_mapping(u64 va) {
    return !address_space_query_page(&g_process.owned_address_space, va, 0, 0);
}

static bool create_fixture(void) {
    if (g_process.initialized || g_data != FRAME_INVALID || g_extra != FRAME_INVALID) return false;
    if (!process_create(&g_process)) return false;
    g_data = frame_alloc();
    if (g_data == FRAME_INVALID) return false;
    return address_space_map_page(&g_process.owned_address_space, TEST_VA, g_data, VM_WRITE);
}

static bool free_data(void) {
    if (g_data == FRAME_INVALID || !frame_free(g_data)) return false;
    g_data = FRAME_INVALID;
    return true;
}

static u64 shared_fingerprint(void) {
    AddressSpace *space = address_space_kernel();
    u64 *p = space ? table(space->page_map.root_frame) : 0;
    if (!p) return 0;
    u64 hash = 0x514A43534D4D5531ULL;
    for (u32 i = 0; i < VM_PML4_ENTRY_COUNT; ++i) {
        /* Hardware may update accessed bits independently of ownership. */
        hash = (hash << 7) | (hash >> 57);
        hash ^= p[i] & ~(1ULL << 5);
    }
    return hash;
}

static bool prune_failure(u32 level, u64 baseline) {
    if (!check("MAP FIXTURE", create_fixture())) return false;
    AddressSpace *space = &g_process.owned_address_space;
    VmPageMap *map = &space->page_map;
    frame_t frames[4];
    if (!check("TABLE CHAIN", chain(frames))) return false;
    u64 id = g_process.id, space_id = space->id;
    u64 free_before = pmm_stats().free_pages;
    if (!check("LIVE LEAF DESTROY REJECTED", !process_destroy(&g_process) &&
        g_process.id == id && !map->destroy_pending && mapping(TEST_VA, g_data) &&
        pmm_stats().free_pages == free_before)) return false;

    if (!pmm_test_fail_free_range_once(frames[level], 1)) return false;
    frame_t old = FRAME_INVALID;
    bool unmapped = address_space_unmap_page(space, TEST_VA, &old);
    if (!check("UNMAP COMMITTED / TABLE RETAINED", unmapped && old == g_data && no_mapping(TEST_VA) &&
        !pmm_test_free_failure_armed() && reachable(frames[level]) &&
        pmm_test_frame_releasable(frames[level]) && pmm_test_frame_releasable(g_data) &&
        pmm_stats().free_pages == free_before + (3U - level))) return false;

    free_before = pmm_stats().free_pages;
    if (!pmm_test_fail_free_range_once(frames[level], 1)) return false;
    if (!check("DESTROY FAILURE PRESERVES OWNER", !process_destroy(&g_process) &&
        !pmm_test_free_failure_armed() && g_process.initialized && g_process.owns_address_space &&
        g_process.id == id && space->id == space_id && map->root_frame == frames[0] &&
        map->destroy_pending && reachable(frames[level]) && pmm_stats().free_pages == free_before)) return false;
    if (!check("RETIRING MAP NOT REUSED", !address_space_cr3(space) &&
        !address_space_map_page(space, TEST_VA, g_data, VM_WRITE))) return false;
    if (!check("DESTROY RETRY", process_destroy(&g_process) && !g_process.initialized &&
        pmm_test_frame_releasable(g_data))) return false;
    if (!check("DATA FRAME STILL CALLER OWNED", free_data())) return false;
    return check("CASE FRAME BASELINE", pmm_stats().free_pages == baseline);
}

void vmm_reclaim_test_run(void) {
    terminal_writeln("VMM TABLE RECLAIM FAILURE TEST:");
    if (!check("NO RETAINED FIXTURE", !g_retained)) return;
    if (!check("FAULT HOOKS / QUARANTINE IDLE", !vmm_test_faults_armed() &&
        !pmm_test_free_failure_armed() && vmm_test_unlinked_table() == FRAME_INVALID)) return;
    Thread *main = thread_current();
    Process *kernel = process_kernel();
    if (!check("MAIN THREAD", main && kernel && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1 && !scheduler_preemption_enabled())) return;
    u64 reply = 0;
    if (!check("SUPERVISOR BEFORE", supervisor_ping(0x564D4D4245464F52ULL, &reply) &&
        reply == 0x564D4D4245464F52ULL)) return;
    u64 baseline = pmm_stats().free_pages;
    u32 caps = capability_table_count(process_capabilities(kernel));
    u64 kernel_threads = process_thread_count(kernel);
    u64 shared = shared_fingerprint();
    terminal_write("  FREE BEFORE: "); terminal_write_u64(baseline); terminal_putchar('\n');
    g_retained = true;

    AddressSpace *space = &g_process.owned_address_space;
    VmPageMap *map = &space->page_map;
    if (!check("ACTIVE KERNEL MAP NOT DESTROYED", !vmm_page_map_destroy(&address_space_kernel()->page_map))) goto failed;
    terminal_writeln(" PT FAILURE:");
    if (!prune_failure(3U, baseline)) goto failed;
    terminal_writeln(" PD FAILURE:");
    if (!prune_failure(2U, baseline)) goto failed;
    terminal_writeln(" PDPT FAILURE:");
    if (!prune_failure(1U, baseline)) goto failed;

    terminal_writeln(" ROOT FAILURE:");
    if (!process_create(&g_process)) goto failed;
    frame_t root = map->root_frame;
    u64 id = g_process.id;
    if (!pmm_test_fail_free_range_once(root, 1)) goto failed;
    if (!check("ROOT FAILURE PRESERVES PROCESS", !process_destroy(&g_process) &&
        !pmm_test_free_failure_armed() && g_process.id == id && map->root_frame == root &&
        map->destroy_pending && pmm_test_frame_releasable(root))) goto failed;
    if (!check("ROOT RETRY", process_destroy(&g_process) && pmm_stats().free_pages == baseline)) goto failed;

    terminal_writeln(" LIVE SIBLING:");
    if (!create_fixture()) goto failed;
    frame_t frames[4];
    if (!chain(frames)) goto failed;
    g_extra = frame_alloc();
    if (g_extra == FRAME_INVALID || !address_space_map_page(space, OTHER_VA, g_extra, VM_WRITE)) goto failed;
    if (!pmm_test_fail_free_range_once(frames[3], 1)) goto failed;
    frame_t old = FRAME_INVALID;
    if (!address_space_unmap_page(space, TEST_VA, &old) || old != g_data) goto failed;
    if (!check("COLLECT PRESERVES LIVE SIBLING", vmm_page_map_collect(map) &&
        no_mapping(TEST_VA) && mapping(OTHER_VA, g_extra) && pmm_test_frame_releasable(g_extra))) goto failed;
    if (!free_data() || !address_space_unmap_page(space, OTHER_VA, &old) || old != g_extra) goto failed;
    if (!frame_free(g_extra)) goto failed;
    g_extra = FRAME_INVALID;
    if (!check("SIBLING CLEANUP", process_destroy(&g_process) && pmm_stats().free_pages == baseline)) goto failed;

    terminal_writeln(" MAPPING ROLLBACK:");
    if (!process_create(&g_process)) goto failed;
    g_data = frame_alloc();
    if (g_data == FRAME_INVALID) goto failed;
    u64 free_before = pmm_stats().free_pages;
    if (!vmm_test_fail_once(map, VMM_TEST_ALLOC, 2) || !vmm_test_fail_once(map, VMM_TEST_FREE, 0)) goto failed;
    if (!check("FAILED MAP KEEPS TABLE LINKS", !address_space_map_page(space, TEST_VA, g_data, VM_WRITE) &&
        !vmm_test_faults_armed() && no_mapping(TEST_VA) &&
        pmm_stats().free_pages + 2 == free_before && (table(map->root_frame)[1] & 1ULL))) goto failed;
    if (!check("ROLLBACK RETRY", vmm_page_map_collect(map) &&
        !table(map->root_frame)[1] && pmm_stats().free_pages == free_before)) goto failed;
    if (!free_data() || !process_destroy(&g_process)) goto failed;

    terminal_writeln(" UNLINKED TABLE:");
    if (!process_create(&g_process)) goto failed;
    g_data = frame_alloc();
    if (g_data == FRAME_INVALID) goto failed;
    free_before = pmm_stats().free_pages;
    if (!vmm_test_fail_once(map, VMM_TEST_ACCESS, 0) || !vmm_test_fail_once(map, VMM_TEST_FREE, 0)) goto failed;
    bool rejected = !address_space_map_page(space, TEST_VA, g_data, VM_WRITE);
    frame_t retained = vmm_test_unlinked_table();
    if (!check("PRE-LINK ALLOCATION QUARANTINED", rejected && retained != FRAME_INVALID &&
        !vmm_test_faults_armed() && !table(map->root_frame)[1] &&
        pmm_test_frame_releasable(retained) && pmm_stats().free_pages + 1 == free_before)) goto failed;
    if (!check("QUARANTINE IS BOUNDED", !address_space_map_page(space, TEST_VA, g_data, VM_WRITE) &&
        vmm_test_unlinked_table() == retained && pmm_stats().free_pages + 1 == free_before)) goto failed;
    if (!pmm_test_fail_free_range_once(retained, 1)) goto failed;
    if (!check("QUARANTINE FREE FAILURE RETAINED", !vmm_reclaim_unlinked_table() &&
        !pmm_test_free_failure_armed() && vmm_test_unlinked_table() == retained)) goto failed;
    if (!check("QUARANTINE RETRY", vmm_reclaim_unlinked_table() &&
        vmm_test_unlinked_table() == FRAME_INVALID && pmm_stats().free_pages == free_before)) goto failed;
    if (!free_data() || !process_destroy(&g_process)) goto failed;

    if (!check("SHARED KERNEL TABLES UNCHANGED", shared_fingerprint() == shared)) goto failed;
    if (!check("SUPERVISOR AFTER", supervisor_ping(0x564D4D4146544552ULL, &reply) &&
        reply == 0x564D4D4146544552ULL)) goto failed;
    if (!check("PERSISTENT BASELINES", thread_current() == main && scheduler_thread_count() == 1 &&
        process_thread_count(kernel) == kernel_threads &&
        capability_table_count(process_capabilities(kernel)) == caps &&
        !vmm_test_faults_armed() && !pmm_test_free_failure_armed())) goto failed;
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    if (!check("FRAME COUNT RESTORED", pmm_stats().free_pages == baseline)) goto failed;
    g_retained = false;
    test_output_final("VMM TABLE RECLAIM FAILURE TEST", true);
    return;
failed:
    vmm_test_clear_faults();
    pmm_test_clear_free_failure();
    test_output_final("VMM TABLE RECLAIM FAILURE TEST", false);
    terminal_writeln("TEST FIXTURES RETAINED. REBOOT BEFORE FURTHER TESTS.");
}
