#include "process_exit_test.h"
#include "user_test_fixture.h"

#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "physmap.h"
#include "pmm_test.h"
#include "scheduler.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "user_stack.h"
#include "vfs.h"
#include "vmm.h"
#include "../../include/user_abi.h"

#define EXIT_TEST_PATH "/bin/runtimetest.elf"
#define EXIT_TEST_TEXT ADDRESS_SPACE_USER_BASE
#define EXIT_TEST_DATA (ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE)
#define EXIT_TEST_SIBLING (EXIT_TEST_TEXT + 0x100ULL)
#define EXIT_TEST_UNMAPPED (ADDRESS_SPACE_USER_BASE + 0x400000ULL)

typedef enum {
    EXIT_SIBLING_READY = 0,
    EXIT_SIBLING_BLOCKED,
    EXIT_SIBLING_COMPLETED
} ExitSiblingState;

typedef struct {
    volatile u64 *counter;
    CapabilityHandle sibling_receive;
    CapabilityHandle kernel_send;
    CapabilityHandle kernel_receive;
    u64 expected_rip;
    u64 expected_cr2;
} ExitCase;

static void mov64(u8 **p, u8 opcode, u64 value) {
    *(*p)++ = 0x48U;
    *(*p)++ = opcode;
    for (u32 i = 0; i < 8U; ++i) *(*p)++ = (u8)(value >> (8U * i));
}

static void exit_code(u8 **p) {
    mov64(p, 0xB8U, JCOS_SYSCALL_THREAD_EXIT);
    *(*p)++ = 0xCDU; *(*p)++ = 0x80U;
    *(*p)++ = 0x0FU; *(*p)++ = 0x0BU;
}

static bool endpoint_owned(UserTestFixture *f, u32 i) {
    if (!endpoint_create_owned(&f->endpoints[i], &f->process)) return false;
    f->endpoint_created[i] = true;
    return true;
}

static bool setup(UserTestFixture *f, ExitCase *c, u64 vector, bool normal,
    ExitSiblingState state) {
    k_memset(c, 0, sizeof(*c));
    VfsNode *file = vfs_resolve(vfs_root(), EXIT_TEST_PATH);
    if (!file || !user_fixture_process_create(f) || !user_fixture_load(f, file)) return false;
    ProcessExitInfo empty;
    if (process_exit_info(&f->process, &empty)) return false;
    if (!endpoint_owned(f, 0U) || !endpoint_owned(f, 1U) ||
        !user_fixture_grant(f, false, 0U, CAPABILITY_RIGHT_RECEIVE, &c->sibling_receive) ||
        !user_fixture_grant(f, true, 0U, CAPABILITY_RIGHT_SEND, &c->kernel_send) ||
        !user_fixture_grant(f, true, 1U, CAPABILITY_RIGHT_RECEIVE, &c->kernel_receive)) return false;

    AddressSpace *space = process_address_space(&f->process);
    frame_t text_frame = FRAME_INVALID, data_frame = FRAME_INVALID;
    if (!space || !address_space_query_page(space, EXIT_TEST_TEXT, &text_frame, 0) ||
        !address_space_query_page(space, EXIT_TEST_DATA, &data_frame, 0)) return false;
    u8 *text = phys_to_virt(frame_to_phys(text_frame));
    c->counter = phys_to_virt(frame_to_phys(data_frame));
    if (!text || !c->counter) return false;
    k_memset(text, 0x90, (usize)VM_PAGE_SIZE);
    *c->counter = 0;
    u8 *p = text;
    c->expected_rip = EXIT_TEST_TEXT;
    if (normal) exit_code(&p);
    else if (vector == 0ULL) {
        *p++ = 0x31U; *p++ = 0xC0U; /* xor eax,eax */
        *p++ = 0x31U; *p++ = 0xD2U; /* xor edx,edx */
        *p++ = 0x48U; *p++ = 0xF7U; *p++ = 0xF0U; /* div rax (zero) */
        c->expected_rip += 4ULL;
    } else if (vector == 13ULL) {
        *p++ = 0xFAU; /* CLI at CPL3 must fault. */
    } else if (vector == 7ULL) {
        *p++ = 0xDBU; *p++ = 0xE3U; /* FNINIT under CR0.TS=1. */
    } else if (vector == 14ULL) {
        if (address_space_query_page(space, EXIT_TEST_UNMAPPED, 0, 0)) return false;
        mov64(&p, 0xB8U, EXIT_TEST_UNMAPPED);
        *p++ = 0x48U; *p++ = 0x8BU; *p++ = 0x00U; /* mov rax,[rax] */
        c->expected_rip += 10ULL;
        c->expected_cr2 = EXIT_TEST_UNMAPPED;
    } else {
        *p++ = 0x0FU; *p++ = 0x0BU; /* UD2 */
    }
    /* No accidental fallthrough if the expected fault is missing. */
    exit_code(&p);

    p = text + 0x100U;
    if (state != EXIT_SIBLING_READY || normal) {
        mov64(&p, 0xBBU, c->sibling_receive);
        mov64(&p, 0xB8U, JCOS_SYSCALL_IPC_RECEIVE_BLOCKING);
        *p++ = 0xCDU; *p++ = 0x80U;
    }
    mov64(&p, 0xB8U, EXIT_TEST_DATA);
    *p++ = 0x48U; *p++ = 0xFFU; *p++ = 0x00U; /* inc qword [rax] */
    exit_code(&p);

    for (u32 i = 0; i < 2U; ++i) {
        u64 stack = USER_STACK_INITIAL_BASE + (u64)i * 2ULL * VM_PAGE_SIZE;
        if (!user_fixture_stack_create(f, i, stack, 0, 0) || !user_fixture_thread_create(f, i) ||
            !thread_prepare_user(&f->threads[i], i ? EXIT_TEST_SIBLING : EXIT_TEST_TEXT,
                user_fixture_stack_rsp(f, i))) return false;
    }
    return true;
}

