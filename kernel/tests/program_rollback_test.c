#include "program_rollback_test.h"

#include "construction_test.h"
#include "endpoint.h"
#include "pmm_test.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "user_elf_test.h"
#include "vmm_test.h"

static ProgramInstance g_rollback;
static bool g_space_hook_owned;
static bool g_thread_hook_owned;
static bool g_free_hook_owned;
static bool g_vmm_hooks_owned;

typedef struct {
    u64 free_pages;
    u64 scheduler_count;
    u64 kernel_threads;
    u64 supervisor_pid;
    u64 supervisor_tid;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 tables;
    u32 kernel_caps;
} RollbackBaseline;

static bool slots_idle(void) {
    return !address_space_creation_cleanup_pending() && !thread_creation_cleanup_pending() &&
        !vmm_unlinked_table_cleanup_pending();
}

static bool hooks_idle(void) {
    return !address_space_test_create_fault_armed() && !thread_test_create_fault_armed() &&
        !pmm_test_free_failure_armed() && !user_elf_test_faults_armed() && !vmm_test_faults_armed();
}

static void clear_owned_hooks(void) {
    if (g_space_hook_owned) address_space_test_clear_create_fault();
    if (g_thread_hook_owned) thread_test_clear_create_fault();
    if (g_free_hook_owned) pmm_test_clear_free_failure();
    if (g_vmm_hooks_owned) vmm_test_clear_faults();
    g_space_hook_owned = false;
    g_thread_hook_owned = false;
    g_free_hook_owned = false;
    g_vmm_hooks_owned = false;
}

bool program_rollback_test_cleanup(void) {
    clear_owned_hooks();
    return !program_instance_needs_cleanup(&g_rollback) || program_terminate(&g_rollback);
}

static void capture(RollbackBaseline *baseline) {
    Process *kernel = process_kernel();
    baseline->free_pages = pmm_stats().free_pages;
    baseline->scheduler_count = scheduler_thread_count();
    baseline->kernel_threads = process_thread_count(kernel);
    baseline->supervisor_pid = supervisor_process_id();
    baseline->supervisor_tid = supervisor_thread_id();
    baseline->processes = process_object_count();
    baseline->spaces = address_space_object_count();
    baseline->threads = thread_object_count();
    baseline->endpoints = endpoint_object_count();
    baseline->tables = capability_table_object_count();
    baseline->kernel_caps = capability_table_count(process_capabilities(kernel));
}

static bool baseline_matches(const RollbackBaseline *baseline) {
    Process *kernel = process_kernel();
    return kernel && pmm_stats().free_pages == baseline->free_pages &&
        scheduler_thread_count() == baseline->scheduler_count &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        supervisor_process_id() == baseline->supervisor_pid &&
        supervisor_thread_id() == baseline->supervisor_tid &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        capability_table_object_count() == baseline->tables &&
        capability_table_count(process_capabilities(kernel)) == baseline->kernel_caps &&
        slots_idle() && hooks_idle() && !scheduler_preemption_enabled();
}

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_writeln(pass ? ": PASS" : ": FAILED");
}

static bool constructor_case(const ProgramLaunchSpec *spec, const RollbackBaseline *baseline,
    bool thread_constructor, u32 fault, bool fail_rollback) {
    bool armed;
    if (thread_constructor) {
        armed = thread_test_fail_create_once(&g_rollback.thread, (ThreadCreateTestFault)fault, fail_rollback);
        g_thread_hook_owned = armed;
    } else {
        armed = address_space_test_fail_create_once(&g_rollback.process.owned_address_space,
            (SpaceCreateTestFault)fault, fail_rollback);
        g_space_hook_owned = armed;
    }
    /* The constructor may arm its own exact-range PMM failure for this case. */
    g_free_hook_owned = armed && fail_rollback;

    u64 launches = program_launch_count();
    bool rejected = armed && !program_launch(&g_rollback, spec);
    bool pass = rejected && !program_instance_needs_cleanup(&g_rollback) &&
        program_launch_count() == launches && baseline_matches(baseline);
    clear_owned_hooks();

    terminal_write("  LAUNCH ");
    terminal_write(thread_constructor ? "THREAD" : "ADDRESS-SPACE");
    terminal_write(" CONSTRUCTOR ");
    terminal_write_u64(fault);
    terminal_write(fail_rollback ? " / FAILED LOWER ROLLBACK" : " / NORMAL LOWER ROLLBACK");
    terminal_writeln(pass ? ": PASS" : ": FAILED");
    return pass;
}

