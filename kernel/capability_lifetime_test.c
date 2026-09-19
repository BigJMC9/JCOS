#include "capability_test.h"
#include "capability.h"
#include "endpoint.h"
#include "process.h"
#include "thread.h"
#include "task.h"
#include "ipc.h"
#include "pmm.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "lib.h"

/* Static records survive an assertion or a rejected destruction. No live copies
 * are ever treated as owners. These commands require the quiet UP test profile. */
static struct {
    CapabilityTable a, b, table_copy;
    Endpoint endpoint, other, endpoint_copy;
    Process process;
    Thread thread;
    CapabilitySlot baseline_slots[CAPABILITY_TABLE_CAPACITY];
    u64 frames, threads, process_count, spaces, endpoints, tables, supervisor_pid, supervisor_tid;
    u32 kernel_caps;
    bool active, a_live, b_live, endpoint_live, other_live, process_live;
} g_test;

static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool supervisor_check(void) {
    u64 reply = 0, cookie = 0x4341504C49464531ULL;
    return supervisor_process_id() == g_test.supervisor_pid && supervisor_thread_id() == g_test.supervisor_tid &&
        supervisor_ping(cookie, &reply) && reply == cookie;
}

static bool begin_test(void) {
    if (!check("NO RETAINED FIXTURE", !g_test.active)) return false;
    Thread *current = thread_current();
    if (!check("MAIN THREAD", current && current->process == process_kernel() &&
        current->state == THREAD_STATE_RUNNING && current->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled())) return false;
    k_memset(&g_test, 0, sizeof(g_test));
    g_test.frames = pmm_stats().free_pages;
    g_test.threads = thread_object_count();
    g_test.process_count = process_object_count();
    g_test.spaces = address_space_object_count();
    g_test.endpoints = endpoint_object_count();
    g_test.tables = capability_table_object_count();
    CapabilityTable *kernel_caps = process_capabilities(process_kernel());
    if (!kernel_caps) return false;
    g_test.kernel_caps = capability_table_count(kernel_caps);
    k_memcpy(g_test.baseline_slots, kernel_caps->slots, sizeof(g_test.baseline_slots));
    g_test.supervisor_pid = supervisor_process_id();
    g_test.supervisor_tid = supervisor_thread_id();
    g_test.active = true;
    terminal_write("  FREE BEFORE: "); terminal_write_u64(g_test.frames); terminal_putchar('\n');
    return check("SUPERVISOR BEFORE", supervisor_check());
}

static bool cleanup_test(void) {
    if (g_test.a_live && !capability_revoke_all(&g_test.a)) return false;
    if (g_test.b_live && !capability_revoke_all(&g_test.b)) return false;
    if (g_test.process_live) {
        if (!task_quiesce_process(&g_test.process) || !process_destroy(&g_test.process)) return false;
        g_test.process_live = false;
    }
    if (g_test.endpoint_live) {
        if (endpoint_message_ready(&g_test.endpoint)) {
            IpcMessage discard;
            if (!endpoint_try_receive(&g_test.endpoint, &discard)) return false;
        }
        if (!endpoint_destroy(&g_test.endpoint)) return false;
        g_test.endpoint_live = false;
    }
    if (g_test.other_live) {
        if (!endpoint_destroy(&g_test.other)) return false;
        g_test.other_live = false;
    }
    if (g_test.a_live) {
        if (!capability_table_destroy(&g_test.a)) return false;
        g_test.a_live = false;
    }
    if (g_test.b_live) {
        if (!capability_table_destroy(&g_test.b)) return false;
        g_test.b_live = false;
    }
    if (!thread_reclaim_unpublished_stack() || !address_space_reclaim_unpublished()) return false;
    CapabilityTable *kernel_caps = process_capabilities(process_kernel());
    if (!kernel_caps || capability_table_count(kernel_caps) != g_test.kernel_caps) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *old = &g_test.baseline_slots[i], *now = &kernel_caps->slots[i];
        if (old->occupied && (!now->occupied || old->object != now->object || old->object_id != now->object_id ||
            old->generation != now->generation || old->rights != now->rights || old->type != now->type)) return false;
    }
    return pmm_stats().free_pages == g_test.frames && thread_object_count() == g_test.threads &&
        process_object_count() == g_test.process_count && address_space_object_count() == g_test.spaces &&
        endpoint_object_count() == g_test.endpoints && capability_table_object_count() == g_test.tables &&
        scheduler_thread_count() == 1ULL && supervisor_check();
}

