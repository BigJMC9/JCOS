#include "r2_stress_test.h"
#include "user_test_fixture.h"

#include "address_space.h"
#include "arch.h"
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

#define R2_STRESS_PATH "/bin/runtimecanceltest.elf"
#define R2_STRESS_ITERATIONS 64U
#define R2_STRESS_STACK_RX (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define R2_STRESS_STACK_TX (ADDRESS_SPACE_USER_BASE + 0x102000ULL)
#define R2_STRESS_STARTUP_SIZE (2ULL * sizeof(u64))
#define R2_STRESS_RFLAGS_IF (1ULL << 9)

typedef enum {
    R2_STRESS_BOTH_BLOCKED = 0,

    R2_STRESS_RX_READY,
    R2_STRESS_TX_COMMITTED,
    R2_STRESS_BOTH_READY,

    R2_STRESS_RX_CLOSE_BLOCKED,
    R2_STRESS_TX_CLOSE_BLOCKED,

    R2_STRESS_RX_READY_THEN_CLOSE,
    R2_STRESS_TX_COMMITTED_THEN_CLOSE
} R2StressScenario;

static u64 stress_interrupt_save(void) {
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

static void stress_interrupt_restore(u64 flags) {
    if (flags & R2_STRESS_RFLAGS_IF) interrupts_enable();
}

static bool stress_schedule_once(void) {
    u64 flags = stress_interrupt_save();
    bool result = scheduler_yield();

    stress_interrupt_restore(flags);
    return result;
}

static const char *stress_scenario_name(R2StressScenario scenario) {
    switch (scenario) {
        case R2_STRESS_BOTH_BLOCKED:
            return "BOTH BLOCKED";

        case R2_STRESS_RX_READY:
            return "RX READY";

        case R2_STRESS_TX_COMMITTED:
            return "TX COMMITTED";

        case R2_STRESS_BOTH_READY:
            return "BOTH READY";

        case R2_STRESS_RX_CLOSE_BLOCKED:
            return "RX CLOSE/BLOCKED";

        case R2_STRESS_TX_CLOSE_BLOCKED:
            return "TX CLOSE/BLOCKED";

        case R2_STRESS_RX_READY_THEN_CLOSE:
            return "RX READY/CLOSE";

        case R2_STRESS_TX_COMMITTED_THEN_CLOSE:
            return "TX COMMIT/CLOSE";

        default:
            return "UNKNOWN";
    }
}

static void stress_print_cycle(u32 iteration, R2StressScenario scenario, bool pass) {
    terminal_write("  CYCLE ");
    terminal_write_u64((u64)iteration + 1ULL);
    terminal_write(" [");
    terminal_write(stress_scenario_name(scenario));
    terminal_write("]: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void stress_fill_message(IpcMessage *message, bool prefill) {
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

static bool stress_message_matches(const IpcMessage *message, bool prefill) {
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

static bool stress_drain_message(Process *kernel_process, CapabilityHandle handle, bool expected_prefill) {
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));

    if (!ipc_try_receive(kernel_process, handle, &message)) {
        return false;
    }

    if (!stress_message_matches(&message, expected_prefill)) {
        return false;
    }

    IpcMessage extra;
    k_memset(&extra, 0, sizeof(extra));

    return !ipc_try_receive(kernel_process, handle, &extra);
}

static bool stress_supervisor_ping(u32 iteration) {
    u64 cookie = 0x5232535452455353ULL ^ (u64)iteration;
    u64 reply = 0;

    return (
        supervisor_ping(cookie, &reply) &&
        reply == cookie
    );
}

static bool stress_iteration(const VfsNode *file, u32 iteration, u32 kernel_caps_baseline, u64 frame_baseline) {
    R2StressScenario scenario = (R2StressScenario)(iteration % 8U);
    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;
    Thread *main_thread = thread_current();

    if (!file || !kernel_process || !kernel_caps || !main_thread ||
        main_thread->process != kernel_process || main_thread->state != THREAD_STATE_RUNNING ||
        !main_thread->on_run_queue || scheduler_thread_count() != 1ULL || scheduler_preemption_enabled()) {
        return false;
    }

    UserTestFixture *fixture = user_fixture_begin("R2 STRESS ITERATION");
    if (!fixture) return false;
    bool behavior_completed = false;
    user_fixture_set_quiet(fixture, true);

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
    bool process_quiescent = false;
    bool iteration_ok = false;
    AddressSpace *space = 0;
    CapabilityTable *user_caps = 0;

    process_created = user_fixture_process_create(fixture);

    if (!process_created) goto cleanup;

    space = process_address_space(&fixture->process);
    user_caps = process_capabilities(&fixture->process);

    if (!space || !user_caps) goto cleanup;

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

    if (!user_send_cap) goto cleanup;

    image_loaded = user_fixture_load(fixture, file);

    if (!image_loaded) goto cleanup;
    if (!user_fixture_stack_create(fixture, 0U, R2_STRESS_STACK_RX, user_receive, JCOS_RTC_MODE_RECEIVE)) {
        goto cleanup;
    }
    if (!user_fixture_stack_create(fixture, 1U, R2_STRESS_STACK_TX, user_send, JCOS_RTC_MODE_SEND)) {
        goto cleanup;
    }
    if (!user_fixture_thread_create(fixture, 0U)) {
        goto cleanup;
    }
    if (!user_fixture_thread_create(fixture, 1U)) {
        goto cleanup;
    }
    if (process_thread_count(&fixture->process) != 2ULL || !process_thread_contains(&fixture->process, &fixture->threads[0]) ||
        !process_thread_contains(&fixture->process, &fixture->threads[1])) {
        goto cleanup;
    }
    if (!thread_prepare_user(&fixture->threads[0], fixture->image.entry, user_fixture_stack_rsp(fixture, 0U))) {
        goto cleanup;
    }
    if (!thread_prepare_user(&fixture->threads[1], fixture->image.entry, user_fixture_stack_rsp(fixture, 1U))) {
        goto cleanup;
    }
    if (!scheduler_add(&fixture->threads[0]) || !scheduler_add(&fixture->threads[1])) {
        goto cleanup;
    }

    IpcMessage prefill;
    stress_fill_message(&prefill, true);

    if (!ipc_try_send(kernel_process, kernel_send_send, &prefill)) {
        goto cleanup;
    }

    interrupt_clear_user_fault();

    /*
     * RECEIVE blocks.
     *
     * SEND then runs automatically, finds its
     * mailbox full, and blocks.
     *
     * Control returns to the kernel main Thread.
     */
    if (!stress_schedule_once()) goto cleanup;
    if (thread_current() != main_thread || fixture->threads[0].state != THREAD_STATE_BLOCKED ||
        fixture->threads[0].on_run_queue ||
        !thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0]) ||
        fixture->threads[1].state != THREAD_STATE_BLOCKED || fixture->threads[1].on_run_queue ||
        !thread_wait_matches(&fixture->threads[1], THREAD_WAIT_IPC_SEND, &fixture->endpoints[1]) ||
        !fixture->endpoints[1]. waiting_sender_message_ready || scheduler_thread_count() != 1ULL) {
        goto cleanup;
    }

    bool wake_receive = scenario == R2_STRESS_RX_READY || scenario == R2_STRESS_BOTH_READY || scenario == R2_STRESS_RX_READY_THEN_CLOSE;
    bool commit_send = scenario == R2_STRESS_TX_COMMITTED || scenario == R2_STRESS_BOTH_READY || scenario == R2_STRESS_TX_COMMITTED_THEN_CLOSE;

    bool close_receive = scenario == R2_STRESS_RX_CLOSE_BLOCKED || scenario == R2_STRESS_RX_READY_THEN_CLOSE;
    bool close_send = scenario == R2_STRESS_TX_CLOSE_BLOCKED || scenario == R2_STRESS_TX_COMMITTED_THEN_CLOSE;

    if (wake_receive) {
        IpcMessage wake;
        stress_fill_message(&wake, false);

        if (!ipc_try_send(kernel_process, kernel_receive_send, &wake)) {
            goto cleanup;
        }
        if (fixture->threads[0].state != THREAD_STATE_READY || !fixture->threads[0].on_run_queue ||
            !thread_wait_matches(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0])) {
            goto cleanup;
        }
    }

    if (commit_send) {
        IpcMessage old;
        k_memset(&old, 0, sizeof(old));

        if (!ipc_try_receive(kernel_process, kernel_send_receive, &old) || !stress_message_matches(&old, true)) {
            goto cleanup;
        }
        if (fixture->threads[1].state != THREAD_STATE_READY || !fixture->threads[1].on_run_queue ||
            !thread_wait_matches(&fixture->threads[1], THREAD_WAIT_IPC_SEND, &fixture->endpoints[1]) ||
            fixture->endpoints[1]. waiting_sender_message_ready || !endpoint_message_ready(&fixture->endpoints[1])) {
            goto cleanup;
        }
    }

    if (close_receive) {
        if (!ipc_endpoint_close(&fixture->endpoints[0])) {
            goto cleanup;
        }
        if (!endpoint_closed(&fixture->endpoints[0]) || fixture->threads[0].state != THREAD_STATE_READY ||
            !fixture->threads[0].on_run_queue ||
            !thread_wait_cancelled(&fixture->threads[0], THREAD_WAIT_IPC_RECEIVE, &fixture->endpoints[0])) {
            goto cleanup;
        }
    }

    if (close_send) {
        if (!ipc_endpoint_close(&fixture->endpoints[1])) {
            goto cleanup;
        }
        if (!endpoint_closed(&fixture->endpoints[1]) || fixture->threads[1].state != THREAD_STATE_READY || !fixture->threads[1].on_run_queue) {
            goto cleanup;
        }

        bool should_cancel = !commit_send;

        if (thread_wait_cancelled(&fixture->threads[1], THREAD_WAIT_IPC_SEND, &fixture->endpoints[1]) != should_cancel) {
            goto cleanup;
        }
    }

    /* Simulate service/process death before either continuation gets another timeslice. */
    process_quiescent = user_fixture_quiesce(fixture);

    if (!process_quiescent || process_thread_count(&fixture->process) != 0ULL || process_thread_first(&fixture->process) ||
        capability_table_count(user_caps) != 0U || scheduler_thread_count() != 1ULL ||
        thread_current() != main_thread) {
        goto cleanup;
    }

    /* User handles must now be stale. */
    void *object = 0;

    if (capability_lookup(user_caps, user_receive, CAPABILITY_TYPE_ENDPOINT, &object) ||
        capability_lookup(user_caps, user_send, CAPABILITY_TYPE_ENDPOINT, &object)) {
        goto cleanup;
    }

    /* RECEIVE semantics after killing the owning Process. */
    if (close_receive) {
        if (!endpoint_closed(&fixture->endpoints[0]) || endpoint_message_ready(&fixture->endpoints[0])) {
            goto cleanup;
        }

    } else if (wake_receive) {
        if (!stress_drain_message(kernel_process, kernel_receive_receive, false)) {
            goto cleanup;
        }

    } else {
        if (endpoint_message_ready(&fixture->endpoints[0])) {
            goto cleanup;
        }
    }

    /* SEND semantics after killing the Process. */
    if (close_send) {
        if (!endpoint_closed(&fixture->endpoints[1]) || endpoint_message_ready(&fixture->endpoints[1])) {
            goto cleanup;
        }

    } else if (commit_send) {
        if (!stress_drain_message(kernel_process, kernel_send_receive, false)) {
            goto cleanup;
        }

    } else {
        if (!stress_drain_message(kernel_process, kernel_send_receive, true)) {
            goto cleanup;
        }
    }

    if (endpoint_receiver_waiting(&fixture->endpoints[0]) || endpoint_sender_waiting(&fixture->endpoints[0]) ||
        endpoint_receiver_waiting(&fixture->endpoints[1]) || endpoint_sender_waiting(&fixture->endpoints[1])) {
        goto cleanup;
    }

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));

    if (interrupt_last_user_fault(&fault)) goto cleanup;
    iteration_ok = true;

    behavior_completed = iteration_ok;

