#include "user_test_fixture.h"

#include "construction_test.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "pmm_test.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "user_elf_test.h"
#include "vmm_test.h"
#include "user_stack.h"

static UserTestFixture g_fixture;
static bool g_last_result;

static u64 fixture_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void fixture_irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }
static bool canonical(const UserTestFixture *f) { return f == &g_fixture && f->active; }
static bool mutable(const UserTestFixture *f) { return canonical(f) && !f->cleanup_started && !f->quiesced; }

static void print_check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool main_ready(void) {
    Thread *main = thread_current();
    Process *kernel = process_kernel();
    return main && kernel && main->process == kernel && main->state == THREAD_STATE_RUNNING &&
        main->on_run_queue && scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();
}

bool user_fixture_hooks_idle(void) {
    return !pmm_test_free_failure_armed() && !user_elf_test_faults_armed() && !vmm_test_faults_armed() &&
        !thread_test_create_fault_armed() && !address_space_test_create_fault_armed();
}

static bool rollback_slots_idle(void) {
    return !thread_creation_cleanup_pending() && !address_space_creation_cleanup_pending() &&
        vmm_test_unlinked_table() == FRAME_INVALID;
}

bool user_fixture_busy(void) { return g_fixture.active; }
bool user_fixture_last_result(void) { return g_last_result; }

static void report_retained(void) {
    terminal_writeln("USER TEST FIXTURE RETAINED. DO NOT RUN OTHER TESTS.");
    terminal_write("  OWNER: "); terminal_writeln(g_fixture.label);
    terminal_write("  CLEANUP STAGE: "); terminal_write_u64(g_fixture.cleanup_stage);
    terminal_write("  INDEX: "); terminal_write_u64(g_fixture.cleanup_index); terminal_putchar('\n');
    terminal_writeln("RUN usercleanupretry. IF IT FAILS, REBOOT.");
}

bool user_fixture_available(void) {
    if (g_fixture.active) { report_retained(); return false; }
    if (!main_ready() || !user_fixture_hooks_idle() || !rollback_slots_idle()) {
        terminal_writeln("USER TEST PREFLIGHT: FAILED (state, hooks, or rollback slot)");
        return false;
    }
    return true;
}

UserTestFixture *user_fixture_begin(const char *label) {
    if (g_fixture.active || !label || !user_fixture_available()) return 0;
    u64 flags = fixture_irq_save();
    if (g_fixture.active || !main_ready()) { fixture_irq_restore(flags); return 0; }
    k_memset(&g_fixture, 0, sizeof(g_fixture));
    UserTestFixture *f = &g_fixture;
    f->main_thread = thread_current();
    f->kernel_process = process_kernel();
    f->kernel_caps = process_capabilities(f->kernel_process);
    if (!f->kernel_caps || !f->kernel_caps->initialized) { fixture_irq_restore(flags); return 0; }
    for (u32 i = 0; i + 1U < sizeof(f->label) && label[i]; ++i) f->label[i] = label[i];
    for (u32 i = 0; i < USER_FIXTURE_STACKS; ++i) f->stacks[i].frame = FRAME_INVALID;
    k_memcpy(f->kernel_slots, f->kernel_caps->slots, sizeof(f->kernel_slots));
    f->kernel_cap_count = capability_table_count(f->kernel_caps);
    f->free_pages = pmm_stats().free_pages;
    f->process_count = process_object_count();
    f->thread_count = thread_object_count();
    f->space_count = address_space_object_count();
    f->kernel_thread_count = process_thread_count(f->kernel_process);
    f->kernel_head = f->kernel_process->thread_head;
    f->kernel_tail = f->kernel_process->thread_tail;
    f->supervisor_pid = supervisor_process_id();
    f->supervisor_tid = supervisor_thread_id();
    f->checks_ok = true;
    f->active = true;
    g_last_result = false;
    fixture_irq_restore(flags);
    return f;
}

void user_fixture_set_quiet(UserTestFixture *f, bool quiet) { if (canonical(f)) f->quiet = quiet; }
void user_fixture_check(const char *name, bool pass) {
    if (g_fixture.active && !pass) g_fixture.checks_ok = false;
    print_check(name, pass);
}

