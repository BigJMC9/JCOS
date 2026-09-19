#include "user_ipc_cancel_test.h"
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
#include "terminal.h"
#include "thread.h"
#include "user_elf.h"
#include "vfs.h"
#include "vmm.h"

#include "../include/runtime_cancel_test_abi.h"

#define USER_RTC_PATH "/bin/runtimecanceltest.elf"
#define USER_RTC_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define USER_RTC_STACK_TOP (USER_RTC_STACK + VM_PAGE_SIZE)
#define USER_RTC_STARTUP_SIZE (2ULL * sizeof(u64))
#define USER_RTC_RFLAGS_IF (1ULL << 9)

typedef enum {
    RTC_CASE_RECEIVE_CANCEL = 1,

    RTC_CASE_SEND_CANCEL,
    RTC_CASE_SEND_COMMITTED_CLOSE
} RuntimeCancelCase;

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
    if (flags & USER_RTC_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = interrupt_save();
    bool result = scheduler_yield();

    interrupt_restore(flags);
    return result;
}

static void fill_prefill(IpcMessage *message) {
    if (!message) return;

    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTC_PREFILL_WORD0;
    message->words[1] = JCOS_RTC_PREFILL_WORD1;
    message->words[2] = JCOS_RTC_PREFILL_WORD2;
    message->words[3] = JCOS_RTC_PREFILL_WORD3;
}

static bool prefill_matches(const IpcMessage *message) {
    if (!message) return false;

    return (
        message->word_count == IPC_MESSAGE_MAX_WORDS &&
        message->words[0] == JCOS_RTC_PREFILL_WORD0 &&
        message->words[1] == JCOS_RTC_PREFILL_WORD1 &&
        message->words[2] == JCOS_RTC_PREFILL_WORD2 &&
        message->words[3] == JCOS_RTC_PREFILL_WORD3
    );
}

static bool words_zero(const volatile JcosU64 *words) {
    if (!words) return false;
    for (u32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        if (words[i] != 0ULL) return false;
    }

    return true;
}

static bool receive_outputs_zero(const volatile JcosRuntimeCancelTestResult *result) {
    if (!result) return false;

    return (
        result->receive_word_count == 0ULL &&
        words_zero(result->receive_words) &&
        result->retry_try_word_count == 0ULL &&
        words_zero(result->retry_try_words) &&
        result->retry_block_word_count == 0ULL &&
        words_zero(result->retry_block_words)
    );
}

