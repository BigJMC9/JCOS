#include "process_terminate_test.h"

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

typedef struct {
    u64 virtual_address;
    frame_t frame;
    bool allocated;
    bool mapped;
} ProcessKillStack;

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

static bool stack_create(AddressSpace *space, ProcessKillStack *stack, u64 virtual_address,
    CapabilityHandle capability, u64 mode) {
    if (!space || !stack || !virtual_address) {
        return false;
    }

    k_memset(stack, 0, sizeof(*stack));

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

    u64 initial_rsp = virtual_address + VM_PAGE_SIZE - PROCESS_KILL_STARTUP_SIZE;
    u64 offset = initial_rsp - virtual_address;
    u64 *startup = (u64 *)(void *)(direct + offset);

    startup[0] = capability;
    startup[1] = mode;
    return true;
}

static u64 stack_initial_rsp(const ProcessKillStack *stack) {
    if (!stack || !stack->mapped) {
        return 0;
    }

    return (
        stack->virtual_address +
        VM_PAGE_SIZE -
        PROCESS_KILL_STARTUP_SIZE
    );
}

static bool stack_release(AddressSpace *space, ProcessKillStack *stack) {
    if (!stack) return false;
    if (!stack->allocated) return true;
    if (stack->mapped) {
        frame_t old = FRAME_INVALID;

        if (!space || !address_space_unmap_page(space, stack->virtual_address, &old) || old != stack->frame) {
            return false;
        }
        stack->mapped = false;
    }
    if (!frame_free(stack->frame)) return false;

    stack->allocated = false;
    stack->frame = FRAME_INVALID;
    return true;
}

