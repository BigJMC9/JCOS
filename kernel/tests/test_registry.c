#include "test_registry.h"

#include "capability_test.h"
#include "constructor_test.h"
#include "elf_reclaim_test.h"
#include "force_thread_test.h"
#include "ipc_timeout_order_test.h"
#include "ipc_wait_order_test.h"
#include "lib.h"
#include "peer_death_test.h"
#include "process_terminate_test.h"
#include "lifetime_ipc_acceptance_test.h"
#include "lifetime_stress_test.h"
#include "stack_reclaim_test.h"
#include "user_ipc_cancel_test.h"
#include "user_process_cleanup_test.h"
#include "user_runtime_block_test.h"
#include "user_runtime_test.h"
#include "user_test_fixture.h"
#include "vmm_reclaim_test.h"

#define KERNEL_TEST_CAPACITY 32U

static KernelTest g_tests[KERNEL_TEST_CAPACITY];
static u32 g_test_count;
static bool g_test_registry_initialized;

static void kernel_test_add(const char *name, const char *description, KernelTestGroup group,
    void (*run)(void), void (*cleanup)(void)) {
    if (g_test_count >= KERNEL_TEST_CAPACITY) return;
    KernelTest *test = &g_tests[g_test_count++];
    test->name = name;
    test->description = description;
    test->group = group;
    test->run = run;
    test->cleanup = cleanup;
}

static void kernel_test_registry_init(void) {
    if (g_test_registry_initialized) return;
    g_test_registry_initialized = true;
    kernel_test_add("capability", "capability handles, rights and revoke", KERNEL_TEST_LIFETIME, capability_table_test_run, 0);
    kernel_test_add("capability-lifetime", "capability lifetime and safe storage reuse", KERNEL_TEST_LIFETIME, capability_lifetime_test_run, 0);
    kernel_test_add("constructor", "creation rollback and storage lifetime", KERNEL_TEST_LIFETIME, constructor_test_run, 0);
    kernel_test_add("stack-reclaim", "atomic kernel-stack release and retry", KERNEL_TEST_MEMORY, stack_reclaim_test_run, 0);
    kernel_test_add("elf-reclaim", "ELF cleanup failure and retry", KERNEL_TEST_MEMORY, elf_reclaim_test_run, 0);
    kernel_test_add("vmm-reclaim", "page-table cleanup failure and retry", KERNEL_TEST_MEMORY, vmm_reclaim_test_run, 0);
    kernel_test_add("force-thread", "forced IPC thread termination", KERNEL_TEST_TASK, force_thread_test_run, force_thread_cleanup_run);
    kernel_test_add("published-cleanup", "published fixture cleanup and retry", KERNEL_TEST_LIFETIME, published_cleanup_test_run, force_thread_cleanup_run);
    kernel_test_add("process-kill", "forced multi-thread process termination", KERNEL_TEST_TASK, process_terminate_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("wait-order", "bounded IPC completion ordering", KERNEL_TEST_IPC, ipc_wait_order_test_run, ipc_wait_order_cleanup_run);
    kernel_test_add("peer-death", "owned-endpoint peer-death propagation", KERNEL_TEST_IPC, peer_death_test_run, peer_death_cleanup_run);
    kernel_test_add("timeout", "IPC timeout/deadline ordering", KERNEL_TEST_IPC, ipc_timeout_order_test_run, ipc_timeout_order_cleanup_run);
    kernel_test_add("user-ipc-cancel", "Ring3 IPC cancellation and close", KERNEL_TEST_USERSPACE, user_ipc_cancel_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("user-process-cleanup", "shared user-process cleanup", KERNEL_TEST_USERSPACE, user_process_cleanup_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("user-runtime", "shared Ring3 C runtime", KERNEL_TEST_USERSPACE, user_runtime_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("user-runtime-block", "blocking Ring3 C runtime", KERNEL_TEST_USERSPACE, user_runtime_block_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("lifetime-stress", "IPC/process lifetime recovery stress", KERNEL_TEST_ACCEPTANCE, lifetime_stress_test_run, user_fixture_cleanup_retry_run);
    kernel_test_add("lifetime-ipc-acceptance", "integrated lifetime and IPC robustness gate", KERNEL_TEST_ACCEPTANCE, lifetime_ipc_acceptance_test_run, lifetime_ipc_acceptance_cleanup_run);
}

u32 kernel_test_registry_count(void) {
    kernel_test_registry_init();
    return g_test_count;
}

const KernelTest *kernel_test_registry_at(u32 index) {
    kernel_test_registry_init();
    return index < g_test_count ? &g_tests[index] : 0;
}

const KernelTest *kernel_test_registry_find(const char *name) {
    kernel_test_registry_init();
    if (!name || !*name) return 0;
    for (u32 i = 0; i < g_test_count; ++i) {
        if (k_strieq(name, g_tests[i].name)) return &g_tests[i];
    }
    return 0;
}

const char *kernel_test_group_slug(KernelTestGroup group) {
    switch (group) {
        case KERNEL_TEST_MEMORY: return "memory";
        case KERNEL_TEST_TASK: return "task";
        case KERNEL_TEST_IPC: return "ipc";
        case KERNEL_TEST_LIFETIME: return "lifetime";
        case KERNEL_TEST_USERSPACE: return "userspace";
        case KERNEL_TEST_SCHEDULING: return "scheduling";
        case KERNEL_TEST_ACCEPTANCE: return "acceptance";
        default: return "unknown";
    }
}

const char *kernel_test_group_title(KernelTestGroup group) {
    switch (group) {
        case KERNEL_TEST_MEMORY: return "MEMORY";
        case KERNEL_TEST_TASK: return "TASKS";
        case KERNEL_TEST_IPC: return "IPC";
        case KERNEL_TEST_LIFETIME: return "CAPABILITIES / LIFETIME";
        case KERNEL_TEST_USERSPACE: return "USERSPACE";
        case KERNEL_TEST_SCHEDULING: return "SCHEDULING";
        case KERNEL_TEST_ACCEPTANCE: return "ACCEPTANCE";
        default: return "OTHER";
    }
}
