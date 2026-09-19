#include "user_protection_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "gdt.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "scheduler.h"
#include "terminal.h"
#include "thread.h"
#include "vfs.h"
#include "vmm.h"

#define USER_PROTECTION_PATH "/bin/runtimetest.elf"
#define USER_PROTECTION_TEXT ADDRESS_SPACE_USER_BASE
#define USER_PROTECTION_DATA (ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE)
#define USER_PROTECTION_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define USER_PROTECTION_WRITE_TARGET (USER_PROTECTION_TEXT + 0x100ULL)

#define PF_ERROR_PRESENT (1ULL << 0)
#define PF_ERROR_WRITE   (1ULL << 1)
#define PF_ERROR_USER    (1ULL << 2)
#define PF_ERROR_FETCH   (1ULL << 4)
#define PF_ERROR_LOW_MASK 0x1FULL

typedef enum {
    USER_PROTECTION_EXEC_STACK = 0,
    USER_PROTECTION_EXEC_DATA,
    USER_PROTECTION_WRITE_TEXT
} UserProtectionCase;

static void print_check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void emit_mov_rax_imm64(u8 **cursor, u64 value) {
    u8 *p = *cursor;
    *p++ = 0x48U;
    *p++ = 0xB8U;
    for (u32 i = 0; i < 8U; ++i) *p++ = (u8)(value >> (i * 8U));
    *cursor = p;
}