static void end_test(const char *name, bool passed) {
    bool clean = cleanup_test();
    check("CLEANUP / OBJECT BASELINES", clean);
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    check("FRAME COUNT RESTORED", pmm_stats().free_pages == g_test.frames);
    terminal_write(name); terminal_writeln(passed && clean ? ": PASS" : ": FAILED");
    if (clean) g_test.active = false;
    else terminal_writeln("CAPABILITY FIXTURE RETAINED. REBOOT BEFORE FURTHER TESTS.");
}

static bool stale(const CapabilityTable *table, CapabilityHandle handle, CapabilityType type) {
    void *object = (void *)(u64)1;
    return !capability_lookup(table, handle, type, &object) && !object;
}

void capability_table_test_run(void) {
    terminal_writeln("CAPABILITY TABLE TEST:");
    if (!begin_test()) return;
    bool passed = false;
    CapabilityHandle first = 0, second = 0;
    CapabilityHandle handles[CAPABILITY_TABLE_CAPACITY];

    k_memset(handles, 0, sizeof(handles));
    void *object = 0;
    g_test.a_live = capability_table_init(&g_test.a);
    g_test.endpoint_live = endpoint_create(&g_test.endpoint);
    g_test.other_live = endpoint_create(&g_test.other);
    if (!check("INITIALIZE / EMPTY", g_test.a_live && g_test.endpoint_live && g_test.other_live &&
        capability_table_empty(&g_test.a))) goto done;
    if (!check("INSERT", capability_insert(&g_test.a, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE, &first))) goto done;
    if (!check("LOOKUP / RIGHTS", first && capability_lookup_rights(&g_test.a, first, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE, &object) && object == &g_test.endpoint)) goto done;
    if (!check("WRONG TYPE / MISSING RIGHT REJECTED", stale(&g_test.a, first, CAPABILITY_TYPE_THREAD) &&
        !capability_lookup_rights(&g_test.a, first, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_MANAGE, &object) &&
        !object)) goto done;
    if (!check("REVOKE / STALE HANDLE", capability_revoke(&g_test.a, first) &&
        stale(&g_test.a, first, CAPABILITY_TYPE_ENDPOINT) && !capability_revoke(&g_test.a, first) &&
        !g_test.endpoint.capability_refs)) goto done;
    if (!check("SLOT REUSE / NEW GENERATION", capability_insert(&g_test.a, &g_test.other, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_READ, &second) && (u32)first == (u32)second && first != second &&
        (u32)(first >> 32) != (u32)(second >> 32))) goto done;
    handles[0] = second;
    for (u32 i = 1; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        if (!capability_insert(&g_test.a, &g_test.other, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_READ,
            &handles[i])) goto done;
    }
    CapabilityHandle overflow = 123;
    if (!check("FULL COUNT / ONE PIN PER SLOT", capability_table_count(&g_test.a) == CAPABILITY_TABLE_CAPACITY &&
        g_test.other.capability_refs == CAPABILITY_TABLE_CAPACITY)) goto done;
    if (!check("OVERFLOW REJECTED", !capability_insert(&g_test.a, &g_test.other, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_READ, &overflow) && !overflow && g_test.other.capability_refs == CAPABILITY_TABLE_CAPACITY)) goto done;
    if (!check("REVOKE ALL / FINAL COUNT ZERO", capability_revoke_all(&g_test.a) &&
        capability_table_empty(&g_test.a) && !g_test.other.capability_refs)) goto done;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) if (!stale(&g_test.a, handles[i], CAPABILITY_TYPE_ENDPOINT)) goto done;
    passed = true;
done:
    end_test("CAPABILITY TABLE TEST", passed);
}