static bool dead(const Thread *t) {
    return t->state == THREAD_STATE_DEAD && !t->on_run_queue && !t->run_next &&
        !t->interrupt_context_ready && !t->interrupt_rsp && !thread_wait_active(t);
}

static bool send_sibling(UserTestFixture *f, const ExitCase *c) {
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = 1U; message.words[0] = 1ULL;
    return ipc_try_send(f->kernel_process, c->kernel_send, &message);
}

static bool peer_returns_closed(UserTestFixture *f, const ExitCase *c) {
    IpcMessage message;
    k_memset(&message, 0xA5, sizeof(message));
    u64 timeout = (u64)timer_frequency() * 2ULL;
    u64 started = timer_ticks();
    bool received = ipc_receive_blocking_for(f->kernel_process, c->kernel_receive, &message, timeout);
    bool zero = !message.word_count;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message.words[i]) zero = false;
    /* Public IPC is still boolean: do not advertise a new detailed-result ABI. */
    return !received && zero && endpoint_closed(&f->endpoints[1]) &&
        (u64)(timer_ticks() - started) < timeout && thread_current() == f->main_thread;
}

static bool same_record(const ProcessExitInfo *a, const ProcessExitInfo *b) {
    return a->reason == b->reason && a->process_id == b->process_id && a->thread_id == b->thread_id &&
        a->vector == b->vector && a->error_code == b->error_code && a->rip == b->rip && a->cr2 == b->cr2;
}

static bool finish_case(UserTestFixture *f, bool behavior) {
    bool finished = user_fixture_finish(f, behavior);
    /* Cleanup result and original behavior are deliberately not conflated. */
    bool released = !user_fixture_busy();
    user_fixture_check("FIXTURE RELEASED", released);
    ProcessExitInfo info;
    bool forgotten = released && !process_exit_info(&f->process, &info) && info.reason == PROCESS_EXIT_NONE;
    user_fixture_check("DESTROYED PROCESS HAS NO EXIT RECORD", forgotten);
    return finished && forgotten;
}

