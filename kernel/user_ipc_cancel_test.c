#include "user_ipc_cancel_test.h"

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

static bool endpoint_references_thread(const Endpoint *endpoint, const Thread *thread) {
    if (!endpoint || !thread) return false;

    return (
        endpoint->waiting_receiver == thread ||
        endpoint->waiting_sender == thread
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

    PmmStats before = pmm_stats();
    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    if (!kernel_process || !kernel_caps) {
        print_test("KERNEL STATE", false);
        return false;
    }

    u32 kernel_caps_before = capability_table_count(kernel_caps);
    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    print_test("MAIN THREAD", main_ok);

    if (!main_ok) return false;

    Process process;
    Endpoint endpoint;
    UserElfImage image;
    Thread user;

    k_memset(&process, 0, sizeof(process));
    k_memset(&endpoint, 0, sizeof(endpoint));
    k_memset(&image, 0, sizeof(image));
    k_memset(&user, 0, sizeof(user));

    CapabilityTable *user_caps = 0;
    AddressSpace *space = 0;
    CapabilityHandle user_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_receive_handle = CAPABILITY_INVALID_HANDLE;
    frame_t stack_frame = FRAME_INVALID;
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

    process_created = process_create(&process);
    endpoint_created = process_created && endpoint_create(&endpoint);

    if (process_created) {
        space = process_address_space(&process);
        user_caps = process_capabilities(&process);
    }

    bool setup_base = process_created && endpoint_created && space && user_caps;

    print_test("BASE SETUP", setup_base);

    if (!setup_base) goto cleanup;

    CapabilityRights user_right = receive_case ? CAPABILITY_RIGHT_RECEIVE : CAPABILITY_RIGHT_SEND;
    user_cap = capability_insert(user_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, user_right, &user_handle);

    if (!receive_case) {
        kernel_send_cap = capability_insert(kernel_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &kernel_send_handle);

        kernel_receive_cap = kernel_send_cap &&
            capability_insert(kernel_caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
                &kernel_receive_handle);
    }

    bool caps_ok = user_cap && (receive_case || (kernel_send_cap && kernel_receive_cap));

    print_test("CAPABILITIES", caps_ok);

    if (!caps_ok) goto cleanup;

    image_loaded = user_elf_load(&process, file, &image);

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

    stack_frame = frame_alloc();
    stack_allocated = stack_frame != FRAME_INVALID;

    if (!stack_allocated) goto cleanup;

    stack_mapped = address_space_map_page(space, USER_RTC_STACK, stack_frame, VM_WRITE);

    if (!stack_mapped) goto cleanup;

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(stack_frame));

    if (!stack) goto cleanup;

    k_memset(stack, 0, (usize)VM_PAGE_SIZE);

    u64 initial_rsp = USER_RTC_STACK_TOP - USER_RTC_STARTUP_SIZE;
    u64 offset = initial_rsp - USER_RTC_STACK;
    u64 *startup = (u64 *)(void *)(stack + offset);

    startup[0] = user_handle;
    startup[1] = receive_case ? JCOS_RTC_MODE_RECEIVE : JCOS_RTC_MODE_SEND;

    thread_created = thread_create(&user, &process);

    if (!thread_created) goto cleanup;

    u64 expected_thread_id = user.id;
    bool prepared = thread_prepare_user(&user, image.entry, initial_rsp);
    queued = prepared && scheduler_add(&user);
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
            user.state == THREAD_STATE_BLOCKED && !user.on_run_queue &&
            endpoint.waiting_receiver == &user &&
            thread_wait_matches(&user, THREAD_WAIT_IPC_RECEIVE, &endpoint);

    } else {
        blocked = first_schedule && result->phase == JCOS_RTC_PHASE_WAITING &&
            user.state == THREAD_STATE_BLOCKED && !user.on_run_queue && endpoint.waiting_sender == &user &&
            endpoint. waiting_sender_message_ready &&
            thread_wait_matches(&user, THREAD_WAIT_IPC_SEND, &endpoint);
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

        committed = old_received && prefill_matches(&received) && user.state == THREAD_STATE_READY &&
            user.on_run_queue && endpoint.waiting_sender == &user &&
            !endpoint. waiting_sender_message_ready && endpoint_message_ready(&endpoint) &&
            !thread_wait_cancelled(&user, THREAD_WAIT_IPC_SEND, &endpoint);

        print_test("SEND COMMITTED", committed);
        if (!committed) goto cleanup;
    }

    close_ok = ipc_endpoint_close(&endpoint);
    print_test("ENDPOINT CLOSE", close_ok);
    if (!close_ok) goto cleanup;

    bool expected_cancelled = !committed_case;

    if (receive_case) {
        wake_reserved = endpoint_closed(&endpoint) && user.state == THREAD_STATE_READY &&
            user.on_run_queue && endpoint.waiting_receiver == &user &&
            thread_wait_matches(&user, THREAD_WAIT_IPC_RECEIVE, &endpoint);

        cancellation_state = thread_wait_cancelled(&user, THREAD_WAIT_IPC_RECEIVE, &endpoint) == expected_cancelled;

    } else {
        wake_reserved = endpoint_closed(&endpoint) && user.state == THREAD_STATE_READY &&
            user.on_run_queue && endpoint.waiting_sender == &user &&
            thread_wait_matches(&user, THREAD_WAIT_IPC_SEND, &endpoint);

        cancellation_state = thread_wait_cancelled(&user, THREAD_WAIT_IPC_SEND, &endpoint) == expected_cancelled;
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
    destroy_before_resume_rejected = !endpoint_destroy(&endpoint);
    thread_destroy_before_resume_rejected = !thread_destroy(&user);

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

    wait_released = !thread_wait_active(&user);

    user_dead = user.state == THREAD_STATE_DEAD && !user.on_run_queue && !user.interrupt_context_ready &&
        !user.interrupt_rsp;

    endpoint_idle = endpoint_closed(&endpoint) && !endpoint_message_ready(&endpoint) &&
        !endpoint_receiver_waiting(&endpoint) && !endpoint_sender_waiting(&endpoint) &&
        !endpoint. waiting_sender_message_ready;

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

cleanup: {
        bool reserved = thread_created && (endpoint_references_thread(&endpoint, &user) || thread_wait_active(&user));
        bool thread_reaped = !thread_created;

        if (thread_created && thread_current() != &user && !reserved && user.state != THREAD_STATE_BLOCKED) {
            if (user.on_run_queue) {
                u64 flags = interrupt_save();
                bool removed = scheduler_remove(&user);

                interrupt_restore(flags);

                if (!removed) thread_reaped = false;
            }
            if (!user.on_run_queue && user.state != THREAD_STATE_RUNNING && user.state != THREAD_STATE_BLOCKED) {
                thread_reaped = thread_destroy(&user);
            }
        }

        bool safe = !thread_created || thread_reaped;
        bool stack_clean = !stack_allocated;

        if (safe && stack_mapped) {
            frame_t old = FRAME_INVALID;
            bool unmapped = address_space_unmap_page(space, USER_RTC_STACK, &old);

            if (unmapped && old == stack_frame) {
                stack_mapped = false;

                stack_clean = frame_free(stack_frame);
            }

        } else if (safe && stack_allocated && !stack_mapped) {
            stack_clean = frame_free(stack_frame);
        }

        bool image_clean = !image_loaded;

        if (safe && image_loaded) {
            image_clean = user_elf_unload(&process, &image);
        }

        /* For pre-close setup failures, drain a normal open mailbox if no waiter owns it. */
        if (safe && endpoint_created && !endpoint_closed(&endpoint) &&
            !endpoint_receiver_waiting(&endpoint) && !endpoint_sender_waiting(&endpoint) &&
            endpoint_message_ready(&endpoint)) {

            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            (void)endpoint_try_receive(&endpoint, &discard);
        }

        bool user_cap_revoked = !user_cap;

        if (safe && user_cap && user_caps) {
            user_cap_revoked = capability_revoke(user_caps, user_handle);
        }

        bool kernel_send_revoked = !kernel_send_cap;

        if (safe && kernel_send_cap) {
            kernel_send_revoked = capability_revoke(kernel_caps, kernel_send_handle);
        }

        bool kernel_receive_revoked = !kernel_receive_cap;

        if (safe && kernel_receive_cap) {
            kernel_receive_revoked = capability_revoke(kernel_caps, kernel_receive_handle);
        }

        bool caps_clean = (!user_caps || capability_table_count(user_caps) == 0U) &&
            capability_table_count(kernel_caps) == kernel_caps_before;

        bool endpoint_clean = !endpoint_created;

        if (safe && endpoint_created && user_cap_revoked && kernel_send_revoked && kernel_receive_revoked &&
            caps_clean) {
            endpoint_clean = endpoint_destroy(&endpoint);
        }

        bool process_clean = !process_created;

        if (safe && process_created && thread_reaped && stack_clean && image_clean && caps_clean && endpoint_clean) {
            process_clean = process_destroy(&process);
        }

        PmmStats after = pmm_stats();
        bool frames_restored = before.free_pages == after.free_pages;

        print_test("CLEANUP", thread_reaped && stack_clean && image_clean && caps_clean && endpoint_clean && process_clean);

        print_test("FRAME COUNT RESTORED", frames_restored);

        bool pass = main_ok && setup_base && caps_ok && image_loaded && result_ready && stack_allocated &&
            thread_created && queued && prefilled && blocked && committed && close_ok && wake_reserved &&
            cancellation_state && destroy_before_resume_rejected && thread_destroy_before_resume_rejected &&
            resumed && completed && blocking_result_ok && retries_rejected && receive_cleared &&
            wait_released && user_dead && endpoint_idle && !fault_captured && thread_reaped &&
            stack_clean && image_clean && caps_clean && endpoint_clean && process_clean && frames_restored;

        print_test("CASE RESULT", pass);

        if (!safe) {
            terminal_set_color(terminal_error_color());
            terminal_writeln("  LIVE IPC RESERVATION REMAINS.");
            terminal_writeln("  REBOOT BEFORE MORE TESTING.");
            terminal_set_color(terminal_default_color());
        }

        return pass;
    }
}

void user_ipc_cancel_test_run(void) {
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