static bool run_case(const VfsNode *file, RuntimeCancelCase test_case) {
    const bool receive_case = test_case == RTC_CASE_RECEIVE_CANCEL;
    const bool committed_case = test_case == RTC_CASE_SEND_COMMITTED_CLOSE;

    if (receive_case) {
        terminal_writeln(" RECEIVE CANCELLATION:");

    } else if (committed_case) {
        terminal_writeln(" COMMITTED SEND + CLOSE:");

    } else terminal_writeln(" UNCOMMITTED SEND CANCELLATION:");

    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    if (!kernel_process || !kernel_caps) {
        print_test("KERNEL STATE", false);
        return false;
    }

    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    print_test("MAIN THREAD", main_ok);

    if (!main_ok) return false;

    UserTestFixture *fixture = user_fixture_begin("USER IPC CANCELLATION CASE");
    if (!fixture) return false;
    bool behavior_completed = false;

    CapabilityTable *user_caps = 0;
    AddressSpace *space = 0;
    CapabilityHandle user_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_receive_handle = CAPABILITY_INVALID_HANDLE;
    bool process_created = false;
    bool endpoint_created = false;
    bool user_cap = false;
    bool kernel_send_cap = false;
    bool kernel_receive_cap = false;
    bool image_loaded = false;
    bool stack_allocated = false;
    bool stack_mapped = false;
    bool thread_created = false;
    bool queued = false;
    bool prefilled = receive_case;
    bool blocked = false;
    bool committed = !committed_case;
    bool close_ok = false;
    bool wake_reserved = false;
    bool cancellation_state = false;
    bool destroy_before_resume_rejected = false;
    bool thread_destroy_before_resume_rejected = false;
    bool resumed = false;
    bool blocking_result_ok = false;
    bool retries_rejected = false;
    bool receive_cleared = !receive_case;
    bool completed = false;
    bool user_dead = false;
    bool wait_released = false;
    bool endpoint_idle = false;
    bool fault_captured = false;

    volatile JcosRuntimeCancelTestResult *result = 0;

    process_created = user_fixture_process_create(fixture);
    endpoint_created = process_created && user_fixture_endpoint_create(fixture, 0U);

    if (process_created) {
        space = process_address_space(&fixture->process);
        user_caps = process_capabilities(&fixture->process);
    }

    bool setup_base = process_created && endpoint_created && space && user_caps;

    print_test("BASE SETUP", setup_base);

    if (!setup_base) goto cleanup;

    CapabilityRights user_right = receive_case ? CAPABILITY_RIGHT_RECEIVE : CAPABILITY_RIGHT_SEND;
    user_cap = user_fixture_grant(fixture, false, 0U,
            user_right, &user_handle);

    if (!receive_case) {
        kernel_send_cap = user_fixture_grant(fixture, true, 0U,
            CAPABILITY_RIGHT_SEND, &kernel_send_handle);

        kernel_receive_cap = kernel_send_cap &&
            user_fixture_grant(fixture, true, 0U,
            CAPABILITY_RIGHT_RECEIVE, &kernel_receive_handle);
    }

    bool caps_ok = user_cap && (receive_case || (kernel_send_cap && kernel_receive_cap));

    print_test("CAPABILITIES", caps_ok);

    if (!caps_ok) goto cleanup;

    image_loaded = user_fixture_load(fixture, file);

    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    frame_t result_frame = FRAME_INVALID;
    vm_flags_t result_flags = 0;
    bool result_mapping = address_space_query_page(space, JCOS_RTC_RESULT_ADDRESS, &result_frame, &result_flags);

    if (result_mapping) result = (volatile JcosRuntimeCancelTestResult *) phys_to_virt(frame_to_phys(result_frame));

    bool result_ready = result && (result_flags & VM_USER) && (result_flags & VM_WRITE) &&
        result->initial_magic == JCOS_RTC_INITIAL_MAGIC && result->completion_magic == 0ULL;

    print_test("RESULT PAGE", result_ready);

    if (!result_ready) goto cleanup;

    stack_allocated = user_fixture_stack_allocate(fixture, 0U, USER_RTC_STACK);

    if (!stack_allocated) goto cleanup;

    stack_mapped = user_fixture_stack_map(fixture, 0U);

    if (!stack_mapped) goto cleanup;

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(fixture->stacks[0].frame));

    if (!stack) goto cleanup;

    k_memset(stack, 0, (usize)VM_PAGE_SIZE);

    u64 initial_rsp = USER_RTC_STACK_TOP - USER_RTC_STARTUP_SIZE;
    u64 offset = initial_rsp - USER_RTC_STACK;
    u64 *startup = (u64 *)(void *)(stack + offset);

    startup[0] = user_handle;
    startup[1] = receive_case ? JCOS_RTC_MODE_RECEIVE : JCOS_RTC_MODE_SEND;

    thread_created = user_fixture_thread_create(fixture, 0U);

    if (!thread_created) goto cleanup;

    u64 expected_thread_id = fixture->threads[0].id;
    bool prepared = thread_prepare_user(&fixture->threads[0], fixture->image.entry, initial_rsp);
    queued = prepared && scheduler_add(&fixture->threads[0]);
    print_test("USER THREAD READY", queued);

    if (!queued) goto cleanup;

    /* SEND cases begin with a full mailbox so the user must genuinely block. */
    if (!receive_case) {
        IpcMessage prefill;
        fill_prefill(&prefill);
        prefilled = ipc_try_send(kernel_process, kernel_send_handle, &prefill);
    }

    print_test("PREFILL", prefilled);
    if (!prefilled) goto cleanup;

    interrupt_clear_user_fault();
    bool first_schedule = schedule_once();

    if (receive_case) {
        blocked = first_schedule && result->phase == JCOS_RTC_PHASE_WAITING &&
            fixture->threads[0].state == THREAD_STATE_BLOCKED && !fixture->threads[0].on_run_queue &&
            fixture->endpoints[0].waiting_receiver == &fixture->threads[0] &&
            thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]);

    } else {
        blocked = first_schedule && result->phase == JCOS_RTC_PHASE_WAITING &&
            fixture->threads[0].state == THREAD_STATE_BLOCKED && !fixture->threads[0].on_run_queue && fixture->endpoints[0].waiting_sender == &fixture->threads[0] &&
            fixture->endpoints[0]. waiting_sender_message_ready &&
            thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_SEND, &fixture->endpoints[0]);
    }

    print_test("USER BLOCKED", blocked);

    if (!blocked) goto cleanup;

    /*
     * Committed-send case:
     *
     * consume the pre-existing message first,
     * which promotes the user's staged message
     * and wakes the user.
     */
    if (committed_case) {
        IpcMessage received;
        k_memset(&received, 0, sizeof(received));
        bool old_received = ipc_try_receive(kernel_process, kernel_receive_handle, &received);

        committed = old_received && prefill_matches(&received) && fixture->threads[0].state == THREAD_STATE_READY &&
            fixture->threads[0].on_run_queue && fixture->endpoints[0].waiting_sender == &fixture->threads[0] &&
            !fixture->endpoints[0]. waiting_sender_message_ready && endpoint_message_ready(&fixture->endpoints[0]) &&
            !thread_wait_cancelled(&fixture->threads[0], THREAD_WAIT_IPC_SEND, &fixture->endpoints[0]);

        print_test("SEND COMMITTED", committed);
        if (!committed) goto cleanup;
    }

    close_ok = ipc_endpoint_close(&fixture->endpoints[0]);
    print_test("ENDPOINT CLOSE", close_ok);
    if (!close_ok) goto cleanup;

    bool expected_cancelled = !committed_case;

    if (receive_case) {
        wake_reserved = endpoint_closed(&fixture->endpoints[0]) && fixture->threads[0].state == THREAD_STATE_READY &&
            fixture->threads[0].on_run_queue && fixture->endpoints[0].waiting_receiver == &fixture->threads[0] &&
            thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]);

        cancellation_state = thread_wait_cancelled(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]) == expected_cancelled;

    } else {
        wake_reserved = endpoint_closed(&fixture->endpoints[0]) && fixture->threads[0].state == THREAD_STATE_READY &&
            fixture->threads[0].on_run_queue && fixture->endpoints[0].waiting_sender == &fixture->threads[0] &&
            thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_SEND, &fixture->endpoints[0]);

        cancellation_state = thread_wait_cancelled(&fixture->threads[0], THREAD_WAIT_IPC_SEND, &fixture->endpoints[0]) == expected_cancelled;
    }

    print_test("WAKE RESERVATION KEPT", wake_reserved);
    print_test(committed_case ? "COMMIT PRESERVED" : "WAIT CANCELLED", cancellation_state);

    if (!wake_reserved || !cancellation_state) {
        goto cleanup;
    }

    /*
     * Closed does not mean destroyable yet.
     *
     * The resumed continuation still owns the
     * lifetime reservation.
     */
    destroy_before_resume_rejected = !endpoint_destroy(&fixture->endpoints[0]);
    thread_destroy_before_resume_rejected = !thread_destroy(&fixture->threads[0]);

    print_test("EARLY ENDPOINT DESTROY REJECTED", destroy_before_resume_rejected);
    print_test("EARLY THREAD DESTROY REJECTED", thread_destroy_before_resume_rejected);

    if (!destroy_before_resume_rejected || !thread_destroy_before_resume_rejected) {
        goto cleanup;
    }

    resumed = schedule_once();
    print_test("USER RESUMED", resumed);

    if (!resumed) goto cleanup;

    completed = result->phase == JCOS_RTC_PHASE_COMPLETE &&
        result->completion_magic == JCOS_RTC_COMPLETE_MAGIC && result->thread_id == expected_thread_id;

    JcosU64 expected_block_result = committed_case ? JCOS_SYSCALL_RESULT_OK : JCOS_SYSCALL_RESULT_FAILED;
    blocking_result_ok = result->blocking_result == expected_block_result;

    retries_rejected = result->retry_try_result == JCOS_SYSCALL_RESULT_FAILED &&
        result->retry_block_result == JCOS_SYSCALL_RESULT_FAILED;

    if (receive_case) receive_cleared = receive_outputs_zero(result);

    wait_released = !thread_wait_active(&fixture->threads[0]);

    user_dead = fixture->threads[0].state == THREAD_STATE_DEAD && !fixture->threads[0].on_run_queue && !fixture->threads[0].interrupt_context_ready &&
        !fixture->threads[0].interrupt_rsp;

    endpoint_idle = endpoint_closed(&fixture->endpoints[0]) && !endpoint_message_ready(&fixture->endpoints[0]) &&
        !endpoint_receiver_waiting(&fixture->endpoints[0]) && !endpoint_sender_waiting(&fixture->endpoints[0]) &&
        !fixture->endpoints[0]. waiting_sender_message_ready;

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    fault_captured = interrupt_last_user_fault(&fault);

    print_test("BLOCKING RESULT", blocking_result_ok);
    print_test("CLOSED IPC REJECTED", retries_rejected);

    if (receive_case) print_test("RECEIVE OUTPUT CLEARED", receive_cleared);

    print_test("WAIT RELEASED", wait_released);
    print_test("THREAD EXIT", user_dead);
    print_test("ENDPOINT QUIESCENT", endpoint_idle);
    print_test("NO USER FAULT", !fault_captured);

    behavior_completed = main_ok && setup_base && caps_ok && image_loaded && result_ready && stack_allocated &&
            thread_created && queued && prefilled && blocked && committed && close_ok && wake_reserved &&
            cancellation_state && destroy_before_resume_rejected && thread_destroy_before_resume_rejected &&
            resumed && completed && blocking_result_ok && retries_rejected && receive_cleared &&
            wait_released && user_dead && endpoint_idle && !fault_captured;

cleanup: {
        bool pass = user_fixture_finish(fixture, behavior_completed);
        print_test("CASE RESULT", pass);
        return pass;
    }
}

void user_ipc_cancel_test_run(void) {
    if (!user_fixture_available()) return;
    terminal_writeln("USER IPC CANCELLATION TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    VfsNode *file = vfs_resolve(vfs_root(), USER_RTC_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;

    print_test("ELF FILE", file_ok);

    if (!file_ok) return;

    bool receive_cancel = run_case(file, RTC_CASE_RECEIVE_CANCEL);
    bool send_cancel = receive_cancel && run_case(file, RTC_CASE_SEND_CANCEL);
    bool committed_close = send_cancel && run_case(file, RTC_CASE_SEND_COMMITTED_CLOSE);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');

    print_test("TOTAL FRAME COUNT RESTORED", frames_restored);

    bool pass = file_ok && receive_cancel && send_cancel && committed_close && frames_restored;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USER IPC CANCELLATION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}