static bool fault_case(u64 vector, ExitSiblingState state, bool retry) {
    terminal_write("  FAULT VECTOR "); terminal_write_u64(vector);
    terminal_write(" / SIBLING "); terminal_write_u64((u64)state); terminal_putchar('\n');
    UserTestFixture *f = user_fixture_begin("PROCESS FAULT / TERMINAL RECORD");
    if (!f) return false;
    user_fixture_set_quiet(f, true);
    ExitCase c;
    bool pass = false;
    if (!setup(f, &c, vector, false, state)) goto done;

    if (state != EXIT_SIBLING_READY) {
        if (!scheduler_add(&f->threads[1]) || !user_fixture_schedule_once() ||
            f->threads[1].state != THREAD_STATE_BLOCKED || !thread_wait_active(&f->threads[1])) goto done;
    }
    /* The faulting thread must run before a READY or just-completed sibling. */
    if (!scheduler_add(&f->threads[0])) goto done;
    if (state == EXIT_SIBLING_READY && !scheduler_add(&f->threads[1])) goto done;
    if (state == EXIT_SIBLING_COMPLETED && (!send_sibling(f, &c) ||
            f->threads[1].state != THREAD_STATE_READY || !thread_wait_active(&f->threads[1]))) goto done;

    u64 pid = f->process.id, tid = f->threads[0].id;
    u64 frames = pmm_stats().free_pages;
    bool peer = peer_returns_closed(f, &c);
    bool contained = peer && dead(&f->threads[0]) && dead(&f->threads[1]) && *c.counter == 0 &&
        scheduler_thread_count() == 1ULL && endpoint_closed(&f->endpoints[0]) &&
        !endpoint_receiver_waiting(&f->endpoints[0]) && !endpoint_sender_waiting(&f->endpoints[0]);
    user_fixture_check("DOMAIN STOP / SIBLING NEVER CONTINUED / PEER RETURN", contained);
    bool retained = pmm_stats().free_pages == frames && process_thread_count(&f->process) == 2ULL &&
        f->image.loaded && f->stacks[0].mapped && f->stacks[1].mapped;
    user_fixture_check("NO STACK / IMAGE / PROCESS REAP ON FAULT", retained);
    ProcessExitInfo info, copy;
    bool record = process_exit_info(&f->process, &info) && info.reason == PROCESS_EXIT_FAULT &&
        info.process_id == pid && info.thread_id == tid && info.vector == vector &&
        info.rip == c.expected_rip && info.cr2 == c.expected_cr2 &&
        (vector == 14ULL ? info.error_code == 4ULL : info.error_code == 0ULL);
    interrupt_clear_user_fault();
    record = record && process_exit_info(&f->process, &copy) && same_record(&info, &copy) &&
        !process_exit_publish(&f->process, &copy);
    user_fixture_check("FIRST TERMINAL RECORD / INDEPENDENT OF GLOBAL DIAGNOSTIC", record);
    if (!contained || !retained || !record) goto done;

    if (retry) {
        frame_t stack = phys_to_frame(f->threads[0].kernel_stack_physical);
        bool armed = pmm_test_fail_free_range_once(stack, THREAD_KERNEL_STACK_PAGES);
        bool failed = armed && !task_quiesce_process(&f->process) && !pmm_test_free_failure_armed();
        bool stable = failed && process_exit_info(&f->process, &copy) && same_record(&info, &copy);
        bool retried = stable && task_quiesce_process(&f->process) &&
            process_exit_info(&f->process, &copy) && same_record(&info, &copy);
        user_fixture_check("REAP FAILURE / RETRY PRESERVES ORIGINAL FAULT", retried);
        if (!retried) goto done;
    }
    pass = true;
done:
    return finish_case(f, pass);
}

