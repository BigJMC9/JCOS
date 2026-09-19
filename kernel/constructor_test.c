#include "constructor_test.h"
#include "construction_test.h"
#include "pmm_test.h"
#include "vmm_test.h"
#include "lib.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"

/* A failed diagnostic keeps all live fixture storage until reboot. */
static bool g_retained;
static Process g_process, g_other, g_process_copy;
static Thread g_thread, g_other_thread, g_thread_copy;
static AddressSpace g_space, g_space_copy;

static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool bytes_equal(const void *a, const void *b, usize size) {
    const u8 *left = a, *right = b;
    for (usize i = 0; i < size; ++i) if (left[i] != right[i]) return false;
    return true;
}

static bool all_zero(const void *object, usize size) {
    const u8 *bytes = object;
    for (usize i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

static bool hooks_idle(void) {
    return !thread_test_create_fault_armed() && !address_space_test_create_fault_armed() &&
        !pmm_test_free_failure_armed() && !vmm_test_faults_armed();
}

static bool cleanup_idle(void) {
    return !thread_creation_cleanup_pending() && !address_space_creation_cleanup_pending() &&
        vmm_test_unlinked_table() == FRAME_INVALID;
}

static bool stack_range_owned(frame_t first) {
    if (first == FRAME_INVALID) return false;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        if (!pmm_test_frame_releasable(first + i)) return false;
    }
    return true;
}

/* Exercise a caller that returns immediately after a clean false result. */
static __attribute__((noinline)) bool local_process_failure(void) {
    Process local;
    if (!address_space_test_fail_create_once(&local.owned_address_space,
            SPACE_CREATE_TEST_AFTER_SHARE, true)) return false;
    bool result = !process_create(&local) && all_zero(&local, sizeof(local)) &&
        !address_space_test_create_fault_armed();
    address_space_test_clear_create_fault();
    return result;
}

static __attribute__((noinline)) bool local_thread_failure(Process *owner) {
    Thread local;
    if (!thread_test_fail_create_once(&local, THREAD_CREATE_TEST_ATTACH, true)) return false;
    bool result = !thread_create(&local, owner) && all_zero(&local, sizeof(local)) &&
        !thread_test_create_fault_armed();
    thread_test_clear_create_fault();
    return result;
}

void constructor_test_run(void) {
    terminal_writeln("CONSTRUCTOR ROLLBACK TEST:");
    if (!check("NO RETAINED FIXTURE", !g_retained)) return;
    if (!check("FAULT HOOKS / CLEANUP SLOTS IDLE", hooks_idle() && cleanup_idle())) return;
    Thread *main = thread_current();
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!check("MAIN THREAD", main && kernel && caps && main->process == kernel &&
            main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
            scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled())) return;
    u64 reply = 0;
    if (!check("SUPERVISOR BEFORE", supervisor_ping(0x4354524245464F52ULL, &reply) &&
            reply == 0x4354524245464F52ULL)) return;
    const u64 pid = supervisor_process_id(), tid = supervisor_thread_id();
    const u64 free_before = pmm_stats().free_pages;
    const u64 kernel_threads = process_thread_count(kernel);
    const u32 cap_count = capability_table_count(caps);
    const u32 threads = thread_object_count(), processes = process_object_count();
    const u32 spaces = address_space_object_count();
    terminal_write("  FREE BEFORE: "); terminal_write_u64(free_before); terminal_putchar('\n');
    g_retained = true;

    /* Registry guards must not read uninitialized output structs. */
    k_memset(&g_process, 0xA5, sizeof(g_process));
    if (!check("ALLOCATION-BOUNDARY FAILURE", address_space_test_fail_create_once(
            &g_process.owned_address_space, SPACE_CREATE_TEST_ALLOCATE, false) &&
            !process_create(&g_process) && all_zero(&g_process, sizeof(g_process)) &&
            pmm_stats().free_pages == free_before && cleanup_idle())) goto failed;
    if (!check("PROCESS CLEAN ROLLBACK", address_space_test_fail_create_once(
            &g_process.owned_address_space, SPACE_CREATE_TEST_AFTER_ROOT, false) &&
            !process_create(&g_process) && all_zero(&g_process, sizeof(g_process)) &&
            pmm_stats().free_pages == free_before && cleanup_idle())) goto failed;

    if (!check("RETURNING CALLER / RETAINED ROOT", local_process_failure() &&
            address_space_creation_cleanup_pending() && process_object_count() == processes &&
            address_space_object_count() == spaces && pmm_stats().free_pages + 1ULL == free_before)) goto failed;
    frame_t root = address_space_test_unpublished_root();
    if (!check("ROOT STILL OWNED", pmm_test_frame_releasable(root) && hooks_idle())) goto failed;
    k_memset(&g_other, 0x5A, sizeof(g_other));
    if (!check("ROOT QUARANTINE IS BOUNDED", !process_create(&g_other) &&
            all_zero(&g_other, sizeof(g_other)) && address_space_test_unpublished_root() == root &&
            pmm_stats().free_pages + 1ULL == free_before)) goto failed;
    if (!check("ROOT RETRY FAILURE RETAINED", pmm_test_fail_free_range_once(root, 1ULL) &&
            !address_space_reclaim_unpublished() && address_space_test_unpublished_root() == root &&
            pmm_test_frame_releasable(root))) goto failed;
    if (!check("ROOT RETRY / BASELINE", address_space_reclaim_unpublished() &&
            address_space_reclaim_unpublished() && cleanup_idle() &&
            pmm_stats().free_pages == free_before)) goto failed;

    if (!check("PROCESS CREATE AFTER RETRY", process_create(&g_process))) goto failed;
    k_memcpy(&g_process_copy, &g_process, sizeof(g_process));
    if (!check("LIVE PROCESS NOT REINITIALIZED", !process_create(&g_process) &&
            bytes_equal(&g_process, &g_process_copy, sizeof(g_process)))) goto failed;
    if (!check("COPIED PROCESS NOT DESTROYED", !process_destroy(&g_process_copy) &&
            bytes_equal(&g_process, &g_process_copy, sizeof(g_process)))) goto failed;

    k_memset(&g_space, 0xA5, sizeof(g_space));
    if (!check("ADDRESS-SPACE CREATE", address_space_create(&g_space))) goto failed;
    k_memcpy(&g_space_copy, &g_space, sizeof(g_space));
    if (!check("LIVE / COPIED ADDRESS-SPACE GUARDS", !address_space_create(&g_space) &&
            !address_space_destroy(&g_space_copy) &&
            bytes_equal(&g_space, &g_space_copy, sizeof(g_space)))) goto failed;
    if (!check("ADDRESS-SPACE STORAGE REUSABLE", address_space_destroy(&g_space) &&
            address_space_create(&g_space) && address_space_destroy(&g_space))) goto failed;

    u64 with_process = pmm_stats().free_pages;
    if (!check("THREAD ALLOCATION-BOUNDARY FAILURE", thread_test_fail_create_once(
            &g_thread, THREAD_CREATE_TEST_ALLOCATE, false) && !thread_create(&g_thread, &g_process) &&
            all_zero(&g_thread, sizeof(g_thread)) && pmm_stats().free_pages == with_process)) goto failed;
    if (!check("THREAD CLEAN ROLLBACK", thread_test_fail_create_once(
            &g_thread, THREAD_CREATE_TEST_ACCESS, false) && !thread_create(&g_thread, &g_process) &&
            all_zero(&g_thread, sizeof(g_thread)) && !thread_creation_cleanup_pending() &&
            pmm_stats().free_pages == with_process)) goto failed;
    if (!check("RETURNING CALLER / RETAINED STACK", local_thread_failure(&g_process) &&
            thread_creation_cleanup_pending() && !process_thread_count(&g_process) &&
            thread_object_count() == threads &&
            pmm_stats().free_pages + THREAD_KERNEL_STACK_PAGES == with_process)) goto failed;
    frame_t stack = thread_test_unpublished_stack();
    if (!check("ENTIRE UNPUBLISHED STACK OWNED", stack_range_owned(stack) && hooks_idle())) goto failed;
    if (!check("STACK QUARANTINE IS BOUNDED", !thread_create(&g_other_thread, &g_process) &&
            all_zero(&g_other_thread, sizeof(g_other_thread)) && thread_test_unpublished_stack() == stack &&
            pmm_stats().free_pages + THREAD_KERNEL_STACK_PAGES == with_process)) goto failed;
    if (!check("FAILED THREAD BORROWS NO PROCESS", process_destroy(&g_process) &&
            thread_creation_cleanup_pending() && stack_range_owned(stack))) goto failed;
    if (!check("STACK RETRY FAILURE RETAINED", pmm_test_fail_free_range_once(stack, THREAD_KERNEL_STACK_PAGES) &&
            !thread_reclaim_unpublished_stack() && stack_range_owned(stack))) goto failed;
    if (!check("STACK RETRY / BASELINE", thread_reclaim_unpublished_stack() &&
            thread_reclaim_unpublished_stack() && cleanup_idle() && pmm_stats().free_pages == free_before)) goto failed;

    if (!check("PUBLISHED THREAD FIXTURE", process_create(&g_process) && process_create(&g_other) &&
            thread_create(&g_thread, &g_process))) goto failed;
    k_memcpy(&g_thread_copy, &g_thread, sizeof(g_thread));
    if (!check("LIVE THREAD / CROSS-OWNER REINIT REJECTED", !thread_create(&g_thread, &g_process) &&
            !thread_create(&g_thread, &g_other) &&
            bytes_equal(&g_thread, &g_thread_copy, sizeof(g_thread)) &&
            process_thread_count(&g_process) == 1ULL && !process_thread_count(&g_other))) goto failed;
    if (!check("COPIED THREAD NOT REAPED", !thread_destroy(&g_thread_copy) &&
            bytes_equal(&g_thread, &g_thread_copy, sizeof(g_thread)))) goto failed;
    u64 previous_id = g_thread.id;
    if (!check("PUBLISHED STORAGE REUSE", thread_destroy(&g_thread) &&
            thread_create(&g_thread, &g_process) && g_thread.id == previous_id + 1ULL &&
            thread_destroy(&g_thread))) goto failed;
    if (!check("RETIRING THREAD FIXTURE", thread_create(&g_thread, &g_process))) goto failed;
    stack = phys_to_frame(g_thread.kernel_stack_physical);
    if (!check("RETIRING THREAD RETAINED", pmm_test_fail_free_range_once(stack, THREAD_KERNEL_STACK_PAGES) &&
            !thread_destroy(&g_thread))) goto failed;
    k_memcpy(&g_thread_copy, &g_thread, sizeof(g_thread));
    if (!check("RETIRING THREAD NOT REINITIALIZED", !thread_create(&g_thread, &g_other) &&
            bytes_equal(&g_thread, &g_thread_copy, sizeof(g_thread)) &&
            process_thread_count(&g_process) == 1ULL)) goto failed;
    if (!check("RETIRING THREAD RETRY", thread_destroy(&g_thread))) goto failed;
    if (!check("PROCESS FIXTURES DESTROYED", process_destroy(&g_process) && process_destroy(&g_other))) goto failed;

    /* A failed destructor must keep constructor guards registered too. */
    if (!check("RETIRING PROCESS FIXTURE", process_create(&g_process))) goto failed;
    root = g_process.owned_address_space.page_map.root_frame;
    if (!check("RETIRING PROCESS RETAINED", pmm_test_fail_free_range_once(root, 1ULL) &&
            !process_destroy(&g_process))) goto failed;
    k_memcpy(&g_process_copy, &g_process, sizeof(g_process));
    if (!check("RETIRING PROCESS NOT REINITIALIZED", !process_create(&g_process) &&
            bytes_equal(&g_process, &g_process_copy, sizeof(g_process)))) goto failed;
    if (!check("RETIRING PROCESS RETRY", process_destroy(&g_process))) goto failed;

    reply = 0;
    if (!check("SUPERVISOR AFTER", supervisor_ping(0x4354524146544552ULL, &reply) &&
            reply == 0x4354524146544552ULL && supervisor_process_id() == pid && supervisor_thread_id() == tid)) goto failed;
    if (!check("OBJECT / CAP / SCHEDULER BASELINES", thread_object_count() == threads &&
            process_object_count() == processes && address_space_object_count() == spaces &&
            process_thread_count(kernel) == kernel_threads && capability_table_count(caps) == cap_count &&
            scheduler_thread_count() == 1ULL && thread_current() == main && hooks_idle() && cleanup_idle())) goto failed;
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    if (!check("FRAME COUNT RESTORED", pmm_stats().free_pages == free_before)) goto failed;
    g_retained = false;
    terminal_writeln("CONSTRUCTOR ROLLBACK TEST: PASS");
    return;
failed:
    thread_test_clear_create_fault();
    address_space_test_clear_create_fault();
    pmm_test_clear_free_failure();
    terminal_writeln("CONSTRUCTOR ROLLBACK TEST: FAILED");
    terminal_writeln("TEST FIXTURES RETAINED. REBOOT BEFORE FURTHER TESTS.");
}
