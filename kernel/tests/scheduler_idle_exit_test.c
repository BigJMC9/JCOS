#include "scheduler_idle_exit_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "capability.h"
#include "gdt.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "terminal.h"
#include "thread.h"
#include "user_abi.h"
#include "vfs.h"
#include "vmm.h"

#define IDLE_EXIT_PATH "/bin/runtimetest.elf"
#define IDLE_EXIT_TEXT ADDRESS_SPACE_USER_BASE
#define IDLE_EXIT_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define IDLE_EXIT_TIMEOUT_TICKS 3ULL

typedef enum {
    IDLE_EXIT_SYSCALL = 0,
    IDLE_EXIT_FAULT
} IdleExitCase;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool message_zero(const IpcMessage *message) {
    if (!message || message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message->words[i]) return false;
    return true;
}

static bool run_case(IdleExitCase test_case, const char *label) {
    UserTestFixture *fixture = user_fixture_begin(label);
    if (!fixture) return false;
    user_fixture_set_quiet(fixture, true);
    bool behavior = false;

    VfsNode *file = vfs_resolve(vfs_root(), IDLE_EXIT_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    user_fixture_check("ELF FILE", file_ok);
    if (!file_ok) goto finish;

    bool process_ok = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", process_ok);
    if (!process_ok) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    user_fixture_check("ADDRESS SPACE", space_ok);
    if (!space_ok) goto finish;

    bool image_ok = user_fixture_load(fixture, file);
    user_fixture_check("ELF LOAD", image_ok);
    if (!image_ok) goto finish;

    frame_t text_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    bool text_ok = address_space_query_page(space, IDLE_EXIT_TEXT, &text_frame, &text_flags) &&
        (text_flags & VM_USER) && (text_flags & VM_EXEC) && !(text_flags & VM_WRITE);
    user_fixture_check("RX ENTRY PAGE", text_ok);
    if (!text_ok) goto finish;

    bool stack_ok = user_fixture_stack_create(fixture, 0U, IDLE_EXIT_STACK, 0ULL, 0ULL);
    user_fixture_check("GUARDED USER STACK", stack_ok);
    if (!stack_ok) goto finish;

    u8 *text = (u8 *)phys_to_virt(frame_to_phys(text_frame));
    user_fixture_check("PRIVATE TEXT FRAME ACCESS", text != 0);
    if (!text) goto finish;
    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);

    if (test_case == IDLE_EXIT_SYSCALL) {
        text[0] = 0xB8U; /* mov eax, JCOS_SYSCALL_THREAD_EXIT */
        u32 number = JCOS_SYSCALL_THREAD_EXIT;
        k_memcpy(text + 1U, &number, sizeof(number));
        text[5] = 0xCDU;
        text[6] = 0x80U;
        text[7] = 0x0FU;
        text[8] = 0x0BU; /* tripwire: successful exit never returns */
    } else {
        text[0] = 0x0FU;
        text[1] = 0x0BU; /* #UD */
    }

    bool endpoint_ok = user_fixture_endpoint_create(fixture, 0U);
    user_fixture_check("WAKE ENDPOINT", endpoint_ok);
    if (!endpoint_ok) goto finish;
    CapabilityHandle receive = CAPABILITY_INVALID_HANDLE;
    bool cap_ok = user_fixture_grant(fixture, true, 0U, CAPABILITY_RIGHT_RECEIVE, &receive);
    user_fixture_check("KERNEL RECEIVE CAP", cap_ok);
    if (!cap_ok) goto finish;

    bool thread_ok = user_fixture_thread_create(fixture, 0U);
    user_fixture_check("THREAD CREATE", thread_ok);
    if (!thread_ok) goto finish;
    Thread *thread = &fixture->threads[0];
    u64 thread_id = thread->id;
    u64 user_rsp = user_fixture_stack_rsp(fixture, 0U);
    bool prepared = user_rsp && thread_prepare_user(thread, IDLE_EXIT_TEXT, user_rsp);
    user_fixture_check("PREPARE USER", prepared);
    if (!prepared) goto finish;

    bool queued = scheduler_add(thread);
    user_fixture_check("QUEUE USER", queued && scheduler_thread_count() == 2ULL);
    if (!queued) goto finish;

    interrupt_clear_user_fault();
    u64 entries_before = scheduler_idle_entry_count();
    u64 resumes_before = scheduler_idle_resume_count();
    IpcMessage output;
    k_memset(&output, 0xA5, sizeof(output));

    /* Main blocks first. The test user Thread becomes the sole normal runnable
     * context, exits/faults into scheduler idle, then PIT expires this wait and
     * the scheduler resumes main from the private idle stack. */
    bool received = ipc_receive_blocking_for(fixture->kernel_process, receive, &output,
        IDLE_EXIT_TIMEOUT_TICKS);

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    bool faulted = interrupt_last_user_fault(&fault);
    bool terminal_ok = thread->state == THREAD_STATE_DEAD && !thread->on_run_queue &&
        !thread->interrupt_context_ready && !thread->interrupt_rsp;
    bool idle_transition = scheduler_idle_entry_count() > entries_before &&
        scheduler_idle_resume_count() > resumes_before && !scheduler_idle_active() &&
        scheduler_idle_stack_guarded();
    bool main_resumed = !received && message_zero(&output) && thread_current() == fixture->main_thread &&
        fixture->main_thread->state == THREAD_STATE_RUNNING && fixture->main_thread->on_run_queue &&
        !thread_wait_active(fixture->main_thread) && scheduler_thread_count() == 1ULL;
    bool cause_ok = test_case == IDLE_EXIT_FAULT
        ? (faulted && fault.thread_id == thread_id && fault.vector == 6ULL && fault.rip == IDLE_EXIT_TEXT &&
            fault.user_rsp == user_rsp && fault.user_ss == GDT_USER_DATA_SELECTOR)
        : !faulted;

    user_fixture_check(test_case == IDLE_EXIT_FAULT ? "SOLE RUNNABLE USER FAULT" : "SOLE RUNNABLE USER EXIT",
        terminal_ok && cause_ok);
    user_fixture_check("PRIVATE IDLE ENTER / PIT RESUME", idle_transition);
    user_fixture_check("TIMED MAIN WAKE / RESTORE", main_resumed);
    behavior = terminal_ok && cause_ok && idle_transition && main_resumed;

finish: {
        interrupt_clear_user_fault();
        bool finished = user_fixture_finish(fixture, behavior);
        report("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        return finished;
    }
}

void scheduler_idle_exit_test_run(void) {
    terminal_writeln("SCHEDULER PRIVATE IDLE EXIT TEST:");
    bool ready = scheduler_idle_context_ready() && scheduler_idle_stack_guarded() &&
        !scheduler_idle_active() && scheduler_thread_count() == 1ULL && user_fixture_available();
    report("GUARDED PRIVATE IDLE CONTEXT", ready);
    if (!ready) return;

    bool exit_ok = run_case(IDLE_EXIT_SYSCALL, "LAST RUNNABLE USER EXIT");
    bool fault_ok = exit_ok && !user_fixture_busy() && run_case(IDLE_EXIT_FAULT, "LAST RUNNABLE USER FAULT");
    bool pass = exit_ok && fault_ok && !user_fixture_busy() && !scheduler_idle_active() &&
        scheduler_idle_stack_guarded() && scheduler_thread_count() == 1ULL;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("SCHEDULER PRIVATE IDLE EXIT TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