static bool run_case(UserProtectionCase test_case, const char *label) {
    terminal_write("\n  ");
    terminal_writeln(label);

    UserTestFixture *fixture = user_fixture_begin(label);
    if (!fixture) return false;
    user_fixture_set_quiet(fixture, true);

    bool behavior_completed = false;
    bool process_created = false;
    bool image_loaded = false;
    bool permissions_ok = false;
    bool stack_ready = false;
    bool thread_created = false;
    bool prepared = false;
    bool queued = false;
    bool yielded = false;
    bool fault_captured = false;
    bool fault_identity_ok = false;
    bool error_bits_ok = false;
    bool contained = false;

    VfsNode *file = vfs_resolve(vfs_root(), USER_PROTECTION_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    user_fixture_check("ELF FILE", file_ok);
    if (!file_ok) goto finish;

    process_created = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", process_created);
    if (!process_created) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    user_fixture_check("ADDRESS SPACE", space_ok);
    if (!space_ok) goto finish;

    image_loaded = user_fixture_load(fixture, file);
    user_fixture_check("ELF LOAD", image_loaded);
    if (!image_loaded) goto finish;

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    vm_flags_t data_flags = 0;
    bool text_mapping = address_space_query_page(space, USER_PROTECTION_TEXT, &text_frame, &text_flags);
    bool data_mapping = address_space_query_page(space, USER_PROTECTION_DATA, &data_frame, &data_flags);
    bool image_layout = fixture->image.entry == USER_PROTECTION_TEXT && fixture->image.page_count == 2U;
    bool text_rx = text_mapping && (text_flags & VM_USER) && (text_flags & VM_EXEC) && !(text_flags & VM_WRITE);
    bool data_rw_nx = data_mapping && (data_flags & VM_USER) && (data_flags & VM_WRITE) && !(data_flags & VM_EXEC);
    permissions_ok = image_layout && text_rx && data_rw_nx;
    user_fixture_check("RX TEXT / RW-NX DATA", permissions_ok);
    if (!permissions_ok) goto finish;

    stack_ready = user_fixture_stack_create(fixture, 0U, USER_PROTECTION_STACK, 0ULL, 0ULL);
    user_fixture_check("RW-NX STACK", stack_ready);
    if (!stack_ready) goto finish;

    frame_t stack_frame = FRAME_INVALID;
    vm_flags_t stack_flags = 0;
    bool stack_mapping = address_space_query_page(space, USER_PROTECTION_STACK, &stack_frame, &stack_flags);
    bool stack_permissions = stack_mapping && stack_frame == fixture->stacks[0].frame &&
        (stack_flags & VM_USER) && (stack_flags & VM_WRITE) && !(stack_flags & VM_EXEC);
    user_fixture_check("STACK PERMISSIONS", stack_permissions);
    if (!stack_permissions) goto finish;

    u8 *text = (u8 *)phys_to_virt(frame_to_phys(text_frame));
    u8 *data = (u8 *)phys_to_virt(frame_to_phys(data_frame));
    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(stack_frame));
    bool direct_ok = text && data && stack;
    user_fixture_check("PRIVATE FRAME ACCESS", direct_ok);
    if (!direct_ok) goto finish;

    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);
    u8 *cursor = text;
    u64 expected_cr2 = 0;
    u64 expected_rip = 0;
    u64 expected_error = 0;

    if (test_case == USER_PROTECTION_EXEC_STACK) {
        stack[0] = 0x0FU;
        stack[1] = 0x0BU; /* UD2 tripwire if NX is not enforced. */
        emit_mov_rax_imm64(&cursor, USER_PROTECTION_STACK);
        *cursor++ = 0xFFU;
        *cursor++ = 0xE0U; /* jmp rax */
        *cursor++ = 0x0FU;
        *cursor++ = 0x0BU;
        expected_cr2 = USER_PROTECTION_STACK;
        expected_rip = USER_PROTECTION_STACK;
        expected_error = PF_ERROR_PRESENT | PF_ERROR_USER | PF_ERROR_FETCH;
    } else if (test_case == USER_PROTECTION_EXEC_DATA) {
        data[0] = 0x0FU;
        data[1] = 0x0BU; /* UD2 tripwire if NX is not enforced. */
        emit_mov_rax_imm64(&cursor, USER_PROTECTION_DATA);
        *cursor++ = 0xFFU;
        *cursor++ = 0xE0U; /* jmp rax */
        *cursor++ = 0x0FU;
        *cursor++ = 0x0BU;
        expected_cr2 = USER_PROTECTION_DATA;
        expected_rip = USER_PROTECTION_DATA;
        expected_error = PF_ERROR_PRESENT | PF_ERROR_USER | PF_ERROR_FETCH;
    } else {
        emit_mov_rax_imm64(&cursor, USER_PROTECTION_WRITE_TARGET);
        *cursor++ = 0xC6U;
        *cursor++ = 0x00U;
        *cursor++ = 0x90U; /* mov byte [rax], 0x90 */
        *cursor++ = 0x0FU;
        *cursor++ = 0x0BU; /* UD2 tripwire if the write succeeds. */
        expected_cr2 = USER_PROTECTION_WRITE_TARGET;
        expected_rip = USER_PROTECTION_TEXT + 10ULL;
        expected_error = PF_ERROR_PRESENT | PF_ERROR_WRITE | PF_ERROR_USER;
    }

    thread_created = user_fixture_thread_create(fixture, 0U);
    user_fixture_check("THREAD CREATE", thread_created);
    if (!thread_created) goto finish;

    Thread *thread = &fixture->threads[0];
    u64 thread_id = thread->id;
    u64 user_rsp = user_fixture_stack_rsp(fixture, 0U);
    prepared = user_rsp && thread_prepare_user(thread, USER_PROTECTION_TEXT, user_rsp);
    user_fixture_check("PREPARE RX ENTRY / NX STACK", prepared);
    if (!prepared) goto finish;

    queued = scheduler_add(thread);
    user_fixture_check("QUEUE USER", queued);
    if (!queued) goto finish;

    interrupt_clear_user_fault();
    yielded = user_fixture_schedule_once();
    user_fixture_check("SHELL RESUMED", yielded && thread_current() == fixture->main_thread);

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    fault_captured = interrupt_last_user_fault(&fault);

    fault_identity_ok = fault_captured && fault.thread_id == thread_id && fault.vector == 14ULL &&
        fault.cr2 == expected_cr2 && fault.rip == expected_rip && fault.user_rsp == user_rsp &&
        fault.user_ss == GDT_USER_DATA_SELECTOR;
    error_bits_ok = fault_captured && (fault.error_code & PF_ERROR_LOW_MASK) == expected_error;
    contained = yielded && thread_current() == fixture->main_thread &&
        fixture->main_thread->state == THREAD_STATE_RUNNING && thread->state == THREAD_STATE_DEAD &&
        !thread->on_run_queue && !thread->interrupt_context_ready && !thread->interrupt_rsp &&
        scheduler_thread_count() == 1ULL;

    user_fixture_check("PAGE FAULT / CR2 / RIP", fault_identity_ok);
    user_fixture_check(test_case == USER_PROTECTION_WRITE_TEXT ? "WRITE-PROTECT PF BITS" : "NX FETCH PF BITS",
        error_bits_ok);
    user_fixture_check("FAULT CONTAINED", contained);

    behavior_completed = permissions_ok && stack_permissions && prepared && queued && yielded && fault_identity_ok &&
        error_bits_ok && contained;

finish: {
        bool finished = user_fixture_finish(fixture, behavior_completed);
        print_check("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        return finished;
    }
}

void user_protection_test_run(void) {
    terminal_writeln("USER MEMORY PROTECTION TEST:");

    bool nx_active = vmm_nx_enabled();
    print_check("NX ACTIVE", nx_active);
    if (!nx_active) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("USER MEMORY PROTECTION TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    bool stack_ok = run_case(USER_PROTECTION_EXEC_STACK, "EXECUTE STACK");
    bool data_ok = !user_fixture_busy() && run_case(USER_PROTECTION_EXEC_DATA, "EXECUTE DATA");
    bool text_ok = !user_fixture_busy() && run_case(USER_PROTECTION_WRITE_TEXT, "WRITE RX TEXT");
    bool pass = stack_ok && data_ok && text_ok && !user_fixture_busy();

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("\nUSER MEMORY PROTECTION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