static void note_unpublished(UserTestFixture *f) {
    /* Begin requires empty slots; only this serialized test constructs objects. */
    if (thread_creation_cleanup_pending()) f->unpublished_stack = true;
    if (address_space_creation_cleanup_pending()) f->unpublished_space = true;
    if (vmm_test_unlinked_table() != FRAME_INVALID) f->unlinked_table = true;
}

bool user_fixture_process_create(UserTestFixture *f) {
    if (!mutable(f) || f->process_created) return false;
    bool result = process_create(&f->process);
    if (result) f->process_created = true;
    else note_unpublished(f);
    return result;
}

bool user_fixture_endpoint_create(UserTestFixture *f, u32 i) {
    if (!mutable(f) || i >= USER_FIXTURE_ENDPOINTS || f->endpoint_created[i]) return false;
    if (!endpoint_create(&f->endpoints[i])) return false;
    f->endpoint_created[i] = true;
    return true;
}

bool user_fixture_grant(UserTestFixture *f, bool kernel, u32 endpoint,
    CapabilityRights rights, CapabilityHandle *out) {
    if (!mutable(f) || !out || endpoint >= USER_FIXTURE_ENDPOINTS || !f->endpoint_created[endpoint]) return false;
    *out = CAPABILITY_INVALID_HANDLE;
    CapabilityTable *table = kernel ? f->kernel_caps : process_capabilities(&f->process);
    if (!table) return false;
    for (u32 i = 0; i < USER_FIXTURE_GRANTS; ++i) {
        UserTestGrant *grant = &f->grants[i];
        if (grant->owned) continue;
        CapabilityHandle handle = CAPABILITY_INVALID_HANDLE;
        if (!capability_insert(table, &f->endpoints[endpoint], CAPABILITY_TYPE_ENDPOINT, rights, &handle)) return false;
        grant->handle = handle;
        grant->rights = rights;
        grant->endpoint = endpoint;
        grant->kernel = kernel;
        grant->owned = true;
        *out = handle;
        return true;
    }
    return false;
}

bool user_fixture_load(UserTestFixture *f, const VfsNode *file) {
    if (!mutable(f) || !f->process_created || user_elf_needs_cleanup(&f->image)) return false;
    bool result = user_elf_load(&f->process, file, &f->image);
    note_unpublished(f);
    return result;
}

bool user_fixture_stack_allocate(UserTestFixture *f, u32 i, u64 address) {
    if (!mutable(f) || !f->process_created || i >= USER_FIXTURE_STACKS || f->stacks[i].allocated ||
        address < ADDRESS_SPACE_USER_BASE || address > ADDRESS_SPACE_USER_LIMIT - VM_PAGE_SIZE ||
        (address & (VM_PAGE_SIZE - 1ULL))) return false;
    AddressSpace *space = process_address_space(&f->process);
    if (!space || !user_stack_slot_available(space, address)) return false;
    for (u32 j = 0; j < USER_FIXTURE_STACKS; ++j) {
        if (!f->stacks[j].allocated) continue;
        u64 other = f->stacks[j].virtual_address;
        u64 distance = address > other ? address - other : other - address;
        if (distance < 2ULL * VM_PAGE_SIZE) return false;
    }
    frame_t frame = frame_alloc();
    if (frame == FRAME_INVALID) return false;
    f->stacks[i].virtual_address = address;
    f->stacks[i].frame = frame;
    f->stacks[i].allocated = true;
    return true;
}

bool user_fixture_stack_map(UserTestFixture *f, u32 i) {
    if (!mutable(f) || i >= USER_FIXTURE_STACKS) return false;
    UserTestStack *stack = &f->stacks[i];
    if (!stack->allocated || stack->mapped) return false;
    AddressSpace *space = process_address_space(&f->process);
    void *direct = phys_to_virt(frame_to_phys(stack->frame));
    if (!space || !direct) return false;
    k_memset(direct, 0, (usize)VM_PAGE_SIZE);
    bool result = user_stack_map_page(space, stack->virtual_address, stack->frame);
    if (result) stack->mapped = true;
    else note_unpublished(f);
    return result;
}

