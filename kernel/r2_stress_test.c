#include "r2_stress_test.h"

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

typedef struct {
    u64 virtual_address;
    frame_t frame;
    bool allocated;
    bool mapped;
} R2StressStack;

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

static bool stress_stack_create(AddressSpace *space, R2StressStack *stack, u64 virtual_address,
    CapabilityHandle capability, u64 mode) {
    if (!space || !stack || !virtual_address) {
        return false;
    }

    k_memset(stack, 0, sizeof(*stack));

    stack->frame = FRAME_INVALID;
    stack->virtual_address = virtual_address;
    stack->frame = frame_alloc();

    if (stack->frame == FRAME_INVALID) {
        return false;
    }

    stack->allocated = true;

    if (!address_space_map_page(space, stack->virtual_address, stack->frame, VM_WRITE)) {
        return false;
    }

    stack->mapped = true;
    u8 *direct = (u8 *)phys_to_virt(frame_to_phys(stack->frame));

    if (!direct) return false;

    k_memset(direct, 0, (usize)VM_PAGE_SIZE);

    u64 initial_rsp = stack->virtual_address + VM_PAGE_SIZE - R2_STRESS_STARTUP_SIZE;
    u64 offset = initial_rsp - stack->virtual_address;
    u64 *startup = (u64 *)(void *)(direct + offset);

    startup[0] = capability;
    startup[1] = mode;
    return true;
}

static u64 stress_stack_rsp(const R2StressStack *stack) {
    if (!stack || !stack->mapped) {
        return 0;
    }

    return (
        stack->virtual_address +
        VM_PAGE_SIZE -
        R2_STRESS_STARTUP_SIZE
    );
}

