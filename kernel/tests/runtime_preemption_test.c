#include "runtime_preemption_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "user_stack.h"
#include "vfs.h"
#include "vmm.h"

#define RUNTIME_PREEMPT_PATH "/bin/runtimetest.elf"
#define RUNTIME_PREEMPT_TEXT ADDRESS_SPACE_USER_BASE
#define RUNTIME_PREEMPT_DATA (ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE)
#define RUNTIME_PREEMPT_COOKIE 0x505245454D50544FULL

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool setup_spinner(UserTestFixture *fixture, Thread **thread_out, volatile u64 **counter_out) {
    if (thread_out) *thread_out = 0;
    if (counter_out) *counter_out = 0;
    if (!fixture || !thread_out || !counter_out) return false;

    VfsNode *file = vfs_resolve(vfs_root(), RUNTIME_PREEMPT_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) return false;
    if (!user_fixture_process_create(fixture)) return false;
    if (!user_fixture_load(fixture, file)) return false;

    AddressSpace *space = process_address_space(&fixture->process);
    if (!space || space->kernel || !address_space_cr3(space)) return false;

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    vm_flags_t data_flags = 0;
    bool text_ok = address_space_query_page(space, RUNTIME_PREEMPT_TEXT, &text_frame, &text_flags) &&
        (text_flags & VM_USER) && (text_flags & VM_EXEC) && !(text_flags & VM_WRITE);
    bool data_ok = address_space_query_page(space, RUNTIME_PREEMPT_DATA, &data_frame, &data_flags) &&
        (data_flags & VM_USER) && (data_flags & VM_WRITE) && !(data_flags & VM_EXEC);
    if (!text_ok || !data_ok || fixture->image.entry != RUNTIME_PREEMPT_TEXT) return false;

    if (!user_fixture_stack_create(fixture, 0U, USER_STACK_INITIAL_BASE, 0ULL, 0ULL)) return false;

    u8 *text = (u8 *)phys_to_virt(frame_to_phys(text_frame));
    volatile u64 *counter = (volatile u64 *)phys_to_virt(frame_to_phys(data_frame));
    if (!text || !counter) return false;

    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);
    *counter = 0;

    /* Ring3 spinner:
     *   mov rax, RUNTIME_PREEMPT_DATA
     * loop:
     *   inc qword [rax]
     *   jmp loop
     *
     * No syscall, fault, yield, or blocking instruction exists in the loop. */
    text[0] = 0x48U;
    text[1] = 0xB8U;
    for (u32 i = 0; i < 8U; ++i) text[2U + i] = (u8)(RUNTIME_PREEMPT_DATA >> (i * 8U));
    text[10] = 0x48U;
    text[11] = 0xFFU;
    text[12] = 0x00U;
    text[13] = 0xEBU;
    text[14] = 0xFBU;

    if (!user_fixture_thread_create(fixture, 0U)) return false;
    Thread *thread = &fixture->threads[0];
    u64 user_rsp = user_fixture_stack_rsp(fixture, 0U);
    if (!user_rsp || !thread_prepare_user(thread, RUNTIME_PREEMPT_TEXT, user_rsp)) return false;

    *thread_out = thread;
    *counter_out = counter;
    return true;
}

void runtime_preemption_test_run(void) {
    terminal_writeln("RUNTIME PREEMPTION / SERVICE PROGRESS TEST:");

    Thread *main = thread_current();
    Process *kernel = process_kernel();
    bool preflight = main && kernel && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && scheduler_preemption_enabled() &&
        timer_initialized() && supervisor_running() && supervisor_stack_guarded();
    report("NORMAL PREEMPTION POLICY ACTIVE", preflight);
    if (!preflight || !user_fixture_available()) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("RUNTIME PREEMPTION / SERVICE PROGRESS TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    UserTestFixture *fixture = user_fixture_begin("RUNTIME PREEMPTION / SERVICE PROGRESS TEST");
    if (!fixture) return;
    user_fixture_set_quiet(fixture, true);

    bool behavior = false;
    Thread *spinner = 0;
    volatile u64 *counter = 0;
    bool setup = setup_spinner(fixture, &spinner, &counter);
    user_fixture_check("SPINNER IMAGE / STACK / THREAD", setup);
    if (!setup) goto finish;

    u64 supervisor_pid = supervisor_process_id();
    u64 supervisor_tid = supervisor_thread_id();
    u64 preempt_before = scheduler_preemption_count();
    u64 ticks_before = timer_ticks();

    interrupts_disable();
    bool queued = scheduler_add(spinner);
    u64 reply = 0;
    bool ping = queued && supervisor_ping(RUNTIME_PREEMPT_COOKIE, &reply);
    u64 preempt_delta = scheduler_preemption_count() - preempt_before;
    u64 tick_delta = timer_ticks() - ticks_before;
    u64 spin_count = counter ? *counter : 0;

    bool main_restored = thread_current() == main && main->state == THREAD_STATE_RUNNING && main->on_run_queue;
    bool spinner_suspended = queued && spinner->state == THREAD_STATE_READY && spinner->on_run_queue &&
        spinner->interrupt_context_ready && spinner->interrupt_rsp;
    bool supervisor_progress = ping && reply == RUNTIME_PREEMPT_COOKIE &&
        supervisor_process_id() == supervisor_pid && supervisor_thread_id() == supervisor_tid &&
        supervisor_stack_guarded();
    bool timer_preempted = preempt_delta > 0ULL && tick_delta > 0ULL;

    bool spinner_terminated = false;
    if (spinner->state == THREAD_STATE_DEAD) spinner_terminated = true;
    else spinner_terminated = task_terminate_thread(spinner);
    bool queue_restored = spinner_terminated && scheduler_thread_count() == 1ULL &&
        thread_current() == main && main->state == THREAD_STATE_RUNNING && main->on_run_queue;
    interrupts_enable();

    report("SPINNER QUEUED AHEAD OF SUPERVISOR", queued);
    report("SPINNER RAN WITHOUT YIELD", spin_count > 0ULL);
    report("PIT PREEMPTED SPINNER", timer_preempted);
    report("SUPERVISOR REPLIED THROUGH CONTENTION", supervisor_progress);
    report("MAIN RESTORED / SPINNER SUSPENDED", main_restored && spinner_suspended);
    report("SPINNER TERMINATED / RUN QUEUE RESTORED", queue_restored);

    behavior = queued && spin_count > 0ULL && timer_preempted && supervisor_progress &&
        main_restored && spinner_suspended && queue_restored && scheduler_preemption_enabled();

finish: {
        bool finished = user_fixture_finish(fixture, behavior);
        report("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        bool pass = behavior && finished && scheduler_preemption_enabled();
        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("RUNTIME PREEMPTION / SERVICE PROGRESS TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}