bool user_fixture_stack_create(UserTestFixture *f, u32 i, u64 address, u64 arg0, u64 arg1) {
    if (!user_fixture_stack_allocate(f, i, address) || !user_fixture_stack_map(f, i)) return false;
    u8 *direct = (u8 *)phys_to_virt(frame_to_phys(f->stacks[i].frame));
    if (!direct) return false;
    u64 *startup = (u64 *)(void *)(direct + VM_PAGE_SIZE - 2ULL * sizeof(u64));
    startup[0] = arg0; startup[1] = arg1;
    return true;
}

u64 user_fixture_stack_rsp(const UserTestFixture *f, u32 i) {
    if (!canonical(f) || i >= USER_FIXTURE_STACKS || !f->stacks[i].mapped) return 0;
    return f->stacks[i].virtual_address + VM_PAGE_SIZE - 2ULL * sizeof(u64);
}

bool user_fixture_thread_create(UserTestFixture *f, u32 i) {
    if (!mutable(f) || !f->process_created || i >= USER_FIXTURE_THREADS || f->thread_created[i]) return false;
    bool result = thread_create(&f->threads[i], &f->process);
    if (result) f->thread_created[i] = true;
    else note_unpublished(f);
    return result;
}

bool user_fixture_schedule_once(void) {
    u64 flags = fixture_irq_save();
    bool result = scheduler_yield();
    fixture_irq_restore(flags);
    return result;
}

static bool execution_owner_ok(const UserTestFixture *f) {
    return canonical(f) && thread_current() == f->main_thread && f->main_thread->process == f->kernel_process &&
        f->main_thread->state == THREAD_STATE_RUNNING && f->main_thread->on_run_queue &&
        !scheduler_preemption_enabled();
}

bool user_fixture_quiesce(UserTestFixture *f) {
    if (!execution_owner_ok(f)) return false;
    if (f->quiesced) return true;
    f->cleanup_started = true; /* No new publication after a teardown attempt. */
    u64 flags = fixture_irq_save();
    bool result = !f->process_created || task_quiesce_process(&f->process);
    for (u32 i = 0; i < USER_FIXTURE_THREADS; ++i) {
        if (f->thread_created[i] && !f->threads[i].id) f->thread_created[i] = false;
        if (f->thread_created[i]) result = false;
    }
    if (result && f->process_created) {
        CapabilityTable *caps = process_capabilities(&f->process);
        result = caps && !capability_table_count(caps) && !process_thread_count(&f->process) &&
            !f->process.thread_head && !f->process.thread_tail;
    }
    if (result) {
        for (u32 i = 0; i < USER_FIXTURE_GRANTS; ++i) {
            if (!f->grants[i].kernel) f->grants[i].owned = false;
        }
        f->quiesced = true;
    }
    fixture_irq_restore(flags);
    return result;
}

bool user_fixture_pause_once(UserTestFixture *f, UserFixturePause point, u32 index) {
    if (!canonical(f) || point <= USER_FIXTURE_PAUSE_NONE || point > USER_FIXTURE_BEFORE_RELEASE ||
        f->pause != USER_FIXTURE_PAUSE_NONE) return false;
    f->pause = point; f->pause_index = index;
    return true;
}
static bool paused(UserTestFixture *f, UserFixturePause point, u32 index) {
    if (f->pause != point || f->pause_index != index) return false;
    f->pause = USER_FIXTURE_PAUSE_NONE;
    return true;
}

static bool stack_release(UserTestFixture *f, u32 i) {
    UserTestStack *stack = &f->stacks[i];
    if (!stack->allocated) return !stack->mapped;
    if (stack->mapped) {
        AddressSpace *space = process_address_space(&f->process);
        frame_t found = FRAME_INVALID, old = FRAME_INVALID;
        if (!space || !user_stack_mapping_valid(space, stack->virtual_address, stack->frame)) return false;
        if (!address_space_query_page(space, stack->virtual_address, &found, 0) || found != stack->frame) return false;
        if (!address_space_unmap_page(space, stack->virtual_address, &old)) return false;
        stack->mapped = false;
        if (old != stack->frame) return false;
        if (paused(f, USER_FIXTURE_AFTER_STACK_UNMAP, i)) return false;
    }
    if (!frame_free(stack->frame)) return false;
    stack->allocated = false;
    stack->frame = FRAME_INVALID;
    if (paused(f, USER_FIXTURE_AFTER_STACK_FREE, i)) return false;
    return true;
}

