#include "user_process_cleanup_test.h"
#include "user_test_fixture.h"
#include "construction_test.h"
#include "ipc.h"
#include "lib.h"
#include "pmm_test.h"
#include "scheduler.h"
#include "terminal.h"
#include "user_elf_test.h"
#include "vfs.h"
#include "../include/runtime_cancel_test_abi.h"

#define CLEANUP_TEST_STACK0 (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define CLEANUP_TEST_STACK1 (ADDRESS_SPACE_USER_BASE + 0x102000ULL)
#define CLEANUP_TEST_STEPS 12U

/* Only the shared fixture publishes kernel objects. All locals below are values. */
static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static bool setup(UserTestFixture *f, const VfsNode *file, u32 stop_after) {
    if (!user_fixture_process_create(f)) return false;
    if (stop_after == 1U) return true;
    if (!user_fixture_endpoint_create(f, 0) || !user_fixture_endpoint_create(f, 1)) return false;
    if (stop_after == 2U) return true;
    CapabilityHandle handle;
    if (!user_fixture_grant(f, true, 0, CAPABILITY_RIGHT_SEND, &handle) ||
        !user_fixture_grant(f, true, 0, CAPABILITY_RIGHT_RECEIVE, &handle) ||
        !user_fixture_grant(f, true, 1, CAPABILITY_RIGHT_SEND, &handle) ||
        !user_fixture_grant(f, true, 1, CAPABILITY_RIGHT_RECEIVE, &handle) ||
        !user_fixture_grant(f, false, 0, CAPABILITY_RIGHT_RECEIVE, &handle) ||
        !user_fixture_grant(f, false, 1, CAPABILITY_RIGHT_SEND, &handle)) return false;
    if (stop_after == 3U) return true;
    if (!user_fixture_load(f, file)) return false;
    if (stop_after == 4U) return true;
    if (!user_fixture_stack_create(f, 0, CLEANUP_TEST_STACK0, f->grants[4].handle, JCOS_RTC_MODE_RECEIVE)) return false;
    if (stop_after == 5U) return true;
    if (!user_fixture_stack_create(f, 1, CLEANUP_TEST_STACK1, f->grants[5].handle, JCOS_RTC_MODE_SEND)) return false;
    if (stop_after == 6U) return true;
    if (!user_fixture_thread_create(f, 0)) return false;
    if (stop_after == 7U) return true;
    if (!user_fixture_thread_create(f, 1)) return false;
    if (stop_after == 8U) return true;
    if (!thread_prepare_user(&f->threads[0], f->image.entry, user_fixture_stack_rsp(f, 0)) ||
        !thread_prepare_user(&f->threads[1], f->image.entry, user_fixture_stack_rsp(f, 1)) ||
        !scheduler_add(&f->threads[0])) return false;
    if (stop_after == 9U) return true;
    if (!scheduler_add(&f->threads[1])) return false;
    if (stop_after == 10U) return true;
    IpcMessage prefill = { .words = { JCOS_RTC_PREFILL_WORD0, JCOS_RTC_PREFILL_WORD1,
        JCOS_RTC_PREFILL_WORD2, JCOS_RTC_PREFILL_WORD3 }, .word_count = 4U };
    if (!ipc_try_send(f->kernel_process, f->grants[2].handle, &prefill) || !user_fixture_schedule_once()) return false;
    if (f->threads[0].state != THREAD_STATE_BLOCKED || f->threads[1].state != THREAD_STATE_BLOCKED ||
        !thread_wait_matches(&f->threads[0], THREAD_WAIT_IPC_RECEIVE, &f->endpoints[0]) ||
        !thread_wait_matches(&f->threads[1], THREAD_WAIT_IPC_SEND, &f->endpoints[1]) ||
        !f->endpoints[1].waiting_sender_message_ready || scheduler_thread_count() != 1ULL) return false;
    if (stop_after == 11U) return true;
    IpcMessage wake = { .words = { JCOS_RTC_SEND_WORD0, JCOS_RTC_SEND_WORD1,
        JCOS_RTC_SEND_WORD2, JCOS_RTC_SEND_WORD3 }, .word_count = 4U };
    return ipc_try_send(f->kernel_process, f->grants[0].handle, &wake) &&
        f->threads[0].state == THREAD_STATE_READY && f->threads[0].on_run_queue &&
        f->threads[1].state == THREAD_STATE_BLOCKED && scheduler_thread_count() == 2ULL;
}

static UserTestFixture *begin(const VfsNode *file, u32 step) {
    UserTestFixture *f = user_fixture_begin("USER PROCESS CLEANUP TEST");
    if (!f) return 0;
    user_fixture_set_quiet(f, true);
    if (!setup(f, file, step)) {
        /* A failed setup is never reclassified as a successful acceptance case. */
        (void)user_fixture_finish(f, false);
        return 0;
    }
    return f;
}

static bool retained_unchanged(UserTestFixture *f) {
    u64 process_id = f->process.id;
    u32 count = f->image.page_count;
    bool created = f->process_created;
    return user_fixture_busy() && !user_fixture_begin("OTHER CALLER") &&
        f->active && f->process.id == process_id && f->process_created == created && f->image.page_count == count;
}