void process_terminate_test_run(void) {
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

    u32 kernel_caps_before = capability_table_count(kernel_caps);
    Process process;
    Endpoint receive_endpoint;
    Endpoint send_endpoint;
    UserElfImage image;
    Thread receive_thread;
    Thread send_thread;
    ProcessKillStack receive_stack;
    ProcessKillStack send_stack;

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
    bool process_quiesced = false;
    AddressSpace *space = 0;
    CapabilityTable *user_caps = 0;

    process_created = process_create(&process);
    print_test("PROCESS CREATE", process_created);

    if (!process_created) goto cleanup;

    space = process_address_space(&process);
    user_caps = process_capabilities(&process);

    if (!space || !user_caps) {
        goto cleanup;
    }

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

    bool caps_ready = kernel_receive_send_cap && kernel_receive_receive_cap && kernel_send_send_cap &&
        kernel_send_receive_cap && user_receive_cap && user_send_cap;

    print_test("CAPABILITIES", caps_ready);

    if (!caps_ready) goto cleanup;

    image_loaded = user_elf_load(&process, file, &image);
    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    bool receive_stack_ok = stack_create(space, &receive_stack, PROCESS_KILL_STACK_A, user_receive,
        JCOS_RTC_MODE_RECEIVE);

    bool send_stack_ok = receive_stack_ok &&
        stack_create(space, &send_stack, PROCESS_KILL_STACK_B, user_send, JCOS_RTC_MODE_SEND);

    print_test("USER STACKS", receive_stack_ok && send_stack_ok);

    if (!send_stack_ok) goto cleanup;

    bool receive_created = thread_create(&receive_thread, &process);
    bool send_created = receive_created && thread_create(&send_thread, &process);

    bool ownership_list = send_created && process_thread_count(&process) == 2ULL &&
        process_thread_contains(&process, &receive_thread) &&
        process_thread_contains(&process, &send_thread);

    print_test("THREAD OWNERSHIP LIST", ownership_list);

    if (!ownership_list) goto cleanup;

    bool receive_prepared = thread_prepare_user(&receive_thread, image.entry, stack_initial_rsp(&receive_stack));

    bool send_prepared = receive_prepared &&
        thread_prepare_user(&send_thread, image.entry, stack_initial_rsp(&send_stack));

    bool receive_queued = send_prepared && scheduler_add(&receive_thread);
    bool send_queued = receive_queued && scheduler_add(&send_thread);

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
        receive_thread.state == THREAD_STATE_BLOCKED && !receive_thread.on_run_queue &&
        thread_wait_matches(&receive_thread, THREAD_WAIT_IPC_RECEIVE, &receive_endpoint) &&
        send_thread.state == THREAD_STATE_BLOCKED && !send_thread.on_run_queue &&
        thread_wait_matches(&send_thread, THREAD_WAIT_IPC_SEND, &send_endpoint) &&
        send_endpoint. waiting_sender_message_ready && scheduler_thread_count() == 1ULL;

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

    bool mixed_wait_states = receive_woken && receive_thread.state == THREAD_STATE_READY &&
        receive_thread.on_run_queue &&
        thread_wait_matches(&receive_thread, THREAD_WAIT_IPC_RECEIVE, &receive_endpoint) &&
        send_thread.state == THREAD_STATE_BLOCKED && !send_thread.on_run_queue &&
        thread_wait_matches(&send_thread, THREAD_WAIT_IPC_SEND, &send_endpoint) &&
        scheduler_thread_count() == 2ULL;

    print_test("MIXED WAIT STATES", mixed_wait_states);

    if (!mixed_wait_states) goto cleanup;

    /*
     * Kill every Thread in the user Process,
     * cancel its IPC ownership, reap it, and
     * revoke the Process's capabilities.
     */
    process_quiesced = task_quiesce_process(&process);

    bool process_empty = process_quiesced && process_thread_count(&process) == 0ULL &&
        !process_thread_first(&process) && capability_table_count(user_caps) == 0U &&
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
        staged_send_discarded && !endpoint_receiver_waiting(&receive_endpoint) &&
        !endpoint_sender_waiting(&receive_endpoint) && !endpoint_receiver_waiting(&send_endpoint) &&
        !endpoint_sender_waiting(&send_endpoint);

    print_test("ENDPOINT SEMANTICS", endpoint_semantics);

cleanup: {
        /*
         * Cleanup can safely retry Process
         * quiescing. Empty capability tables are
         * valid inputs to capability_revoke_all().
         */
        bool quiescent = !process_created;

        if (process_created) quiescent = task_quiesce_process(&process);

        bool receive_stack_clean = !receive_stack.allocated;

        if (quiescent && receive_stack.allocated) {
            receive_stack_clean = stack_release(space, &receive_stack);
        }

        bool send_stack_clean = !send_stack.allocated;

        if (quiescent && send_stack.allocated) {
            send_stack_clean = stack_release(space, &send_stack);
        }

        bool image_clean = !image_loaded;

        if (quiescent && image_loaded) {
            image_clean = user_elf_unload(&process, &image);
        }

        bool kernel_receive_send_revoked = !kernel_receive_send_cap;
        bool kernel_receive_receive_revoked = !kernel_receive_receive_cap;
        bool kernel_send_send_revoked = !kernel_send_send_cap;
        bool kernel_send_receive_revoked = !kernel_send_receive_cap;

        if (quiescent) {
            if (kernel_receive_send_cap) {
                kernel_receive_send_revoked = capability_revoke(kernel_caps, kernel_receive_send);
            }
            if (kernel_receive_receive_cap) {
                kernel_receive_receive_revoked = capability_revoke(kernel_caps, kernel_receive_receive);
            }
            if (kernel_send_send_cap) kernel_send_send_revoked = capability_revoke(kernel_caps, kernel_send_send);
            if (kernel_send_receive_cap) {
                kernel_send_receive_revoked = capability_revoke(kernel_caps, kernel_send_receive);
            }
        }

        bool kernel_caps_restored = capability_table_count(kernel_caps) == kernel_caps_before;

        /* Normal setup failures may leave an unreserved mailbox message. */
        if (quiescent && receive_endpoint_created && !endpoint_receiver_waiting(&receive_endpoint) &&
            !endpoint_sender_waiting(&receive_endpoint) && endpoint_message_ready(&receive_endpoint)) {
            
                IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            (void)endpoint_try_receive(&receive_endpoint, &discard);
        }
        if (quiescent && send_endpoint_created && !endpoint_receiver_waiting(&send_endpoint) &&
            !endpoint_sender_waiting(&send_endpoint) && endpoint_message_ready(&send_endpoint)) {

            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            (void)endpoint_try_receive(&send_endpoint, &discard);
        }

        bool receive_endpoint_clean = !receive_endpoint_created;

        if (quiescent && receive_endpoint_created && kernel_receive_send_revoked && kernel_receive_receive_revoked) {
            receive_endpoint_clean = endpoint_destroy(&receive_endpoint);
        }

        bool send_endpoint_clean = !send_endpoint_created;

        if (quiescent && send_endpoint_created && kernel_send_send_revoked && kernel_send_receive_revoked) {
            send_endpoint_clean = endpoint_destroy(&send_endpoint);
        }

        bool process_clean = !process_created;

        if (quiescent && process_created && receive_stack_clean && send_stack_clean && image_clean &&
            kernel_caps_restored && receive_endpoint_clean && send_endpoint_clean) {
            process_clean = process_destroy(&process);
        }

        u64 after_cookie = supervisor_cookie ^ 0x0101010101010101ULL;
        u64 after_reply = 0;

        bool supervisor_after = process_clean && supervisor_ping(after_cookie, &after_reply) &&
            after_reply == after_cookie;

        print_test("SUPERVISOR AFTER", supervisor_after);

        PmmStats after = pmm_stats();
        bool frames_restored = before.free_pages == after.free_pages;
        terminal_write("  FREE AFTER: ");
        terminal_write_u64(after.free_pages);
        terminal_putchar('\n');

        print_test("PROCESS CLEANUP",
            quiescent && receive_stack_clean && send_stack_clean && image_clean && kernel_caps_restored && 
            receive_endpoint_clean && send_endpoint_clean && process_clean);

        print_test("FRAME COUNT RESTORED", frames_restored);

        bool pass = main_ok && supervisor_before && file_ok && process_created && caps_ready &&
            image_loaded && receive_stack_ok && send_stack_ok && ownership_list && send_queued &&
            prefilled && both_blocked && mixed_wait_states && process_quiesced && process_empty &&
            stale_handles && endpoint_semantics && quiescent && receive_stack_clean && send_stack_clean &&
            image_clean && kernel_caps_restored && receive_endpoint_clean && send_endpoint_clean &&
            process_clean && supervisor_after && frames_restored;

        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("PROCESS TERMINATION TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());

        if (!quiescent) {
            terminal_set_color(terminal_error_color());
            terminal_writeln("PROCESS STILL OWNS LIVE THREAD STATE.");
            terminal_writeln("REBOOT BEFORE FURTHER TESTING.");
            terminal_set_color(terminal_default_color());
        }
    }
}