static bool stress_stack_release(AddressSpace *space, R2StressStack *stack) {
    if (!stack) return false;
    if (!stack->allocated) return true;
    if (stack->mapped) {
        frame_t old = FRAME_INVALID;

        if (!space || !address_space_unmap_page(space, stack->virtual_address, &old) || old != stack->frame) {
            return false;
        }

        stack->mapped = false;
    }

    if (!frame_free(stack->frame)) {
        return false;
    }

    stack->frame = FRAME_INVALID;
    stack->allocated = false;
    return true;
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

    Process process;
    Endpoint receive_endpoint;
    Endpoint send_endpoint;
    UserElfImage image;
    Thread receive_thread;
    Thread send_thread;
    R2StressStack receive_stack;
    R2StressStack send_stack;

    k_memset(&process, 0, sizeof(process));
    k_memset(&receive_endpoint, 0, sizeof(receive_endpoint));
    k_memset(&send_endpoint, 0, sizeof(send_endpoint));
    k_memset(&image, 0, sizeof(image));
    k_memset(&receive_thread, 0, sizeof(receive_thread));
    k_memset(&send_thread, 0, sizeof(send_thread));
    k_memset(&receive_stack, 0, sizeof(receive_stack));
    k_memset(&send_stack, 0, sizeof(send_stack));

    receive_stack.frame = FRAME_INVALID;
    send_stack.frame = FRAME_INVALID;

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

    process_created = process_create(&process);

    if (!process_created) goto cleanup;

    space = process_address_space(&process);
    user_caps = process_capabilities(&process);

    if (!space || !user_caps) goto cleanup;

    receive_endpoint_created = endpoint_create(&receive_endpoint);

    send_endpoint_created = receive_endpoint_created && endpoint_create(&send_endpoint);

    if (!send_endpoint_created) goto cleanup;

    kernel_receive_send_cap =
        capability_insert(kernel_caps, &receive_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND,
            &kernel_receive_send);

    kernel_receive_receive_cap = kernel_receive_send_cap &&
        capability_insert(kernel_caps, &receive_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &kernel_receive_receive);

    kernel_send_send_cap = kernel_receive_receive_cap &&
        capability_insert(kernel_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND,
            &kernel_send_send);

    kernel_send_receive_cap = kernel_send_send_cap &&
        capability_insert(kernel_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
            &kernel_send_receive);

    user_receive_cap = kernel_send_receive_cap &&
        capability_insert(user_caps, &receive_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
            &user_receive);

    user_send_cap = user_receive_cap &&
        capability_insert(user_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &user_send);

    if (!user_send_cap) goto cleanup;

    image_loaded = user_elf_load(&process, file, &image);

    if (!image_loaded) goto cleanup;
    if (!stress_stack_create(space, &receive_stack, R2_STRESS_STACK_RX, user_receive, JCOS_RTC_MODE_RECEIVE)) {
        goto cleanup;
    }
    if (!stress_stack_create(space, &send_stack, R2_STRESS_STACK_TX, user_send, JCOS_RTC_MODE_SEND)) {
        goto cleanup;
    }
    if (!thread_create(&receive_thread, &process)) {
        goto cleanup;
    }
    if (!thread_create(&send_thread, &process)) {
        goto cleanup;
    }
    if (process_thread_count(&process) != 2ULL || !process_thread_contains(&process, &receive_thread) ||
        !process_thread_contains(&process, &send_thread)) {
        goto cleanup;
    }
    if (!thread_prepare_user(&receive_thread, image.entry, stress_stack_rsp(&receive_stack))) {
        goto cleanup;
    }
    if (!thread_prepare_user(&send_thread, image.entry, stress_stack_rsp(&send_stack))) {
        goto cleanup;
    }
    if (!scheduler_add(&receive_thread) || !scheduler_add(&send_thread)) {
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
    if (thread_current() != main_thread || receive_thread.state != THREAD_STATE_BLOCKED ||
        receive_thread.on_run_queue ||
        !thread_wait_matches(&receive_thread, THREAD_WAIT_IPC_RECEIVE, &receive_endpoint) ||
        send_thread.state != THREAD_STATE_BLOCKED || send_thread.on_run_queue ||
        !thread_wait_matches(&send_thread, THREAD_WAIT_IPC_SEND, &send_endpoint) ||
        !send_endpoint. waiting_sender_message_ready || scheduler_thread_count() != 1ULL) {
        goto cleanup;
    }

    bool wake_receive = scenario == R2_STRESS_RX_READY || scenario == R2_STRESS_BOTH_READY ||
        scenario == R2_STRESS_RX_READY_THEN_CLOSE;

    bool commit_send = scenario == R2_STRESS_TX_COMMITTED || scenario == R2_STRESS_BOTH_READY ||
        scenario == R2_STRESS_TX_COMMITTED_THEN_CLOSE;

    bool close_receive = scenario == R2_STRESS_RX_CLOSE_BLOCKED || scenario == R2_STRESS_RX_READY_THEN_CLOSE;
    bool close_send = scenario == R2_STRESS_TX_CLOSE_BLOCKED || scenario == R2_STRESS_TX_COMMITTED_THEN_CLOSE;

    if (wake_receive) {
        IpcMessage wake;
        stress_fill_message(&wake, false);

        if (!ipc_try_send(kernel_process, kernel_receive_send, &wake)) {
            goto cleanup;
        }
        if (receive_thread.state != THREAD_STATE_READY || !receive_thread.on_run_queue ||
            !thread_wait_matches(&receive_thread, THREAD_WAIT_IPC_RECEIVE, &receive_endpoint)) {
            goto cleanup;
        }
    }

    if (commit_send) {
        IpcMessage old;
        k_memset(&old, 0, sizeof(old));

        if (!ipc_try_receive(kernel_process, kernel_send_receive, &old) || !stress_message_matches(&old, true)) {
            goto cleanup;
        }
        if (send_thread.state != THREAD_STATE_READY || !send_thread.on_run_queue ||
            !thread_wait_matches(&send_thread, THREAD_WAIT_IPC_SEND, &send_endpoint) ||
            send_endpoint. waiting_sender_message_ready || !endpoint_message_ready(&send_endpoint)) {
            goto cleanup;
        }
    }

    if (close_receive) {
        if (!ipc_endpoint_close(&receive_endpoint)) {
            goto cleanup;
        }
        if (!endpoint_closed(&receive_endpoint) || receive_thread.state != THREAD_STATE_READY ||
            !receive_thread.on_run_queue ||
            !thread_wait_cancelled(&receive_thread, THREAD_WAIT_IPC_RECEIVE, &receive_endpoint)) {
            goto cleanup;
        }
    }

    if (close_send) {
        if (!ipc_endpoint_close(&send_endpoint)) {
            goto cleanup;
        }
        if (!endpoint_closed(&send_endpoint) || send_thread.state != THREAD_STATE_READY || !send_thread.on_run_queue) {
            goto cleanup;
        }

        bool should_cancel = !commit_send;

        if (thread_wait_cancelled(&send_thread, THREAD_WAIT_IPC_SEND, &send_endpoint) != should_cancel) {
            goto cleanup;
        }
    }

    /* Simulate service/process death before either continuation gets another timeslice. */
    process_quiescent = task_quiesce_process(&process);

    if (!process_quiescent || process_thread_count(&process) != 0ULL || process_thread_first(&process) ||
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
        if (!endpoint_closed(&receive_endpoint) || endpoint_message_ready(&receive_endpoint)) {
            goto cleanup;
        }

    } else if (wake_receive) {
        if (!stress_drain_message(kernel_process, kernel_receive_receive, false)) {
            goto cleanup;
        }

    } else {
        if (endpoint_message_ready(&receive_endpoint)) {
            goto cleanup;
        }
    }

    /* SEND semantics after killing the Process. */
    if (close_send) {
        if (!endpoint_closed(&send_endpoint) || endpoint_message_ready(&send_endpoint)) {
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

    if (endpoint_receiver_waiting(&receive_endpoint) || endpoint_sender_waiting(&receive_endpoint) ||
        endpoint_receiver_waiting(&send_endpoint) || endpoint_sender_waiting(&send_endpoint)) {
        goto cleanup;
    }

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));

    if (interrupt_last_user_fault(&fault)) goto cleanup;
    iteration_ok = true;

cleanup: {
        /*
         * Always attempt to make the temporary
         * Process completely quiescent before
         * allowing these stack-local objects to
         * disappear.
         */
        bool quiescent = !process_created;

        if (process_created) quiescent = task_quiesce_process(&process);
        if (!quiescent) {
            terminal_set_color(terminal_error_color());
            terminal_writeln("  UNSAFE R2 STRESS CLEANUP.");
            terminal_writeln("  PROCESS RETAINS LIVE THREAD STATE.");
            terminal_set_color(terminal_default_color());

            cpu_halt_forever();
        }

        bool receive_stack_clean = stress_stack_release(space, &receive_stack);
        bool send_stack_clean = stress_stack_release(space, &send_stack);
        bool image_clean = !image_loaded;

        if (image_loaded) image_clean = user_elf_unload(&process, &image);

        bool kernel_receive_send_revoked = !kernel_receive_send_cap;
        bool kernel_receive_receive_revoked = !kernel_receive_receive_cap;
        bool kernel_send_send_revoked = !kernel_send_send_cap;
        bool kernel_send_receive_revoked = !kernel_send_receive_cap;

        if (kernel_receive_send_cap) kernel_receive_send_revoked = capability_revoke(kernel_caps, kernel_receive_send);
        if (kernel_receive_receive_cap) {
            kernel_receive_receive_revoked = capability_revoke(kernel_caps, kernel_receive_receive);
        }

        if (kernel_send_send_cap) kernel_send_send_revoked = capability_revoke(kernel_caps, kernel_send_send);
        if (kernel_send_receive_cap) kernel_send_receive_revoked = capability_revoke(kernel_caps, kernel_send_receive);

        bool kernel_caps_restored = kernel_receive_send_revoked && kernel_receive_receive_revoked &&
            kernel_send_send_revoked && kernel_send_receive_revoked &&
            capability_table_count(kernel_caps) == kernel_caps_baseline;

        /* If setup failed before the semantic checks drained an open mailbox, discard it now. */
        if (receive_endpoint_created && !endpoint_closed(&receive_endpoint) &&
            !endpoint_receiver_waiting(&receive_endpoint) && !endpoint_sender_waiting(&receive_endpoint) &&
            endpoint_message_ready(&receive_endpoint)) {
            IpcMessage discard;

            k_memset(&discard, 0, sizeof(discard));

            (void)endpoint_try_receive(&receive_endpoint, &discard);
        }

        if (send_endpoint_created && !endpoint_closed(&send_endpoint) &&
            !endpoint_receiver_waiting(&send_endpoint) && !endpoint_sender_waiting(&send_endpoint) &&
            endpoint_message_ready(&send_endpoint)) {
            IpcMessage discard;

            k_memset(&discard, 0, sizeof(discard));

            (void)endpoint_try_receive(&send_endpoint, &discard);
        }

        bool receive_endpoint_clean = !receive_endpoint_created;

        if (receive_endpoint_created && kernel_receive_send_revoked && kernel_receive_receive_revoked) {
            receive_endpoint_clean = endpoint_destroy(&receive_endpoint);
        }

        bool send_endpoint_clean = !send_endpoint_created;

        if (send_endpoint_created && kernel_send_send_revoked && kernel_send_receive_revoked) {
            send_endpoint_clean = endpoint_destroy(&send_endpoint);
        }

        bool process_clean = !process_created;

        if (process_created && receive_stack_clean && send_stack_clean && image_clean &&
            kernel_caps_restored && receive_endpoint_clean && send_endpoint_clean) {
            process_clean = process_destroy(&process);
        }

        PmmStats after = pmm_stats();
        bool frames_restored = after.free_pages == frame_baseline;
        bool scheduler_restored = scheduler_thread_count() == 1ULL && thread_current() == main_thread;
        bool supervisor_ok = process_clean && stress_supervisor_ping(iteration);

        return (
            iteration_ok &&
            receive_stack_clean &&
            send_stack_clean &&
            image_clean &&
            kernel_caps_restored &&
            receive_endpoint_clean &&
            send_endpoint_clean &&
            process_clean &&
            frames_restored &&
            scheduler_restored &&
            supervisor_ok
        );
    }
}

void r2_stress_test_run(void) {
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
    bool supervisor_final = stress_supervisor_ping(R2_STRESS_ITERATIONS + 1U);
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