static bool failed_reap(const VfsNode *file) {
    UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
    if (!f) return false;
    u64 id = f->threads[1].id;
    frame_t frame = phys_to_frame(f->threads[1].kernel_stack_physical);
    u64 count = f->threads[1].kernel_stack_size / FRAME_SIZE;
    u64 before = pmm_stats().free_pages;
    if (!check("PARTIAL REAP FAILURE", pmm_test_fail_free_range_once(frame, count) &&
            !user_fixture_cleanup(f) && !pmm_test_free_failure_armed() && !f->threads[0].id &&
            f->threads[1].id == id && f->threads[1].state == THREAD_STATE_DEAD &&
            process_thread_count(&f->process) == 1ULL && pmm_stats().free_pages == before + THREAD_KERNEL_STACK_PAGES &&
            f->image.loaded && f->stacks[0].mapped && f->stacks[1].mapped && f->grants[0].owned &&
            !thread_wait_active(&f->threads[1]))) return false;
    if (!check("REMAINING THREAD / DEPENDENCIES RETAINED", retained_unchanged(f) &&
            pmm_test_fail_free_range_once(frame, count) && !user_fixture_cleanup(f) &&
            f->threads[1].id == id && process_thread_count(&f->process) == 1ULL &&
            capability_table_count(process_capabilities(&f->process)) == 2U)) return false;
    return check("PARTIAL REAP RETRY", user_fixture_finish(f, true));
}

static bool failed_stack(const VfsNode *file) {
    UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
    if (!f) return false;
    frame_t frame = f->stacks[0].frame;
    if (!check("USER STACK FREE FAILURE", pmm_test_fail_free_range_once(frame, 1) && !user_fixture_cleanup(f) &&
            f->quiesced && !f->threads[0].id && !f->threads[1].id && f->stacks[0].allocated &&
            !f->stacks[0].mapped && f->stacks[0].frame == frame && f->stacks[1].mapped &&
            f->image.loaded && pmm_test_frame_releasable(frame))) return false;
    if (!check("UNMAP PROGRESS RETAINED ON RETRY", retained_unchanged(f) &&
            pmm_test_fail_free_range_once(frame, 1) && !user_fixture_cleanup(f) &&
            !f->stacks[0].mapped && f->stacks[0].allocated)) return false;
    return check("USER STACK RETRY", user_fixture_finish(f, true));
}

static bool failed_elf(const VfsNode *file) {
    UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
    if (!f || !f->image.page_count) return false;
    frame_t frame = f->image.pages[f->image.page_count - 1U].frame;
    if (!check("ELF RELEASE FAILURE", pmm_test_fail_free_range_once(frame, 1) && !user_fixture_cleanup(f) &&
            !f->stacks[0].allocated && !f->stacks[1].allocated && user_elf_needs_cleanup(&f->image) &&
            f->image.cleanup_pending && f->image.owner == &f->process && f->process.elf_image == &f->image &&
            !process_destroy(&f->process) && retained_unchanged(f))) return false;
    return check("CANONICAL ELF LEDGER RETRY", user_fixture_finish(f, true));
}

static bool failed_root(const VfsNode *file) {
    UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
    if (!f) return false;
    frame_t root = f->process.owned_address_space.page_map.root_frame;
    u64 id = f->process.id;
    if (!check("ROOT RELEASE FAILURE", pmm_test_fail_free_range_once(root, 1) && !user_fixture_cleanup(f) &&
            f->process_created && f->process.id == id && f->process.owned_address_space.page_map.destroy_pending &&
            !user_elf_needs_cleanup(&f->image) && !f->endpoint_created[0] && !f->endpoint_created[1] &&
            capability_table_count(f->kernel_caps) == f->kernel_cap_count && retained_unchanged(f))) return false;
    return check("PROCESS / ROOT RETRY", user_fixture_finish(f, true));
}

static bool failed_load(const VfsNode *file) {
    UserTestFixture *f = begin(file, 3U);
    if (!f) return false;
    if (!check("FAILED LOAD RETAINS ACTUAL OWNERSHIP",
            user_elf_test_fail_once(&f->image, USER_ELF_TEST_ALLOC, ADDRESS_SPACE_USER_BASE + 2ULL * VM_PAGE_SIZE) &&
            user_elf_test_fail_once(&f->image, USER_ELF_TEST_UNMAP, ADDRESS_SPACE_USER_BASE) &&
            !user_fixture_load(f, file) && !f->image.loaded && user_elf_needs_cleanup(&f->image) &&
            !process_destroy(&f->process) && retained_unchanged(f))) return false;
    return check("FAILED LOAD CLEANUP (NOT loaded FLAG)", user_fixture_finish(f, true));
}