void capability_lifetime_test_run(void) {
    terminal_writeln("CAPABILITY OBJECT LIFETIME TEST:");
    if (!begin_test()) return;
    bool passed = false;
    CapabilityHandle a = 0, b = 0, newer = 0, extra = 0, own = 0;
    void *object = 0;
    g_test.a_live = capability_table_init(&g_test.a);
    g_test.b_live = capability_table_init(&g_test.b);
    g_test.endpoint_live = endpoint_create(&g_test.endpoint);
    g_test.other_live = endpoint_create(&g_test.other);
    if (!check("TWO TABLES / TWO ENDPOINTS", g_test.a_live && g_test.b_live &&
        g_test.endpoint_live && g_test.other_live)) goto done;
    if (!check("CROSS-TABLE GRANTS", capability_insert(&g_test.a, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &a) && capability_insert(&g_test.b, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_RECEIVE, &b) && g_test.endpoint.capability_refs == 2ULL)) goto done;
    u64 endpoint_id = g_test.endpoint.id;
    if (!check("LIVE OBJECT / TABLE NOT REINITIALIZED", !endpoint_create(&g_test.endpoint) &&
        !capability_table_init(&g_test.a) && !endpoint_destroy(&g_test.endpoint) &&
        !capability_table_destroy(&g_test.a) && g_test.endpoint.id == endpoint_id &&
        g_test.endpoint.capability_refs == 2ULL)) goto done;
    k_memcpy(&g_test.endpoint_copy, &g_test.endpoint, sizeof(g_test.endpoint));
    k_memcpy(&g_test.table_copy, &g_test.a, sizeof(g_test.a));
    IpcMessage message = { .words = {1, 2, 3, 4}, .word_count = 4U };
    if (!check("COPIED TABLE / OBJECT REJECTED", !endpoint_destroy(&g_test.endpoint_copy) &&
        !endpoint_try_send(&g_test.endpoint_copy, &message) &&
        stale(&g_test.table_copy, a, CAPABILITY_TYPE_ENDPOINT) &&
        !capability_revoke(&g_test.table_copy, a) && !capability_table_destroy(&g_test.table_copy) &&
        !capability_insert(&g_test.a, &g_test.endpoint_copy, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &extra))) goto done;
    if (!check("INVALID OBJECT / RIGHTS REJECTED", !capability_insert(&g_test.a, (void *)(u64)1,
        CAPABILITY_TYPE_THREAD, CAPABILITY_RIGHT_READ, &extra) &&
        !capability_insert(&g_test.a, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT, 1ULL << 63, &extra))) goto done;
    if (!check("DELETE ONE KEEPS OTHER AUTHORITY", capability_revoke(&g_test.a, a) &&
        g_test.endpoint.capability_refs == 1ULL && !endpoint_destroy(&g_test.endpoint) &&
        capability_lookup(&g_test.b, b, CAPABILITY_TYPE_ENDPOINT, &object) && object == &g_test.endpoint)) goto done;
    if (!check("LAST DELETE DOES NOT CLOSE OR DESTROY", capability_revoke(&g_test.b, b) &&
        !g_test.endpoint.capability_refs && endpoint_storage_in_use(&g_test.endpoint) &&
        !endpoint_closed(&g_test.endpoint))) goto done;
    if (!check("EMPTY LIVE TABLE INIT REJECTED", !capability_table_init(&g_test.a))) goto done;
    if (!endpoint_destroy(&g_test.endpoint)) goto done;
    g_test.endpoint_live = false;
    g_test.endpoint_live = endpoint_create(&g_test.endpoint);
    if (!check("OBJECT STORAGE REUSE / OLD HANDLES STALE", g_test.endpoint_live && g_test.endpoint.id != endpoint_id &&
        capability_insert(&g_test.a, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &newer) &&
        newer != a && stale(&g_test.a, a, CAPABILITY_TYPE_ENDPOINT) && stale(&g_test.b, b, CAPABILITY_TYPE_ENDPOINT))) goto done;
    if (!capability_revoke_all(&g_test.a) || !capability_table_destroy(&g_test.a)) goto done;
    g_test.a_live = false;
    g_test.a_live = capability_table_init(&g_test.a);
    if (!check("TABLE STORAGE REUSE / OLD HANDLE STALE", g_test.a_live &&
        capability_insert(&g_test.a, &g_test.endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &a) &&
        a != newer && stale(&g_test.a, newer, CAPABILITY_TYPE_ENDPOINT))) goto done;
    if (!check("CLOSE IS NOT STORAGE DESTRUCTION", ipc_endpoint_close(&g_test.endpoint) &&
        g_test.endpoint.capability_refs == 1ULL && !endpoint_destroy(&g_test.endpoint) &&
        capability_lookup(&g_test.a, a, CAPABILITY_TYPE_ENDPOINT, &object) && object == &g_test.endpoint &&
        !endpoint_try_send(&g_test.endpoint, &message))) goto done;
    if (!check("NO NEW GRANT TO CLOSED ENDPOINT", !capability_insert(&g_test.b, &g_test.endpoint,
        CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &b))) goto done;
    if (!capability_revoke_all(&g_test.a)) goto done;

    /* A bad bulk-delete input must not decrement even the earlier valid pin. */
    if (!capability_insert(&g_test.a, &g_test.other, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_READ, &a) ||
        !capability_insert(&g_test.a, &g_test.other, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_READ, &b)) goto done;
    CapabilityRights saved_rights = g_test.a.slots[(u32)b].rights;
    g_test.a.slots[(u32)b].rights = 0;
    bool rejected = !capability_revoke_all(&g_test.a) && g_test.a.count == 2U &&
        g_test.a.slots[(u32)a].occupied && g_test.other.capability_refs == 2ULL;
    g_test.a.slots[(u32)b].rights = saved_rights;
    if (!check("BULK DELETE VALIDATES BEFORE MUTATION", rejected)) goto done;
    if (!capability_revoke_all(&g_test.a)) goto done;

    g_test.process_live = process_create(&g_test.process);
    if (!check("THREAD / PROCESS FIXTURE", g_test.process_live && thread_create(&g_test.thread, &g_test.process))) goto done;
    CapabilityTable *pcaps = process_capabilities(&g_test.process);
    if (!capability_insert(&g_test.a, &g_test.thread, CAPABILITY_TYPE_THREAD, CAPABILITY_RIGHT_READ, &a) ||
        !capability_insert(pcaps, &g_test.thread, CAPABILITY_TYPE_THREAD, CAPABILITY_RIGHT_READ, &own) ||
        !capability_insert(pcaps, &g_test.other, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &extra)) goto done;
    u64 id = g_test.thread.id, physical = g_test.thread.kernel_stack_physical;
    if (!check("THREAD REAP REJECTED WHILE REFERENCED", !thread_destroy(&g_test.thread) &&
        g_test.thread.id == id && g_test.thread.kernel_stack_physical == physical && g_test.thread.capability_refs == 2ULL)) goto done;
    if (!check("QUIESCE BREAKS SELF-PIN / RETAINS EXTERNAL PIN", !task_quiesce_process(&g_test.process) &&
        g_test.thread.state == THREAD_STATE_DEAD && g_test.thread.id == id && g_test.thread.capability_refs == 1ULL &&
        stale(pcaps, own, CAPABILITY_TYPE_THREAD) && capability_table_count(pcaps) == 1U &&
        capability_lookup(&g_test.a, a, CAPABILITY_TYPE_THREAD, &object) && object == &g_test.thread &&
        !process_destroy(&g_test.process))) goto done;
    if (!check("EXTERNAL RELEASE / QUIESCE RETRY", capability_revoke(&g_test.a, a) &&
        task_quiesce_process(&g_test.process) && !process_thread_count(&g_test.process) &&
        capability_table_empty(pcaps) && !g_test.thread.id)) goto done;
    AddressSpace *space = process_address_space(&g_test.process);
    if (!capability_insert(&g_test.b, space, CAPABILITY_TYPE_ADDRESS_SPACE, CAPABILITY_RIGHT_READ, &b)) goto done;
    id = g_test.process.id;
    if (!check("ADDRESS-SPACE PIN RETAINS PROCESS", !address_space_destroy(space) &&
        !process_destroy(&g_test.process) && g_test.process.id == id && space->capability_refs == 1ULL &&
        capability_table_storage_in_use(pcaps))) goto done;
    if (!capability_revoke(&g_test.b, b) || !process_destroy(&g_test.process)) goto done;
    g_test.process_live = false;
    if (!check("LAST SPACE PIN RELEASE / PROCESS DESTROY", !capability_table_storage_in_use(pcaps))) goto done;
    if (!check("GENERATION BOUNDARY NEVER WRAPS (LOCAL COUNTER)", capability_test_generation_boundary())) goto done;
    passed = true;
done:
    end_test("CAPABILITY OBJECT LIFETIME TEST", passed);
}
