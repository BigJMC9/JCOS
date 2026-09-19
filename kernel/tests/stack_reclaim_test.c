#include "stack_reclaim_test.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "pmm_test.h"
#include "process.h"
#include "scheduler.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "test_output.h"
#include "kernel_stack.h"

/* Never publish references to stack-local fixtures; retain failures for diagnosis. */
static Process g_process;
static Thread g_threads[3];
static Process g_process_snapshot;
static Thread g_thread_snapshot;
static bool g_retained;
static frame_t g_run_first;
static frame_t g_recovered;

static bool check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool bytes_equal(const void *a, const void *b, usize size) {
    const u8 *left = a;
    const u8 *right = b;
    for (usize i = 0; i < size; ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

static bool span_live(frame_t first, u64 count) {
    for (u64 i = 0; i < count; ++i) {
        if (!pmm_test_frame_releasable(first + i)) return false;
    }
    return true;
}

void stack_reclaim_test_run(void) {
    terminal_writeln("STACK RECLAIM FAILURE TEST:");
    if (!check("NO RETAINED FIXTURE", !g_retained)) return;
    if (!check("FAULT HOOK IDLE", !pmm_test_free_failure_armed())) return;

    Thread *main = thread_current();
    Process *kernel = process_kernel();
    bool ready = main && kernel && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();
    if (!check("MAIN THREAD", ready)) return;

    PmmStats before = pmm_stats();
    u64 kernel_threads = process_thread_count(kernel);
    u32 kernel_caps = capability_table_count(process_capabilities(kernel));
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    g_retained = true;
    g_run_first = FRAME_INVALID;
    g_recovered = FRAME_INVALID;
    k_memset(&g_process, 0, sizeof(g_process));
    k_memset(g_threads, 0, sizeof(g_threads));

    bool invalid = !frame_free_range(FRAME_INVALID, 1) &&
        !frame_free_range(0, 0) && !frame_free_range(0, 1) &&
        !frame_free_range(phys_to_frame(before.bitmap_physical), 1);
    if (!check("INVALID/METADATA REJECTED", invalid &&
            pmm_stats().free_pages == before.free_pages)) goto failed;

    u64 physical = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);
    if (!check("RANGE ALLOCATION", physical != 0)) goto failed;
    g_run_first = phys_to_frame(physical);
    frame_t last = g_run_first + THREAD_KERNEL_STACK_PAGES - 1ULL;

    /* A bad final page must not cause the valid prefix to be freed. */
    if (!check("CREATE FINAL-PAGE HOLE", frame_free(last))) goto failed;
    u64 hole_count = pmm_stats().free_pages;
    bool hole_rejected = !frame_free_range(g_run_first, THREAD_KERNEL_STACK_PAGES);
    bool prefix_kept = span_live(g_run_first, THREAD_KERNEL_STACK_PAGES - 1ULL) &&
        !pmm_test_frame_releasable(last) && pmm_stats().free_pages == hole_count;
    if (!check("LATE INVALID PAGE IS ATOMIC", hole_rejected && prefix_kept)) goto failed;
    if (!check("OVERSIZED RANGE REJECTED", !frame_free_range(g_run_first, ~0ULL) &&
            pmm_stats().free_pages == hole_count)) goto failed;

    g_recovered = frame_alloc();
    if (!check("FAILURE PRESERVED ALLOCATOR HINT", g_recovered == last)) goto failed;
    if (!check("RANGE RETRY", frame_free_range(g_run_first, THREAD_KERNEL_STACK_PAGES))) goto failed;
    if (!check("RANGE DOUBLE FREE REJECTED", !frame_free_range(g_run_first, THREAD_KERNEL_STACK_PAGES) &&
            pmm_stats().free_pages == before.free_pages)) goto failed;
    g_run_first = FRAME_INVALID;
    g_recovered = FRAME_INVALID;

    if (!check("PROCESS CREATE", process_create(&g_process))) goto failed;
    for (u32 i = 0; i < ARRAY_COUNT(g_threads); ++i) {
        if (!check("THREAD CREATE", thread_create(&g_threads[i], &g_process))) goto failed;
    }
    Thread *target = &g_threads[1];
    bool list_ok = g_process.thread_head == &g_threads[0] &&
        g_process.thread_tail == &g_threads[2] && g_process.thread_count == 3ULL &&
        process_thread_can_detach(&g_process, target);
    if (!check("THREE-THREAD OWNERSHIP LIST", list_ok)) goto failed;

    bool guards_ok = kernel_stack_arena_ready();
    for (u32 i = 0; guards_ok && i < ARRAY_COUNT(g_threads); ++i) {
        guards_ok = kernel_stack_mapping_valid(
            g_threads[i].kernel_stack_physical, g_threads[i].kernel_stack_base);
    }
    if (!check("GUARDED KERNEL STACK MAPPINGS", guards_ok)) goto failed;

    /* Reap a DEAD middle node, with live ownership links on both sides. */
    if (!check("TARGET STOPPED", task_terminate_thread(target) &&
            target->state == THREAD_STATE_DEAD && !target->on_run_queue)) goto failed;
    frame_t stack_first = phys_to_frame(target->kernel_stack_physical);
    u64 target_stack_base = target->kernel_stack_base;
    u64 stack_pages = target->kernel_stack_size / FRAME_SIZE;
    if (!check("STACK SPAN VALID", stack_pages == THREAD_KERNEL_STACK_PAGES &&
            span_live(stack_first, stack_pages))) goto failed;

    for (u64 i = 0; i < stack_pages; ++i) {
        u64 *word = phys_to_virt(frame_to_phys(stack_first + i));
        if (!word) goto failed;
        *word = 0x534B5245434C0000ULL + i;
    }
    k_memcpy(&g_thread_snapshot, target, sizeof(*target));
    k_memcpy(&g_process_snapshot, &g_process, sizeof(g_process));
    u64 free_before_failure = pmm_stats().free_pages;

    /* Corrupt only a test-owned link, then restore it before any other work. */
    g_threads[2].process_prev = &g_threads[0];
    bool bad_link_rejected = !thread_destroy(target);
    g_threads[2].process_prev = target;
    bool links_unchanged = bytes_equal(target, &g_thread_snapshot, sizeof(*target)) &&
        bytes_equal(&g_process, &g_process_snapshot, sizeof(g_process)) &&
        g_threads[0].process_next == target && span_live(stack_first, stack_pages) &&
        pmm_stats().free_pages == free_before_failure;
    if (!check("BAD LINK REJECTED BEFORE FREE", bad_link_rejected && links_unchanged)) goto failed;

    if (!check("ARM EXACT-RANGE FAILURE", pmm_test_fail_free_range_once(stack_first, stack_pages))) goto failed;
    bool rejected = !thread_destroy(target);
    bool consumed = !pmm_test_free_failure_armed();
    pmm_test_clear_free_failure();
    if (!check("INJECTED RELEASE REJECTED", rejected && consumed)) goto failed;

    bool metadata_kept = bytes_equal(target, &g_thread_snapshot, sizeof(*target)) &&
        bytes_equal(&g_process, &g_process_snapshot, sizeof(g_process)) &&
        g_threads[0].process_next == target && g_threads[2].process_prev == target;
    if (!check("IDENTITY/LIST/OWNERSHIP RETAINED", metadata_kept)) goto failed;
    if (!check("NO STACK FRAME FREED", span_live(stack_first, stack_pages) &&
            pmm_stats().free_pages == free_before_failure)) goto failed;
    if (!check("GUARDS RESTORED AFTER FAILED REAP",
            kernel_stack_mapping_valid(target->kernel_stack_physical, target->kernel_stack_base))) goto failed;

    bool canaries = true;
    for (u64 i = 0; i < stack_pages; ++i) {
        const u64 *word = phys_to_virt(frame_to_phys(stack_first + i));
        if (!word || *word != 0x534B5245434C0000ULL + i) canaries = false;
    }
    if (!check("RETAINED STACK CONTENTS", canaries)) goto failed;
    if (!check("PROCESS DESTROY STILL REJECTED", !process_destroy(&g_process) &&
            g_process.thread_count == 3ULL)) goto failed;

    if (!check("THREAD REAP RETRY", thread_destroy(target))) goto failed;
    bool unlinked = !target->id && !target->process && !target->owns_kernel_stack &&
        g_process.thread_count == 2ULL && g_process.thread_head == &g_threads[0] &&
        g_process.thread_tail == &g_threads[2] &&
        g_threads[0].process_next == &g_threads[2] &&
        g_threads[2].process_prev == &g_threads[0];
    if (!check("MIDDLE NODE UNLINKED ONCE", unlinked)) goto failed;
    if (!check("STACK VIRTUAL SLOT RELEASED", kernel_stack_virtual_released(target_stack_base))) goto failed;
    if (!check("EXACT STACK COUNT RELEASED", pmm_stats().free_pages ==
            free_before_failure + stack_pages)) goto failed;
    if (!check("SECOND DESTROY REJECTED", !thread_destroy(target) &&
            !frame_free_range(stack_first, stack_pages) &&
            pmm_stats().free_pages == free_before_failure + stack_pages)) goto failed;

    if (!check("HEAD/TAIL REAP", thread_destroy(&g_threads[0]) &&
            thread_destroy(&g_threads[2]) && !g_process.thread_head &&
            !g_process.thread_tail && !g_process.thread_count)) goto failed;
    if (!check("PROCESS DESTROY", process_destroy(&g_process))) goto failed;
    if (!check("PERSISTENT BASELINES", thread_current() == main &&
            scheduler_thread_count() == 1ULL && process_thread_count(kernel) == kernel_threads &&
            capability_table_count(process_capabilities(kernel)) == kernel_caps)) goto failed;

    terminal_write("  FREE AFTER: ");
    terminal_write_u64(pmm_stats().free_pages);
    terminal_putchar('\n');
    if (!check("FRAME COUNT RESTORED", pmm_stats().free_pages == before.free_pages)) goto failed;
    g_retained = false;
    test_output_final("STACK RECLAIM FAILURE TEST", true);
    return;

failed:
    pmm_test_clear_free_failure();
    /* Fixtures were never scheduled. Keep all remaining records in static storage. */
    test_output_final("STACK RECLAIM FAILURE TEST", false);
    terminal_writeln("TEST FIXTURES RETAINED. REBOOT BEFORE FURTHER TESTS.");
}
