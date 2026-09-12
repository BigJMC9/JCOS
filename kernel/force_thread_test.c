#include "force_thread_test.h"

#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"

#include "../include/runtime_cancel_test_abi.h"

#define FORCE_RFLAGS_IF (1ULL << 9)

typedef enum {
    FORCE_CASE_BLOCKED_RECEIVE = 1,
    FORCE_CASE_READY_RECEIVE,
    FORCE_CASE_BLOCKED_SEND,
    FORCE_CASE_READY_SEND
} ForceCase;

static CapabilityHandle g_force_send_handle;
static CapabilityHandle g_force_receive_handle;

static volatile bool g_force_started;
static volatile bool g_force_returned;

static void print_test(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
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
    if (flags & FORCE_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = interrupt_save();
    bool result = scheduler_yield();

    interrupt_restore(flags);
    return result;
}

static void fill_prefill(IpcMessage *message) {
    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTC_PREFILL_WORD0;
    message->words[1] = JCOS_RTC_PREFILL_WORD1;
    message->words[2] = JCOS_RTC_PREFILL_WORD2;
    message->words[3] = JCOS_RTC_PREFILL_WORD3;
}

static void fill_worker_message(IpcMessage *message) {
    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTC_SEND_WORD0;
    message->words[1] = JCOS_RTC_SEND_WORD1;
    message->words[2] = JCOS_RTC_SEND_WORD2;
    message->words[3] = JCOS_RTC_SEND_WORD3;
}

static bool message_matches(const IpcMessage *message, bool prefill) {
    if (!message || message->word_count != IPC_MESSAGE_MAX_WORDS) return false;

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

static void receiver_thread(void *argument) {
    (void)argument;
    g_force_started = true;
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    (void)ipc_receive_blocking(process_kernel(), g_force_receive_handle, &message);

    /* Forced termination must prevent this exact continuation from ever returning. */
    g_force_returned = true;
    scheduler_exit_current();
}

static void sender_thread(void *argument) {
    (void)argument;
    g_force_started = true;
    IpcMessage message;
    fill_worker_message(&message);
    (void)ipc_send_blocking(process_kernel(), g_force_send_handle, &message);
    g_force_returned = true;
    scheduler_exit_current();
}

static bool run_case(ForceCase test_case) {
    PmmStats before = pmm_stats();
    Process *kernel_process = process_kernel();
    CapabilityTable *caps = kernel_process ? process_capabilities(kernel_process) : 0;
    Thread *main_thread = thread_current();

    if (!kernel_process || !caps || !main_thread || main_thread->process != kernel_process ||
        scheduler_thread_count() != 1ULL) {
        return false;
    }

    u32 caps_before = capability_table_count(caps);
    Endpoint endpoint;
    Thread worker;

    k_memset(&endpoint, 0, sizeof(endpoint));
    k_memset(&worker, 0, sizeof(worker));

    g_force_started = false;
    g_force_returned = false;

    g_force_send_handle = CAPABILITY_INVALID_HANDLE;
    g_force_receive_handle = CAPABILITY_INVALID_HANDLE;

    bool endpoint_created = endpoint_create(&endpoint);
    bool send_cap = endpoint_created &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &g_force_send_handle);

    bool receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &g_force_receive_handle);

    if (!receive_cap) return false;

    bool sender_case = test_case == FORCE_CASE_BLOCKED_SEND || test_case == FORCE_CASE_READY_SEND;
    bool ready_case = test_case == FORCE_CASE_READY_RECEIVE || test_case == FORCE_CASE_READY_SEND;

    if (sender_case) {
        IpcMessage prefill;
        fill_prefill(&prefill);

        if (!ipc_try_send(kernel_process, g_force_send_handle, &prefill)) return false;
    }

    bool created = thread_create(&worker, kernel_process);
    bool prepared = created && thread_prepare_kernel(&worker, sender_case ? sender_thread : receiver_thread, 0);
    bool queued = prepared && scheduler_add(&worker);

    if (!queued) return false;

    bool first_schedule = schedule_once();

    bool blocked = first_schedule && g_force_started && !g_force_returned &&
        worker.state == THREAD_STATE_BLOCKED && !worker.on_run_queue && thread_wait_active(&worker);

    print_test("BLOCKED", blocked);

    if (!blocked) return false;
    if (ready_case) {
        if (sender_case) {
            IpcMessage old_message;

            k_memset(&old_message, 0, sizeof(old_message));

            if (!ipc_try_receive(kernel_process, g_force_receive_handle, &old_message) ||
                !message_matches(&old_message, true)) {
                return false;
            }

        } else {
            IpcMessage wake;

            fill_worker_message(&wake);

            if (!ipc_try_send(kernel_process, g_force_send_handle, &wake)) {
                return false;
            }
        }
    }

    bool expected_ready = ready_case ? worker.state == THREAD_STATE_READY &&
        worker.on_run_queue : worker.state == THREAD_STATE_BLOCKED && !worker.on_run_queue;

    bool reservation_before = expected_ready && thread_wait_active(&worker);

    print_test(ready_case ? "READY RESERVATION" : "BLOCKED RESERVATION", reservation_before);

    if (!reservation_before) return false;

    bool terminated = task_terminate_thread(&worker);

    bool dead = terminated && worker.state == THREAD_STATE_DEAD && !worker.on_run_queue &&
        !worker.interrupt_context_ready && !worker.interrupt_rsp && !thread_wait_active(&worker) &&
        !endpoint_receiver_waiting(&endpoint) && !endpoint_sender_waiting(&endpoint) &&
        scheduler_thread_count() == 1ULL;

    print_test("FORCED TERMINATION", dead);
    print_test("CONTINUATION NOT RESUMED", !g_force_returned);

    if (!dead || g_force_returned) return false;

    bool message_state_ok = false;

    if (test_case == FORCE_CASE_BLOCKED_RECEIVE) {
        message_state_ok = !endpoint_message_ready(&endpoint);

    } else if (test_case == FORCE_CASE_READY_RECEIVE) {
        IpcMessage message;
        k_memset(&message, 0, sizeof(message));

        message_state_ok = ipc_try_receive(kernel_process, g_force_receive_handle, &message) &&
            message_matches(&message, false);

    } else if (test_case == FORCE_CASE_BLOCKED_SEND) {
        IpcMessage message;
        k_memset(&message, 0, sizeof(message));

        bool original = ipc_try_receive(kernel_process, g_force_receive_handle, &message) &&
            message_matches(&message, true);

        IpcMessage second;
        k_memset(&second, 0, sizeof(second));

        bool staged_gone = !ipc_try_receive(kernel_process, g_force_receive_handle, &second);
        message_state_ok = original && staged_gone;

    } else {
        IpcMessage promoted;
        k_memset(&promoted, 0, sizeof(promoted));

        message_state_ok = ipc_try_receive(kernel_process, g_force_receive_handle, &promoted) &&
            message_matches(&promoted, false);
    }

    print_test("MESSAGE SEMANTICS", message_state_ok);

    bool reaped = message_state_ok && thread_destroy(&worker);
    bool send_revoked = capability_revoke(caps, g_force_send_handle);
    bool receive_revoked = capability_revoke(caps, g_force_receive_handle);
    bool caps_restored = send_revoked && receive_revoked && capability_table_count(caps) == caps_before;
    bool endpoint_destroyed = caps_restored && endpoint_destroy(&endpoint);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    bool pass = reaped && endpoint_destroyed && frames_restored;

    print_test("THREAD REAP", reaped);
    print_test("CAPS RESTORED", caps_restored);
    print_test("ENDPOINT DESTROY", endpoint_destroyed);
    print_test("FRAME COUNT RESTORED", frames_restored);
    return pass;
}

void force_thread_test_run(void) {
    terminal_writeln("FORCED THREAD TERMINATION TEST:");

    PmmStats before = pmm_stats();
    terminal_writeln(" BLOCKED RECEIVE:");

    bool blocked_receive = run_case(FORCE_CASE_BLOCKED_RECEIVE);
    terminal_writeln(" READY RECEIVE:");

    bool ready_receive = blocked_receive && run_case(FORCE_CASE_READY_RECEIVE);
    terminal_writeln(" BLOCKED SEND:");

    bool blocked_send = ready_receive && run_case(FORCE_CASE_BLOCKED_SEND);
    terminal_writeln(" COMMITTED SEND:");

    bool ready_send = blocked_send && run_case(FORCE_CASE_READY_SEND);
    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');

    bool pass = blocked_receive && ready_receive && blocked_send && ready_send && frames_restored;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("FORCED THREAD TERMINATION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}