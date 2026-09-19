#include "process_terminate_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "user_elf.h"
#include "vfs.h"
#include "vmm.h"

#include "../include/runtime_cancel_test_abi.h"

#define PROCESS_KILL_PATH "/bin/runtimecanceltest.elf"
#define PROCESS_KILL_STACK_A (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define PROCESS_KILL_STACK_B (ADDRESS_SPACE_USER_BASE + 0x102000ULL)
#define PROCESS_KILL_STARTUP_SIZE (2ULL * sizeof(u64))
#define PROCESS_KILL_RFLAGS_IF (1ULL << 9)

static void print_test(const char *name, bool pass) {
    user_fixture_check(name, pass);
}

static u64 interrupt_save(void) {
    u64 flags = 0;

    __asm__ volatile (
        "pushfq\n\t"
        "popq %0"
        : "=r"(flags)
        :
        : "memory"
    );

    interrupts_disable();
    return flags;
}

static void interrupt_restore(u64 flags) {
    if (flags & PROCESS_KILL_RFLAGS_IF) {
        interrupts_enable();
    }
}

static bool schedule_once(void) {
    u64 flags = interrupt_save();
    bool result = scheduler_yield();

    interrupt_restore(flags);
    return result;
}

static void fill_message(IpcMessage *message, bool prefill) {
    if (!message) return;
    k_memset(message, 0, sizeof(*message));
    message->word_count = IPC_MESSAGE_MAX_WORDS;

    if (prefill) {
        message->words[0] = JCOS_RTC_PREFILL_WORD0;
        message->words[1] = JCOS_RTC_PREFILL_WORD1;
        message->words[2] = JCOS_RTC_PREFILL_WORD2;
        message->words[3] = JCOS_RTC_PREFILL_WORD3;

    } else {
        message->words[0] = JCOS_RTC_SEND_WORD0;
        message->words[1] = JCOS_RTC_SEND_WORD1;
        message->words[2] = JCOS_RTC_SEND_WORD2;
        message->words[3] = JCOS_RTC_SEND_WORD3;
    }
}

static bool message_matches(const IpcMessage *message, bool prefill) {
    if (!message || message->word_count != IPC_MESSAGE_MAX_WORDS) {
        return false;
    }

    if (prefill) {
        return (
            message->words[0] == JCOS_RTC_PREFILL_WORD0 &&
            message->words[1] == JCOS_RTC_PREFILL_WORD1 &&
            message->words[2] == JCOS_RTC_PREFILL_WORD2 &&
            message->words[3] == JCOS_RTC_PREFILL_WORD3
        );
    }

    return (
        message->words[0] == JCOS_RTC_SEND_WORD0 &&
        message->words[1] == JCOS_RTC_SEND_WORD1 &&
        message->words[2] == JCOS_RTC_SEND_WORD2 &&
        message->words[3] == JCOS_RTC_SEND_WORD3
    );
}

