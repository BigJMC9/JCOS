#include "user_stack_guard_test.h"
#include "user_test_fixture.h"

#include "gdt.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "user_stack.h"
#include "vfs.h"
#include "vmm.h"

#define USER_STACK_GUARD_PATH "/bin/runtimetest.elf"
#define USER_STACK_GUARD_TEXT ADDRESS_SPACE_USER_BASE
#define USER_STACK_GUARD_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define USER_STACK_GUARD_INITIAL_RSP (USER_STACK_GUARD_STACK + sizeof(u64))
#define USER_STACK_GUARD_FAULT_ADDRESS (USER_STACK_GUARD_STACK - sizeof(u64))

#define PF_ERROR_WRITE (1ULL << 1)
#define PF_ERROR_USER  (1ULL << 2)
#define PF_ERROR_LOW_MASK 0x1FULL

static void print_check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

void user_stack_guard_test_run(void) {
    terminal_writeln("USER STACK GUARD TEST:");

    bool supervisor_before = supervisor_stack_guarded();
    print_check("SUPERVISOR STACK GUARDED", supervisor_before);
    if (!supervisor_before || !user_fixture_available()) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER STACK GUARD TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    UserTestFixture *fixture = user_fixture_begin("USER STACK GUARD TEST");
    if (!fixture) return;
    user_fixture_set_quiet(fixture, true);
    bool behavior_completed = false;

    VfsNode *file = vfs_resolve(vfs_root(), USER_STACK_GUARD_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    user_fixture_check("ELF FILE", file_ok);
    if (!file_ok) goto finish;

    bool process_created = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", process_created);
    if (!process_created) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    user_fixture_check("ADDRESS SPACE", space_ok);
    if (!space_ok) goto finish;

    bool image_loaded = user_fixture_load(fixture, file);
    user_fixture_check("ELF LOAD", image_loaded);
    if (!image_loaded) goto finish;

    frame_t text_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    bool text_ok = address_space_query_page(space, USER_STACK_GUARD_TEXT, &text_frame, &text_flags) &&
        (text_flags & VM_USER) && (text_flags & VM_EXEC) && !(text_flags & VM_WRITE);
    user_fixture_check("RX ENTRY PAGE", text_ok);
    if (!text_ok) goto finish;

    bool stack_ready = user_fixture_stack_create(fixture, 0U, USER_STACK_GUARD_STACK, 0ULL, 0ULL);
    user_fixture_check("GUARDED RW-NX STACK", stack_ready);
    if (!stack_ready) goto finish;

    bool guards_ok = user_stack_mapping_valid(space, USER_STACK_GUARD_STACK, fixture->stacks[0].frame);
    user_fixture_check("LOWER / UPPER GUARDS ABSENT", guards_ok);
    if (!guards_ok) goto finish;

    u8 *text = (u8 *)phys_to_virt(frame_to_phys(text_frame));
    bool direct_ok = text != 0;
    user_fixture_check("PRIVATE TEXT FRAME ACCESS", direct_ok);
    if (!direct_ok) goto finish;

    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);
    text[0] = 0x50U; /* push rax: RSP -> stack base, mapped */
    text[1] = 0x50U; /* push rax: RSP -> lower guard, must #PF */
    text[2] = 0x0FU;
    text[3] = 0x0BU; /* UD2 tripwire if the guard is accidentally mapped. */

    bool thread_created = user_fixture_thread_create(fixture, 0U);
    user_fixture_check("THREAD CREATE", thread_created);
    if (!thread_created) goto finish;

    Thread *thread = &fixture->threads[0];
    u64 thread_id = thread->id;
    bool prepared = thread_prepare_user(thread, USER_STACK_GUARD_TEXT, USER_STACK_GUARD_INITIAL_RSP);
    user_fixture_check("PREPARE NEAR LOWER GUARD", prepared);
    if (!prepared) goto finish;

    bool queued = scheduler_add(thread);
    user_fixture_check("QUEUE USER", queued);
    if (!queued) goto finish;

    interrupt_clear_user_fault();
    bool yielded = user_fixture_schedule_once();

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    bool fault_captured = interrupt_last_user_fault(&fault);
    bool fault_ok = fault_captured && fault.thread_id == thread_id && fault.vector == 14ULL &&
        fault.rip == USER_STACK_GUARD_TEXT + 1ULL && fault.cr2 == USER_STACK_GUARD_FAULT_ADDRESS &&
        fault.user_ss == GDT_USER_DATA_SELECTOR;
    bool error_ok = fault_captured && (fault.error_code & PF_ERROR_LOW_MASK) == (PF_ERROR_WRITE | PF_ERROR_USER);
    bool contained = yielded && thread_current() == fixture->main_thread &&
        fixture->main_thread->state == THREAD_STATE_RUNNING && thread->state == THREAD_STATE_DEAD &&
        !thread->on_run_queue && !thread->interrupt_context_ready && !thread->interrupt_rsp &&
        scheduler_thread_count() == 1ULL;
    bool guards_after = user_stack_mapping_valid(space, USER_STACK_GUARD_STACK, fixture->stacks[0].frame);

    user_fixture_check("LOWER GUARD PAGE FAULT", fault_ok);
    user_fixture_check("NOT-PRESENT USER WRITE PF BITS", error_ok);
    user_fixture_check("FAULT CONTAINED", contained);
    user_fixture_check("GUARDS PRESERVED AFTER FAULT", guards_after);

    interrupt_clear_user_fault();
    behavior_completed = guards_ok && prepared && queued && yielded && fault_ok && error_ok && contained && guards_after;

finish: {
        bool finished = user_fixture_finish(fixture, behavior_completed);
        print_check("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        bool supervisor_after = finished && supervisor_stack_guarded();
        print_check("SUPERVISOR STACK STILL GUARDED", supervisor_after);
        bool pass = supervisor_before && finished && supervisor_after;
        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("USER STACK GUARD TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}