cleanup: {
        bool pass = user_fixture_finish(fixture, behavior_completed);
        return pass && pmm_stats().free_pages == frame_baseline &&
            capability_table_count(kernel_caps) == kernel_caps_baseline;
    }
}

void r2_stress_test_run(void) {
    if (!user_fixture_available()) return;
    terminal_writeln("R2 LIFETIME STRESS TEST:");

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    bool initial_state = main_thread && kernel_process && kernel_caps &&
        main_thread->process == kernel_process && main_thread->state == THREAD_STATE_RUNNING &&
        main_thread->on_run_queue && scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    if (!initial_state) {
        terminal_writeln("  INITIAL STATE: FAILED");
        return;
    }

    VfsNode *file = vfs_resolve(vfs_root(), R2_STRESS_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;

    if (!file_ok) {
        terminal_writeln("  ELF FILE: FAILED");
        return;
    }

    PmmStats before = pmm_stats();
    u64 frame_baseline = before.free_pages;
    u32 kernel_caps_baseline = capability_table_count(kernel_caps);
    terminal_write("  ITERATIONS: ");
    terminal_write_u64(R2_STRESS_ITERATIONS);
    terminal_putchar('\n');
    terminal_write("  FREE BASELINE: ");
    terminal_write_u64(frame_baseline);
    terminal_putchar('\n');
    terminal_write("  KERNEL CAP BASELINE: ");
    terminal_write_u64(kernel_caps_baseline);
    terminal_putchar('\n');

    bool pass = true;

    for (u32 i = 0; i < R2_STRESS_ITERATIONS; ++i) {
        R2StressScenario scenario = (R2StressScenario)(i % 8U);
        bool cycle = stress_iteration(file, i, kernel_caps_baseline, frame_baseline);

        stress_print_cycle(i, scenario, cycle);

        if (!cycle) {
            pass = false;
            break;
        }
    }

    PmmStats after = pmm_stats();
    bool frames_restored = before.free_pages == after.free_pages;
    bool caps_restored = capability_table_count(kernel_caps) == kernel_caps_baseline;
    bool scheduler_restored = scheduler_thread_count() == 1ULL && thread_current() == main_thread;
    bool supervisor_final = !user_fixture_busy() && stress_supervisor_ping(R2_STRESS_ITERATIONS + 1U);
    terminal_write("  FREE AFTER: ");
    terminal_write_u64(after.free_pages);
    terminal_putchar('\n');
    terminal_write("  FRAME BASELINE: ");
    terminal_writeln(frames_restored ? "PASS" : "FAILED");
    terminal_write("  CAP BASELINE: ");
    terminal_writeln(caps_restored ? "PASS" : "FAILED");
    terminal_write("  SCHEDULER BASELINE: ");
    terminal_writeln(scheduler_restored ? "PASS" : "FAILED");
    terminal_write("  SUPERVISOR SURVIVED: ");
    terminal_writeln(supervisor_final ? "PASS" : "FAILED");

    pass = pass && frames_restored && caps_restored && scheduler_restored && supervisor_final;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("R2 LIFETIME STRESS TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}