static bool resources_empty(const UserTestFixture *f) {
    if (f->process_created || user_elf_needs_cleanup(&f->image) ||
        f->unpublished_stack || f->unpublished_space || f->unlinked_table) return false;
    for (u32 i = 0; i < USER_FIXTURE_THREADS; ++i) if (f->thread_created[i] || f->threads[i].id) return false;
    for (u32 i = 0; i < USER_FIXTURE_ENDPOINTS; ++i) if (f->endpoint_created[i]) return false;
    for (u32 i = 0; i < USER_FIXTURE_STACKS; ++i) if (f->stacks[i].allocated || f->stacks[i].mapped) return false;
    for (u32 i = 0; i < USER_FIXTURE_GRANTS; ++i) if (f->grants[i].owned) return false;
    return true;
}

static bool cleanup_locked(UserTestFixture *f) {
    f->cleanup_started = true;
    f->last_cleanup_ok = false;
    f->cleanup_stage = 1; f->cleanup_index = 0;
    if (!user_fixture_quiesce(f)) return false;
    if (paused(f, USER_FIXTURE_AFTER_QUIESCE, 0)) return false;

    f->cleanup_stage = 2;
    if (f->unpublished_stack) {
        if (!thread_reclaim_unpublished_stack()) return false;
        f->unpublished_stack = false;
    }
    if (f->unpublished_space) {
        if (!address_space_reclaim_unpublished()) return false;
        f->unpublished_space = false;
    }
    if (f->unlinked_table) {
        if (!vmm_reclaim_unlinked_table()) return false;
        f->unlinked_table = false;
    }

    f->cleanup_stage = 3;
    for (u32 i = 0; i < USER_FIXTURE_STACKS; ++i) {
        f->cleanup_index = i;
        if (!stack_release(f, i)) return false;
    }
    f->cleanup_stage = 4; f->cleanup_index = 0;
    if (user_elf_needs_cleanup(&f->image) && !user_elf_unload(&f->process, &f->image)) return false;
    if (paused(f, USER_FIXTURE_AFTER_ELF, 0)) return false;

    f->cleanup_stage = 5;
    for (u32 i = 0; i < USER_FIXTURE_ENDPOINTS; ++i) {
        f->cleanup_index = i;
        if (!f->endpoint_created[i]) continue;
        Endpoint *endpoint = &f->endpoints[i];
        if (endpoint->waiting_receiver || endpoint->waiting_sender || endpoint->waiting_sender_message_ready) return false;
        if (endpoint_message_ready(endpoint)) {
            if (endpoint_closed(endpoint)) return false;
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(endpoint, &discard) || endpoint_message_ready(endpoint)) return false;
        }
    }
    f->cleanup_stage = 6;
    for (u32 i = 0; i < USER_FIXTURE_GRANTS; ++i) {
        f->cleanup_index = i;
        UserTestGrant *grant = &f->grants[i];
        if (!grant->owned) continue;
        if (!grant->kernel || grant->endpoint >= USER_FIXTURE_ENDPOINTS) return false;
        void *object = 0;
        if (!capability_lookup_rights(f->kernel_caps, grant->handle, CAPABILITY_TYPE_ENDPOINT,
                grant->rights, &object) || object != &f->endpoints[grant->endpoint]) return false;
        if (!capability_revoke(f->kernel_caps, grant->handle)) return false;
        grant->owned = false;
        if (paused(f, USER_FIXTURE_AFTER_KERNEL_CAP, i)) return false;
    }
    f->cleanup_stage = 7;
    for (u32 i = 0; i < USER_FIXTURE_ENDPOINTS; ++i) {
        f->cleanup_index = i;
        if (!f->endpoint_created[i]) continue;
        if (paused(f, USER_FIXTURE_BEFORE_ENDPOINT, i)) return false;
        if (!endpoint_destroy(&f->endpoints[i])) return false;
        f->endpoint_created[i] = false;
    }
    f->cleanup_stage = 8; f->cleanup_index = 0;
    if (f->process_created) {
        if (paused(f, USER_FIXTURE_BEFORE_PROCESS, 0)) return false;
        if (!process_destroy(&f->process)) return false;
        f->process_created = false;
    }
    f->last_cleanup_ok = resources_empty(f);
    return f->last_cleanup_ok;
}