static bool failed_constructors(const VfsNode *file) {
    UserTestFixture *f = user_fixture_begin("USER PROCESS CLEANUP TEST");
    if (!f) return false;
    user_fixture_set_quiet(f, true);
    if (!check("UNPUBLISHED ROOT OWNERSHIP",
            address_space_test_fail_create_once(&f->process.owned_address_space, SPACE_CREATE_TEST_AFTER_ROOT, true) &&
            !user_fixture_process_create(f) && !f->process_created && f->unpublished_space &&
            address_space_creation_cleanup_pending() && user_fixture_finish(f, true))) return false;
    f = begin(file, 3U);
    if (!f) return false;
    if (!check("UNPUBLISHED STACK OWNERSHIP",
            thread_test_fail_create_once(&f->threads[0], THREAD_CREATE_TEST_ACCESS, true) &&
            !user_fixture_thread_create(f, 0) && !f->thread_created[0] && f->unpublished_stack &&
            thread_creation_cleanup_pending() && user_fixture_finish(f, true))) return false;
    return true;
}

static bool pause_case(const VfsNode *file, UserFixturePause point, u32 index) {
    UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
    if (!f || !user_fixture_pause_once(f, point, index)) return false;
    if (point == USER_FIXTURE_BEFORE_RELEASE) f->suppress_expected_retention_report = true;
    bool stopped = point == USER_FIXTURE_BEFORE_RELEASE ? !user_fixture_finish(f, true) : !user_fixture_cleanup(f);
    if (!stopped || f->pause != USER_FIXTURE_PAUSE_NONE || !retained_unchanged(f)) return false;
    return user_fixture_finish(f, true);
}

void user_process_cleanup_test_run(void) {
    terminal_writeln("USER PROCESS FIXTURE CLEANUP TEST:");
    if (!user_fixture_available()) return;
    VfsNode *file = vfs_resolve(vfs_root(), "/bin/runtimecanceltest.elf");
    if (!check("ELF FILE", file && file->type == VFS_FILE && file->data && file->size)) return;
    u64 before = pmm_stats().free_pages;
    bool pass = true;
    for (u32 i = 1; i <= CLEANUP_TEST_STEPS; ++i) {
        UserTestFixture *f = begin(file, i);
        bool result = f && user_fixture_cleanup(f) && user_fixture_cleanup(f) && user_fixture_finish(f, true);
        terminal_write("  SETUP ABORT "); terminal_write_u64(i); terminal_write(": ");
        terminal_writeln(result ? "PASS" : "FAILED");
        if (!result) { pass = false; goto done; }
    }
    if (!failed_reap(file) || !failed_stack(file) || !failed_elf(file) || !failed_root(file) ||
        !failed_load(file) || !failed_constructors(file)) { pass = false; goto done; }

    for (u32 i = 0; i < 10U; ++i) {
        UserFixturePause point = USER_FIXTURE_AFTER_QUIESCE;
        u32 index = 0;
        if (i == 1U) point = USER_FIXTURE_AFTER_STACK_UNMAP;
        if (i == 2U) { point = USER_FIXTURE_AFTER_STACK_UNMAP; index = 1; }
        if (i == 3U) point = USER_FIXTURE_AFTER_STACK_FREE;
        if (i == 4U) point = USER_FIXTURE_AFTER_ELF;
        if (i == 5U) point = USER_FIXTURE_AFTER_KERNEL_CAP;
        if (i == 6U) { point = USER_FIXTURE_AFTER_KERNEL_CAP; index = 2; }
        if (i == 7U) { point = USER_FIXTURE_BEFORE_ENDPOINT; index = 1; }
        if (i == 8U) point = USER_FIXTURE_BEFORE_PROCESS;
        if (i == 9U) point = USER_FIXTURE_BEFORE_RELEASE;
        bool result = pause_case(file, point, index);
        terminal_write("  PARTIAL CLEANUP "); terminal_write_u64(i + 1U); terminal_write(": ");
        terminal_writeln(result ? "PASS" : "FAILED");
        if (!result) { pass = false; goto done; }
    }
    {
        UserTestFixture *f = begin(file, CLEANUP_TEST_STEPS);
        if (!f || !user_fixture_pause_once(f, USER_FIXTURE_AFTER_ELF, 0)) { pass = false; goto done; }
        f->suppress_expected_retention_report = true;
        if (user_fixture_finish(f, false) || !user_fixture_busy()) { pass = false; goto done; }
        user_fixture_cleanup_retry_run();
        if (!check("RECOVERY DOES NOT TURN FAILED TEST INTO PASS", !user_fixture_busy() &&
                !user_fixture_last_result())) { pass = false; goto done; }
    }
done:
    pass = pass && !user_fixture_busy() && user_fixture_hooks_idle() && pmm_stats().free_pages == before;
    terminal_write("  FREE BEFORE: "); terminal_write_u64(before); terminal_putchar('\n');
    terminal_write("  FREE AFTER: "); terminal_write_u64(pmm_stats().free_pages); terminal_putchar('\n');
    check("FRAME COUNT RESTORED", pmm_stats().free_pages == before);
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER PROCESS FIXTURE CLEANUP TEST: "); terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
    if (user_fixture_busy()) terminal_writeln("FIXTURE RETAINED. RUN usercleanupretry; REBOOT IF RETRY FAILS.");
}