static bool normal_case(void) {
    terminal_writeln("  CLEAN THREAD EXIT / LAST-THREAD EXIT:");
    UserTestFixture *f = user_fixture_begin("NORMAL PROCESS TERMINAL RECORD");
    if (!f) return false;
    user_fixture_set_quiet(f, true);
    ExitCase c;
    bool pass = false;
    if (!setup(f, &c, 0, true, EXIT_SIBLING_BLOCKED) ||
        !scheduler_add(&f->threads[0]) || !scheduler_add(&f->threads[1]) ||
        !user_fixture_schedule_once()) goto done;
    ProcessExitInfo info;
    bool sibling_live = dead(&f->threads[0]) && f->threads[1].state == THREAD_STATE_BLOCKED &&
        thread_wait_active(&f->threads[1]) && !process_exit_info(&f->process, &info) &&
        !endpoint_closed(&f->endpoints[0]) && !endpoint_closed(&f->endpoints[1]);
    user_fixture_check("NONFINAL THREAD EXIT LEAVES DOMAIN OPEN", sibling_live);
    if (!sibling_live || !send_sibling(f, &c)) goto done;
    u64 last_tid = f->threads[1].id;
    u64 frames = pmm_stats().free_pages;
    bool last = peer_returns_closed(f, &c) && dead(&f->threads[1]) && *c.counter == 1 &&
        process_exit_info(&f->process, &info) && info.reason == PROCESS_EXIT_NORMAL &&
        info.process_id == f->process.id && info.thread_id == last_tid &&
        !info.vector && !info.error_code && !info.rip && !info.cr2 && pmm_stats().free_pages == frames;
    user_fixture_check("LAST EXIT CLOSES OWNED ENDPOINTS / RETAINS NORMAL RECORD", last);
    pass = last;
done:
    return finish_case(f, pass);
}

static bool forced_case(void) {
    terminal_writeln("  FORCED QUIESCE:");
    UserTestFixture *f = user_fixture_begin("FORCED PROCESS TERMINAL RECORD");
    if (!f) return false;
    user_fixture_set_quiet(f, true);
    ExitCase c;
    bool pass = false;
    if (!setup(f, &c, 6, false, EXIT_SIBLING_READY)) goto done;
    u64 pid = f->process.id, tid = f->threads[0].id;
    ProcessExitInfo info;
    pass = task_quiesce_process(&f->process) && !process_thread_count(&f->process) &&
        process_exit_info(&f->process, &info) && info.reason == PROCESS_EXIT_TERMINATED &&
        info.process_id == pid && info.thread_id == tid && !info.vector && !info.error_code &&
        !info.rip && !info.cr2 && endpoint_closed(&f->endpoints[0]) && endpoint_closed(&f->endpoints[1]);
    user_fixture_check("FORCED RECORD SURVIVES THREAD REAP", pass);
done:
    return finish_case(f, pass);
}

void process_exit_test_run(void) {
    terminal_writeln("PROCESS EXIT / DOMAIN FAULT TEST:");
    if (!timer_initialized() || !timer_frequency() || scheduler_preemption_enabled() ||
        !user_fixture_available()) {
        terminal_writeln("PROCESS EXIT / DOMAIN FAULT TEST: FAILED (PREFLIGHT)");
        return;
    }
    bool pass = normal_case();
    if (!user_fixture_busy()) { bool ok = forced_case(); pass = ok && pass; }
    const u64 vectors[] = { 0ULL, 6ULL, 7ULL, 13ULL, 14ULL };
    for (u32 i = 0; i < ARRAY_COUNT(vectors) && !user_fixture_busy(); ++i) {
        bool ok = fault_case(vectors[i], EXIT_SIBLING_READY, false); pass = ok && pass;
    }
    if (!user_fixture_busy()) { bool ok = fault_case(6ULL, EXIT_SIBLING_BLOCKED, false); pass = ok && pass; }
    if (!user_fixture_busy()) { bool ok = fault_case(13ULL, EXIT_SIBLING_COMPLETED, true); pass = ok && pass; }
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PROCESS EXIT / DOMAIN FAULT TEST: "); terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