bool user_fixture_cleanup(UserTestFixture *f) {
    if (!execution_owner_ok(f)) return false;
    u64 flags = fixture_irq_save();
    bool result = cleanup_locked(f);
    fixture_irq_restore(flags);
    return result;
}

bool user_fixture_baselines(const UserTestFixture *f) {
    if (!canonical(f) || !resources_empty(f) || !main_ready() || thread_current() != f->main_thread ||
        process_kernel() != f->kernel_process || !user_fixture_hooks_idle() || !rollback_slots_idle() ||
        pmm_stats().free_pages != f->free_pages || process_object_count() != f->process_count ||
        thread_object_count() != f->thread_count || address_space_object_count() != f->space_count ||
        process_thread_count(f->kernel_process) != f->kernel_thread_count ||
        f->kernel_process->thread_head != f->kernel_head || f->kernel_process->thread_tail != f->kernel_tail ||
        capability_table_count(f->kernel_caps) != f->kernel_cap_count) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *before = &f->kernel_slots[i], *now = &f->kernel_caps->slots[i];
        if (before->occupied != now->occupied) return false;
        if (before->occupied && (before->object != now->object || before->generation != now->generation ||
            before->type != now->type || before->rights != now->rights)) return false;
    }
    return true;
}

bool user_fixture_supervisor_ok(const UserTestFixture *f) {
    if (!canonical(f) || !main_ready() || !f->supervisor_pid || !f->supervisor_tid ||
        supervisor_process_id() != f->supervisor_pid || supervisor_thread_id() != f->supervisor_tid) return false;
    u64 cookie = 0x5546495854555245ULL, reply = 0;
    return supervisor_ping(cookie, &reply) && reply == cookie &&
        supervisor_process_id() == f->supervisor_pid && supervisor_thread_id() == f->supervisor_tid;
}

bool user_fixture_finish(UserTestFixture *f, bool behavior_completed) {
    if (!canonical(f)) return false;
    bool expected = f->suppress_expected_retention_report;
    f->suppress_expected_retention_report = false;
    if (!behavior_completed) f->checks_ok = false;
    bool cleaned = user_fixture_cleanup(f);
    bool baseline = cleaned && user_fixture_baselines(f);
    bool healthy = baseline && user_fixture_supervisor_ok(f);
    baseline = baseline && healthy && user_fixture_baselines(f);
    bool released = baseline && !paused(f, USER_FIXTURE_BEFORE_RELEASE, 0) && f->pause == USER_FIXTURE_PAUSE_NONE;
    if ((!f->quiet || !released) && !expected) {
        print_check("SHARED PROCESS CLEANUP", cleaned);
        print_check("SUPERVISOR / IDENTITY", healthy);
        print_check("PERSISTENT BASELINES", baseline);
        terminal_write("  FREE BEFORE: "); terminal_write_u64(f->free_pages); terminal_putchar('\n');
        terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    }
    g_last_result = released && behavior_completed && f->checks_ok;
    if (released) f->active = false;
    else if (!expected) report_retained();
    return g_last_result;
}

void user_fixture_cleanup_retry_run(void) {
    terminal_writeln("USER FIXTURE CLEANUP RETRY:");
    if (!g_fixture.active) { print_check("NOTHING RETAINED", true); return; }
    if (!user_fixture_hooks_idle()) {
        terminal_writeln("FAULT HOOK STILL ARMED. PRESERVE OUTPUT AND REBOOT.");
        return;
    }
    /* Recovery never reclassifies the failed original run as a passing test. */
    g_fixture.checks_ok = false;
    g_fixture.pause = USER_FIXTURE_PAUSE_NONE;
    (void)user_fixture_finish(&g_fixture, false);
    print_check("RETAINED RESOURCES RELEASED", !g_fixture.active);
    terminal_writeln("ORIGINAL TEST WAS NOT RERUN.");
}