void process_terminate_test_run(void) {
    if (!user_fixture_available()) return;
    terminal_writeln("PROCESS TERMINATION TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    bool main_ok = main_thread && kernel_process && kernel_caps && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    print_test("MAIN THREAD", main_ok);

    if (!main_ok) return;

    u64 supervisor_cookie = 0x50524F434B494C4CULL;
    u64 supervisor_reply = 0;

    bool supervisor_before = supervisor_ping(supervisor_cookie, &supervisor_reply) &&
        supervisor_reply == supervisor_cookie;

    print_test("SUPERVISOR BEFORE", supervisor_before);

    if (!supervisor_before) return;

    VfsNode *file = vfs_resolve(vfs_root(), PROCESS_KILL_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;

    print_test("ELF FILE", file_ok);

    if (!file_ok) return;

    UserTestFixture *fixture = user_fixture_begin("PROCESS TERMINATION TEST");
    if (!fixture) return;
    bool behavior_completed = false;

    CapabilityHandle kernel_receive_send = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_receive_receive = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_send = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_receive = CAPABILITY_INVALID_HANDLE;

    CapabilityHandle user_receive = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle user_send = CAPABILITY_INVALID_HANDLE;

    bool process_created = false;
    bool receive_endpoint_created = false;
    bool send_endpoint_created = false;
    bool image_loaded = false;
    bool kernel_receive_send_cap = false;
    bool kernel_receive_receive_cap = false;
    bool kernel_send_send_cap = false;
    bool kernel_send_receive_cap = false;
    bool user_receive_cap = false;
    bool user_send_cap = false;
    bool process_quiesced = false;
    AddressSpace *space = 0;
    CapabilityTable *user_caps = 0;

    process_created = user_fixture_process_create(fixture);
    print_test("PROCESS CREATE", process_created);

    if (!process_created) goto cleanup;

    space = process_address_space(&fixture->process);
    user_caps = process_capabilities(&fixture->process);

    if (!space || !user_caps) {
        goto cleanup;
    }

    receive_endpoint_created = user_fixture_endpoint_create(fixture, 0U);
    send_endpoint_created = receive_endpoint_created && user_fixture_endpoint_create(fixture, 1U);

    if (!send_endpoint_created) goto cleanup;

    kernel_receive_send_cap =
        user_fixture_grant(fixture, true, 0U, CAPABILITY_RIGHT_SEND, &kernel_receive_send);
    kernel_receive_receive_cap = kernel_receive_send_cap &&
        user_fixture_grant(fixture, true, 0U, CAPABILITY_RIGHT_RECEIVE, &kernel_receive_receive);
    kernel_send_send_cap = kernel_receive_receive_cap &&
        user_fixture_grant(fixture, true, 1U, CAPABILITY_RIGHT_SEND, &kernel_send_send);
    kernel_send_receive_cap = kernel_send_send_cap &&
        user_fixture_grant(fixture, true, 1U, CAPABILITY_RIGHT_RECEIVE, &kernel_send_receive);
    user_receive_cap = kernel_send_receive_cap &&
        user_fixture_grant(fixture, false, 0U, CAPABILITY_RIGHT_RECEIVE, &user_receive);
    user_send_cap = user_receive_cap &&
        user_fixture_grant(fixture, false, 1U, CAPABILITY_RIGHT_SEND, &user_send);

    bool caps_ready = (
        kernel_receive_send_cap && kernel_receive_receive_cap && 
        kernel_send_send_cap && kernel_send_receive_cap && 
        user_receive_cap && user_send_cap
    );
    print_test("CAPABILITIES", caps_ready);

    if (!caps_ready) goto cleanup;

    image_loaded = user_fixture_load(fixture, file);
    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    bool receive_stack_ok = user_fixture_stack_create(fixture, 0U, PROCESS_KILL_STACK_A, user_receive,
        JCOS_RTC_MODE_RECEIVE);

    bool send_stack_ok = receive_stack_ok &&
        user_fixture_stack_create(fixture, 1U, PROCESS_KILL_STACK_B, user_send, JCOS_RTC_MODE_SEND);

    print_test("USER STACKS", receive_stack_ok && send_stack_ok);

    if (!send_stack_ok) goto cleanup;

    bool receive_created = user_fixture_thread_create(fixture, 0U);
    bool send_created = receive_created && user_fixture_thread_create(fixture, 1U);

    bool ownership_list = send_created && process_thread_count(&fixture->process) == 2ULL &&
        process_thread_contains(&fixture->process, &fixture->threads[0]) &&
        process_thread_contains(&fixture->process, &fixture->threads[1]);

    print_test("THREAD OWNERSHIP LIST", ownership_list);

    if (!ownership_list) goto cleanup;

    bool receive_prepared = thread_prepare_user(&fixture->threads[0], fixture->image.entry, user_fixture_stack_rsp(fixture, 0U));

    bool send_prepared = receive_prepared &&
        thread_prepare_user(&fixture->threads[1], fixture->image.entry, user_fixture_stack_rsp(fixture, 1U));

    bool receive_queued = send_prepared && scheduler_add(&fixture->threads[0]);
    bool send_queued = receive_queued && scheduler_add(&fixture->threads[1]);

    print_test("THREADS QUEUED", send_queued);

    if (!send_queued) goto cleanup;

    /* Ensure SEND thread must block. */
    IpcMessage prefill;
    fill_message(&prefill, true);
    bool prefilled = ipc_try_send(kernel_process, kernel_send_send, &prefill);

    print_test("SEND ENDPOINT PREFILLED", prefilled);

    if (!prefilled) goto cleanup;
    interrupt_clear_user_fault();

    /*
     * Main yields to RECEIVE thread.
     *
     * RECEIVE blocks -> SEND thread runs.
     * SEND blocks -> Main resumes.
     */
    bool scheduled = schedule_once();

    bool both_blocked = scheduled && thread_current() == main_thread &&
        fixture->threads[0].state == THREAD_STATE_BLOCKED && !fixture->threads[0].on_run_queue &&
        thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]) &&
        fixture->threads[1].state == THREAD_STATE_BLOCKED && !fixture->threads[1].on_run_queue &&
        thread_wait_matches(&fixture->threads[1], THREAD_WAIT_IPC_SEND, &fixture->endpoints[1]) &&
        fixture->endpoints[1]. waiting_sender_message_ready && scheduler_thread_count() == 1ULL;

    print_test("BOTH THREADS BLOCKED", both_blocked);

    if (!both_blocked) goto cleanup;

    /*
     * Wake the receiver but deliberately do not
     * schedule it.
     *
     * This gives process termination both states
     * simultaneously:
     *
     *   receive Thread = READY reservation
     *   send Thread    = BLOCKED reservation
     */
    IpcMessage wake;
    fill_message(&wake, false);

    bool receive_woken = ipc_try_send(kernel_process, kernel_receive_send, &wake);

    bool mixed_wait_states = receive_woken && fixture->threads[0].state == THREAD_STATE_READY &&
        fixture->threads[0].on_run_queue &&
        thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]) &&
        fixture->threads[1].state == THREAD_STATE_BLOCKED && !fixture->threads[1].on_run_queue &&
        thread_wait_matches(&fixture->threads[1], THREAD_WAIT_IPC_SEND, &fixture->endpoints[1]) &&
        scheduler_thread_count() == 2ULL;

    print_test("MIXED WAIT STATES", mixed_wait_states);

    if (!mixed_wait_states) goto cleanup;

    /*
     * Kill every Thread in the user Process,
     * cancel its IPC ownership, reap it, and
     * revoke the Process's capabilities.
     */
    process_quiesced = user_fixture_quiesce(fixture);

    bool process_empty = process_quiesced && process_thread_count(&fixture->process) == 0ULL &&
        !process_thread_first(&fixture->process) && capability_table_count(user_caps) == 0U &&
        scheduler_thread_count() == 1ULL && thread_current() == main_thread;

    print_test("PROCESS QUIESCED", process_empty);

    if (!process_empty) goto cleanup;

    /* Old user handles must now be stale. */
    void *object = 0;

    bool stale_handles = !capability_lookup(user_caps, user_receive, CAPABILITY_TYPE_ENDPOINT, &object) &&
        !capability_lookup(user_caps, user_send, CAPABILITY_TYPE_ENDPOINT, &object);

    print_test("USER HANDLES REVOKED", stale_handles);

    /*
     * READY receiver was killed before consuming
     * its reserved message.
     *
     * The message must become available again.
     */
    IpcMessage receive_message;
    k_memset(&receive_message, 0, sizeof(receive_message));

    bool receive_message_preserved = ipc_try_receive(kernel_process, kernel_receive_receive, &receive_message) &&
        message_matches(&receive_message, false);

    /*
     * Blocked sender had not committed.
     *
     * Its staged message must be discarded while
     * the original prefill remains.
     */
    IpcMessage send_message;
    k_memset(&send_message, 0, sizeof(send_message));

    bool original_send_preserved = ipc_try_receive(kernel_process, kernel_send_receive, &send_message) &&
        message_matches(&send_message, true);

    IpcMessage second_send;
    k_memset(&second_send, 0, sizeof(second_send));

    bool staged_send_discarded = !ipc_try_receive(kernel_process, kernel_send_receive, &second_send);

    bool endpoint_semantics = receive_message_preserved && original_send_preserved &&
        staged_send_discarded && !endpoint_receiver_waiting(&fixture->endpoints[0]) &&
        !endpoint_sender_waiting(&fixture->endpoints[0]) && !endpoint_receiver_waiting(&fixture->endpoints[1]) &&
        !endpoint_sender_waiting(&fixture->endpoints[1]);

    print_test("ENDPOINT SEMANTICS", endpoint_semantics);

    behavior_completed = main_ok && supervisor_before && file_ok && process_created && caps_ready &&
            image_loaded && receive_stack_ok && send_stack_ok && ownership_list && send_queued &&
            prefilled && both_blocked && mixed_wait_states && process_quiesced && process_empty &&
            stale_handles && endpoint_semantics;

cleanup: {
        bool pass = user_fixture_finish(fixture, behavior_completed);
        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("PROCESS TERMINATION TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}