static bool unlinked_table_case(const ProgramLaunchSpec *spec, const RollbackBaseline *baseline) {
    /* The private root constructor uses its own scratch map. These exact-map
     * hooks instead reach the first user ELF mapping in the published space. */
    VmPageMap *map = &g_rollback.process.owned_address_space.page_map;
    bool access = vmm_test_fail_once(map, VMM_TEST_ACCESS, 0U);
    g_vmm_hooks_owned = access;
    bool free_fault = access && vmm_test_fail_once(map, VMM_TEST_FREE, 0U);
    u64 launches = program_launch_count();
    bool rejected = free_fault && !program_launch(&g_rollback, spec);
    bool pass = rejected && !program_instance_needs_cleanup(&g_rollback) &&
        program_launch_count() == launches && baseline_matches(baseline);
    clear_owned_hooks();
    report("LAUNCH UNLINKED VMM TABLE / FAILED LOWER ROLLBACK", pass);
    return pass;
}

static bool release_retry(const ProgramLaunchSpec *spec, const RollbackBaseline *baseline, bool root) {
    bool launched = program_launch(&g_rollback, spec);
    AddressSpace *space = launched ? process_address_space(&g_rollback.process) : 0;
    frame_t target = !launched || !space ? FRAME_INVALID :
        (root ? space->page_map.root_frame : g_rollback.stack_frame);
    bool armed = target != FRAME_INVALID && pmm_test_fail_free_range_once(target, 1ULL);
    g_free_hook_owned = armed;

    bool retained = launched && armed && !program_terminate(&g_rollback) &&
        program_instance_needs_cleanup(&g_rollback) && g_rollback.process_created &&
        !g_rollback.thread.id && !g_rollback.stack_mapped && !pmm_test_free_failure_armed() &&
        (root ? !g_rollback.stack_frame_allocated : g_rollback.stack_frame_allocated);
    report(root ? "LAUNCH ROOT RELEASE FAILURE RETAINED" : "LAUNCH STACK RELEASE FAILURE RETAINED", retained);
    clear_owned_hooks();
    if (!retained) return false;

    bool retried = program_terminate(&g_rollback) && !program_instance_needs_cleanup(&g_rollback) &&
        baseline_matches(baseline);
    report(root ? "LAUNCH ROOT RELEASE RETRY / BASELINE" : "LAUNCH STACK RELEASE RETRY / BASELINE", retried);
    return retried;
}

bool program_rollback_test_run(const ProgramLaunchSpec *spec) {
    if (!spec || !program_rollback_test_cleanup() || !slots_idle() || !hooks_idle() ||
        scheduler_preemption_enabled() || scheduler_thread_count() != 1ULL) {
        report("LAUNCH ROLLBACK PREFLIGHT", false);
        return false;
    }

    RollbackBaseline baseline;
    capture(&baseline);
    bool pass = false;

    for (u32 fault = SPACE_CREATE_TEST_ALLOCATE; fault <= SPACE_CREATE_TEST_AFTER_SHARE; ++fault) {
        if (!constructor_case(spec, &baseline, false, fault, false)) goto done;
        if (fault != SPACE_CREATE_TEST_ALLOCATE &&
            !constructor_case(spec, &baseline, false, fault, true)) goto done;
    }
    for (u32 fault = THREAD_CREATE_TEST_ALLOCATE; fault <= THREAD_CREATE_TEST_ATTACH; ++fault) {
        if (!constructor_case(spec, &baseline, true, fault, false)) goto done;
        if (fault != THREAD_CREATE_TEST_ALLOCATE &&
            !constructor_case(spec, &baseline, true, fault, true)) goto done;
    }
    if (!unlinked_table_case(spec, &baseline)) goto done;
    if (!release_retry(spec, &baseline, false) || !release_retry(spec, &baseline, true)) goto done;
    pass = true;

done: {
        bool cleaned = program_rollback_test_cleanup();
        bool restored = cleaned && baseline_matches(&baseline);
        report("LAUNCH ROLLBACK CLEANUP / BASELINE", restored);
        return pass && restored;
    }
}
