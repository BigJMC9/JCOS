#include "execution_profile_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "execution_profile.h"
#include "gdt.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "terminal.h"
#include "thread.h"
#include "vfs.h"
#include "vmm.h"

#define EXECUTION_PROFILE_PATH "/bin/runtimetest.elf"
#define EXECUTION_PROFILE_TEXT ADDRESS_SPACE_USER_BASE
#define EXECUTION_PROFILE_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)

typedef enum {
    EXECUTION_PROFILE_X87 = 0,
    EXECUTION_PROFILE_SSE2
} ExecutionProfileCase;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool run_case(ExecutionProfileCase test_case, const char *label) {
    UserTestFixture *fixture = user_fixture_begin(label);
    if (!fixture) return false;
    user_fixture_set_quiet(fixture, true);

    bool behavior_completed = false;
    VfsNode *file = vfs_resolve(vfs_root(), EXECUTION_PROFILE_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    user_fixture_check("ELF FILE", file_ok);
    if (!file_ok) goto finish;

    bool process_created = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", process_created);
    if (!process_created) goto finish;

    bool image_loaded = user_fixture_load(fixture, file);
    user_fixture_check("ELF LOAD", image_loaded);
    if (!image_loaded) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    frame_t text_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    bool text_ok = space && address_space_query_page(space, EXECUTION_PROFILE_TEXT,
        &text_frame, &text_flags) && text_frame != FRAME_INVALID &&
        (text_flags & VM_USER) && (text_flags & VM_EXEC) && !(text_flags & VM_WRITE);
    user_fixture_check("RX USER TEXT", text_ok);
    if (!text_ok) goto finish;

    bool stack_ready = user_fixture_stack_create(fixture, 0U, EXECUTION_PROFILE_STACK, 0ULL, 0ULL);
    user_fixture_check("USER STACK", stack_ready);
    if (!stack_ready) goto finish;

    u8 *text = (u8 *)phys_to_virt(frame_to_phys(text_frame));
    bool direct_ok = text != 0;
    user_fixture_check("PRIVATE FRAME ACCESS", direct_ok);
    if (!direct_ok) goto finish;

    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);
    u32 code_size = 0;
    if (test_case == EXECUTION_PROFILE_X87) {
        text[0] = 0xD9U;
        text[1] = 0xEEU; /* FLDZ: must #NM while CR0.TS remains set. */
        code_size = 2U;
    } else {
        text[0] = 0x66U;
        text[1] = 0x0FU;
        text[2] = 0xEFU;
        text[3] = 0xC0U; /* PXOR xmm0,xmm0: must #NM in the restricted profile. */
        code_size = 4U;
    }
    text[code_size] = 0x0FU;
    text[code_size + 1U] = 0x0BU; /* UD2 tripwire if the restricted instruction executes. */

    bool thread_created = user_fixture_thread_create(fixture, 0U);
    user_fixture_check("THREAD CREATE", thread_created);
    if (!thread_created) goto finish;

    Thread *thread = &fixture->threads[0];
    u64 thread_id = thread->id;
    u64 user_rsp = user_fixture_stack_rsp(fixture, 0U);
    bool prepared = user_rsp && thread_prepare_user(thread, EXECUTION_PROFILE_TEXT, user_rsp);
    user_fixture_check("PREPARE USER", prepared);
    if (!prepared) goto finish;

    bool queued = scheduler_add(thread);
    user_fixture_check("QUEUE USER", queued);
    if (!queued) goto finish;

    interrupt_clear_user_fault();
    bool yielded = user_fixture_schedule_once();
    user_fixture_check("SHELL RESUMED", yielded && thread_current() == fixture->main_thread);

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    bool captured = interrupt_last_user_fault(&fault);
    bool nm_ok = captured && fault.thread_id == thread_id && fault.vector == 7ULL &&
        fault.error_code == 0ULL && fault.rip == EXECUTION_PROFILE_TEXT &&
        fault.user_rsp == user_rsp && fault.user_ss == GDT_USER_DATA_SELECTOR;
    bool contained = yielded && thread_current() == fixture->main_thread &&
        thread->state == THREAD_STATE_DEAD && !thread->on_run_queue &&
        !thread->interrupt_context_ready && !thread->interrupt_rsp &&
        scheduler_thread_count() == 1ULL;
    bool profile_still_active = execution_profile_fp_simd_restricted();

    user_fixture_check("#NM IDENTITY", nm_ok);
    user_fixture_check("FAULT CONTAINED", contained);
    user_fixture_check("RESTRICTED PROFILE STILL ACTIVE", profile_still_active);
    behavior_completed = nm_ok && contained && profile_still_active;

finish: {
        bool finished = user_fixture_finish(fixture, behavior_completed);
        report("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        return finished;
    }
}

void execution_profile_test_run(void) {
    terminal_writeln("EXECUTION PROFILE / FP-SIMD RESTRICTION TEST:");

    bool profile = execution_profile_fp_simd_restricted();
    report("UP RESTRICTED PROFILE ACTIVE", profile);
    if (!profile) goto done;

    bool x87_ok = run_case(EXECUTION_PROFILE_X87, "X87 RESTRICTED PROFILE");
    bool sse2_ok = !user_fixture_busy() && run_case(EXECUTION_PROFILE_SSE2, "SSE2 RESTRICTED PROFILE");
    profile = profile && x87_ok && sse2_ok && !user_fixture_busy() && execution_profile_fp_simd_restricted();

done:
    terminal_set_color(profile ? terminal_accent_color() : terminal_error_color());
    terminal_write("EXECUTION PROFILE / FP-SIMD RESTRICTION TEST: ");
    terminal_writeln(profile ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
