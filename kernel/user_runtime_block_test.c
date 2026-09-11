#include "user_runtime_block_test.h"

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

#include "../include/runtime_block_test_abi.h"

#define USER_RTB_PATH "/bin/runtimeblocktest.elf"
#define USER_RTB_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define USER_RTB_STACK_TOP (USER_RTB_STACK + VM_PAGE_SIZE)
#define USER_RTB_STARTUP_SIZE (2ULL * sizeof(u64))
#define USER_RTB_RFLAGS_IF (1ULL << 9)

_Static_assert(JCOS_RTB_RESULT_ADDRESS == ADDRESS_SPACE_USER_BASE + (2ULL * VM_PAGE_SIZE), "runtime blocking result address mismatch");

static void print_test(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 test_interrupt_save(void) {
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

static void test_interrupt_restore(u64 flags) {
    if (flags & USER_RTB_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = test_interrupt_save();
    bool result = scheduler_yield();

    test_interrupt_restore(flags);
    return result;
}

static bool message_matches(const IpcMessage *message, u64 word0, u64 word1, u64 word2, u64 word3) {
    if (!message) return false;

    return (
        message->word_count == IPC_MESSAGE_MAX_WORDS &&
        message->words[0] == word0 &&
        message->words[1] == word1 &&
        message->words[2] == word2 &&
        message->words[3] == word3
    );
}

static bool result_words_zero(const volatile JcosU64 *words) {
    if (!words) return false;
    for (u32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        if (words[i] != 0ULL) return false;
    }

    return true;
}

static bool result_receive1_ok(const volatile JcosRuntimeBlockTestResult *result) {
    if (!result) return false;

    return (
        result->receive1_probe_mask == JCOS_RTB_PROBE_EXPECTED &&
        result->receive1_word_count == JCOS_IPC_MESSAGE_MAX_WORDS &&
        result->receive1_words[0] == JCOS_RTB_RX1_WORD0 &&
        result->receive1_words[1] == JCOS_RTB_RX1_WORD1 &&
        result->receive1_words[2] == JCOS_RTB_RX1_WORD2 &&
        result->receive1_words[3] == JCOS_RTB_RX1_WORD3
    );
}

static bool result_receive2_ok(const volatile JcosRuntimeBlockTestResult *result) {
    if (!result) return false;

    return (
        result->receive2_probe_mask == JCOS_RTB_PROBE_EXPECTED &&
        result->receive2_word_count == JCOS_IPC_MESSAGE_MAX_WORDS &&
        result->receive2_words[0] == JCOS_RTB_RX2_WORD0 &&
        result->receive2_words[1] == JCOS_RTB_RX2_WORD1 &&
        result->receive2_words[2] == JCOS_RTB_RX2_WORD2 &&
        result->receive2_words[3] == JCOS_RTB_RX2_WORD3
    );
}

static void fill_receive1(IpcMessage *message) {
    if (!message) return;

    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTB_RX1_WORD0;
    message->words[1] = JCOS_RTB_RX1_WORD1;
    message->words[2] = JCOS_RTB_RX1_WORD2;
    message->words[3] = JCOS_RTB_RX1_WORD3;
}

static void fill_receive2(IpcMessage *message) {
    if (!message) return;

    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTB_RX2_WORD0;
    message->words[1] = JCOS_RTB_RX2_WORD1;
    message->words[2] = JCOS_RTB_RX2_WORD2;
    message->words[3] = JCOS_RTB_RX2_WORD3;
}

static void fill_prefill(IpcMessage *message) {
    if (!message) return;

    k_memset(message, 0, sizeof(*message));

    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTB_PREFILL_WORD0;
    message->words[1] = JCOS_RTB_PREFILL_WORD1;
    message->words[2] = JCOS_RTB_PREFILL_WORD2;
    message->words[3] = JCOS_RTB_PREFILL_WORD3;
}

static bool prefill_matches(const IpcMessage *message) {
    return message_matches(message, JCOS_RTB_PREFILL_WORD0, JCOS_RTB_PREFILL_WORD1, JCOS_RTB_PREFILL_WORD2, JCOS_RTB_PREFILL_WORD3);
}

static bool tx_matches(const IpcMessage *message) {
    return message_matches(message, JCOS_RTB_TX_WORD0, JCOS_RTB_TX_WORD1, JCOS_RTB_TX_WORD2, JCOS_RTB_TX_WORD3);
}

static bool endpoint_references_thread(const Endpoint *endpoint, const Thread *thread) {
    if (!endpoint || !thread) return false;

    return (endpoint->waiting_receiver == thread || endpoint->waiting_sender == thread);
}

static bool drain_endpoint(Endpoint *endpoint) {
    if (!endpoint) return false;
    if (endpoint->waiting_receiver || endpoint->waiting_sender || endpoint->waiting_sender_message_ready) return false;

    if (!endpoint_message_ready(endpoint)) return true;

    IpcMessage discard;
    k_memset(&discard, 0, sizeof(discard));

    if (!endpoint_try_receive(endpoint, &discard)) return false;
    return !endpoint_message_ready(endpoint);
}

void user_runtime_block_test_run(void) {
    terminal_writeln("USER C BLOCKING RUNTIME TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();
    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;
    u32 kernel_caps_before = kernel_caps ? capability_table_count(kernel_caps) : 0U;

    bool main_ok = main_thread && kernel_process && kernel_caps && main_thread->process == kernel_process &&
        main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    print_test("MAIN THREAD", main_ok);
    terminal_write("  KERNEL CAPS BEFORE: ");
    terminal_write_u64(kernel_caps_before);
    terminal_putchar('\n');

    if (!main_ok) return;

    VfsNode *file = vfs_resolve(vfs_root(), USER_RTB_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;

    print_test("ELF FILE", file_ok);

    if (!file_ok) return;

    Process process;
    Endpoint receive_endpoint;
    Endpoint send_endpoint;
    UserElfImage image;
    Thread user;

    k_memset(&process, 0, sizeof(process));
    k_memset(&receive_endpoint, 0, sizeof(receive_endpoint));
    k_memset(&send_endpoint, 0, sizeof(send_endpoint));
    k_memset(&image, 0, sizeof(image));
    k_memset(&user, 0, sizeof(user));

    AddressSpace *space = 0;
    CapabilityTable *user_caps = 0;

    CapabilityHandle kernel_receive_send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle kernel_send_receive_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle user_receive_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle user_send_handle = CAPABILITY_INVALID_HANDLE;

    frame_t stack_frame = FRAME_INVALID;
    bool process_created = false;
    bool receive_endpoint_created = false;
    bool send_endpoint_created = false;
    bool kernel_receive_send_cap = false;
    bool kernel_send_send_cap = false;
    bool kernel_send_receive_cap = false;
    bool user_receive_cap = false;
    bool user_send_cap = false;
    bool image_loaded = false;
    bool stack_allocated = false;
    bool stack_mapped = false;
    bool stack_ready = false;
    bool thread_created = false;
    bool prepared = false;
    bool queued = false;
    bool first_schedule = false;
    bool preblock_contract = false;
    bool entry_alignment = false;
    bool thread_id_ok = false;
    bool receive1_blocked = false;
    bool receive1_woken = false;
    bool second_schedule = false;
    bool receive1_returned = false;
    bool receive2_blocked = false;
    bool receive2_woken = false;
    bool prefill_sent = false;
    bool third_schedule = false;
    bool receive2_returned = false;
    bool send_blocked = false;
    bool staged_payload = false;
    bool prefill_received = false;
    bool prefill_payload = false;
    bool send_promoted = false;
    bool fourth_schedule = false;
    bool send_returned = false;
    bool completion_ok = false;
    bool user_dead = false;
    bool tx_received = false;
    bool tx_payload = false;
    bool endpoints_idle = false;
    bool fault_captured = false;

    volatile JcosRuntimeBlockTestResult *result = 0;
    process_created = process_create(&process);
    print_test("PROCESS CREATE", process_created);

    if (!process_created) goto cleanup;
    space = process_address_space(&process);
    user_caps = process_capabilities(&process);

    bool space_ok = space && !space->kernel && address_space_cr3(space);
    bool user_caps_empty = user_caps && capability_table_count(user_caps) == 0U;

    print_test("ADDRESS SPACE", space_ok);
    print_test("USER CAP TABLE EMPTY", user_caps_empty);

    if (!space_ok || !user_caps_empty) {
        goto cleanup;
    }

    receive_endpoint_created = endpoint_create(&receive_endpoint);
    send_endpoint_created = receive_endpoint_created && endpoint_create(&send_endpoint);

    print_test("RECEIVE ENDPOINT", receive_endpoint_created);
    print_test("SEND ENDPOINT", send_endpoint_created);

    if (!send_endpoint_created) goto cleanup;

    /* Kernel -> user receive endpoint. */
    kernel_receive_send_cap =
        capability_insert(kernel_caps, &receive_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND,
            &kernel_receive_send_handle);

    user_receive_cap = kernel_receive_send_cap &&
        capability_insert(user_caps, &receive_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
            &user_receive_handle);

    /*
     * User -> kernel send endpoint.
     *
     * Kernel receives the actual test message,
     * but also needs SEND authority so it can
     * pre-fill the mailbox before the user's
     * blocking send.
     */
    kernel_send_send_cap = user_receive_cap &&
        capability_insert(kernel_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND,
            &kernel_send_send_handle);

    kernel_send_receive_cap = kernel_send_send_cap &&
        capability_insert(kernel_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE,
            &kernel_send_receive_handle);

    user_send_cap = kernel_send_receive_cap &&
        capability_insert(user_caps, &send_endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND,
            &user_send_handle);

    bool caps_ready = kernel_receive_send_cap && kernel_send_send_cap && kernel_send_receive_cap &&
        user_receive_cap && user_send_cap;

    print_test("KERNEL/USER CAPS", caps_ready);
    if (!caps_ready) goto cleanup;

    image_loaded = user_elf_load(&process, file, &image);
    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    frame_t result_frame = FRAME_INVALID;
    vm_flags_t result_flags = 0;
    bool result_mapping = address_space_query_page(space, JCOS_RTB_RESULT_ADDRESS, &result_frame, &result_flags);
    frame_t gap_frame = FRAME_INVALID;
    vm_flags_t gap_flags = 0;

    bool gap_unmapped = !address_space_query_page(space, ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE, &gap_frame,
        &gap_flags);

    bool elf_layout = image.entry == ADDRESS_SPACE_USER_BASE && image.page_count == 2U && result_mapping &&
        (result_flags & VM_USER) && (result_flags & VM_WRITE) && gap_unmapped;

    print_test("ELF LAYOUT", elf_layout);

    if (!elf_layout) goto cleanup;

    result = (volatile JcosRuntimeBlockTestResult *) phys_to_virt(frame_to_phys(result_frame));

    bool result_ready = result && result->initial_magic == JCOS_RTB_INITIAL_MAGIC &&
        result->completion_magic == 0ULL && result->phase == JCOS_RTB_PHASE_START;

    print_test("RESULT INITIALIZED", result_ready);
    if (!result_ready) goto cleanup;

    stack_frame = frame_alloc();
    stack_allocated = stack_frame != FRAME_INVALID;
    print_test("USER STACK FRAME", stack_allocated);
    if (!stack_allocated) goto cleanup;

    stack_mapped = address_space_map_page(space, USER_RTB_STACK, stack_frame, VM_WRITE);
    print_test("USER STACK MAP", stack_mapped);
    if (!stack_mapped) goto cleanup;

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(stack_frame));

    if (stack) {
        k_memset(stack, 0, (usize)VM_PAGE_SIZE);

        u64 initial_rsp = USER_RTB_STACK_TOP - USER_RTB_STARTUP_SIZE;
        u64 offset = initial_rsp - USER_RTB_STACK;
        u64 *startup = (u64 *)(void *)(stack + offset);

        /*
         * runtime_block_test.c:
         *
         *   arg0 = RECEIVE cap
         *   arg1 = SEND cap
         */
        startup[0] = user_receive_handle;
        startup[1] = user_send_handle;

        stack_ready = true;
    }

    print_test("STARTUP STACK", stack_ready);

    if (!stack_ready) goto cleanup;

    thread_created = thread_create(&user, &process);
    print_test("USER THREAD CREATE", thread_created);

    if (!thread_created) goto cleanup;

    u64 expected_thread_id = user.id;
    u64 initial_rsp = USER_RTB_STACK_TOP - USER_RTB_STARTUP_SIZE;

    prepared = thread_prepare_user(&user, image.entry, initial_rsp);
    print_test("PREPARE USER", prepared);

    if (!prepared) goto cleanup;

    queued = scheduler_add(&user);
    print_test("QUEUE USER", queued);
    if (!queued) goto cleanup;

    interrupt_clear_user_fault();

    /*
     * ------------------------------------------------
     * RECEIVE #1
     * ------------------------------------------------
     *
     * User executes ordinary C until it enters
     * jcos_runtime_probe_receive(), which calls the
     * shared blocking receive wrapper.
     *
     * Empty endpoint => user must BLOCK.
     */
    first_schedule = schedule_once();

    preblock_contract = result && result->null_receive_result == JCOS_SYSCALL_RESULT_FAILED &&
        result->null_send_result == JCOS_SYSCALL_RESULT_FAILED &&
        result->wrong_right_receive_result == JCOS_SYSCALL_RESULT_FAILED &&
        result->wrong_right_receive_word_count == 0ULL &&
        result_words_zero(result->wrong_right_receive_words) &&
        result->wrong_right_send_result == JCOS_SYSCALL_RESULT_FAILED;

    entry_alignment = result && result->entry_rsp_mod16 == 8ULL;
    thread_id_ok = result && result->thread_id == expected_thread_id;

    receive1_blocked = first_schedule && thread_current() == main_thread && result &&
        result->phase == JCOS_RTB_PHASE_WAIT_RX1 && user.state == THREAD_STATE_BLOCKED &&
        !user.on_run_queue && user.interrupt_context_ready && user.interrupt_rsp &&
        receive_endpoint.waiting_receiver == &user && scheduler_thread_count() == 1ULL;

    print_test("BLOCKING FAILURE CONTRACT", preblock_contract);
    print_test("C ENTRY STACK ALIGNMENT", entry_alignment);
    print_test("THREAD ID", thread_id_ok);
    print_test("RECEIVE 1 BLOCKED", receive1_blocked);

    if (!preblock_contract || !entry_alignment || !thread_id_ok || !receive1_blocked) goto cleanup;

    IpcMessage receive1_message;
    fill_receive1(&receive1_message);
    bool receive1_sent = ipc_try_send(kernel_process, kernel_receive_send_handle, &receive1_message);

    receive1_woken = receive1_sent && user.state == THREAD_STATE_READY && user.on_run_queue &&
        receive_endpoint.waiting_receiver == &user && endpoint_message_ready(&receive_endpoint) &&
        scheduler_thread_count() == 2ULL;

    print_test("RECEIVE 1 WAKE", receive1_woken);

    if (!receive1_woken) goto cleanup;
    second_schedule = schedule_once();
    receive1_returned = second_schedule && result_receive1_ok(result);

    receive2_blocked = second_schedule && thread_current() == main_thread && result &&
        result->phase == JCOS_RTB_PHASE_WAIT_RX2 && user.state == THREAD_STATE_BLOCKED &&
        !user.on_run_queue && user.interrupt_context_ready && user.interrupt_rsp &&
        receive_endpoint.waiting_receiver == &user && !endpoint_message_ready(&receive_endpoint) &&
        scheduler_thread_count() == 1ULL;

    print_test("RECEIVE 1 RETURNED", receive1_returned);
    print_test("RECEIVE 1 ABI REGISTERS", result && result->receive1_probe_mask == JCOS_RTB_PROBE_EXPECTED);
    print_test("RECEIVE 1 PAYLOAD", receive1_returned);
    print_test("RECEIVE 2 BLOCKED", receive2_blocked);

    if (!receive1_returned || !receive2_blocked) goto cleanup;

    /* ------------------------------------------------ RECEIVE #2 ------------------------------------------------ */
    IpcMessage receive2_message;
    fill_receive2(&receive2_message);
    bool receive2_sent = ipc_try_send(kernel_process, kernel_receive_send_handle, &receive2_message);

    receive2_woken = receive2_sent && user.state == THREAD_STATE_READY && user.on_run_queue &&
        receive_endpoint.waiting_receiver == &user && endpoint_message_ready(&receive_endpoint) &&
        scheduler_thread_count() == 2ULL;

    print_test("RECEIVE 2 WAKE", receive2_woken);
    if (!receive2_woken) goto cleanup;

    /*
     * Fill the other endpoint before allowing
     * Ring3 to run again.
     *
     * Once RECEIVE #2 returns, the subsequent
     * blocking SEND must therefore take its
     * genuine blocking path.
     */
    IpcMessage prefill_message;
    fill_prefill(&prefill_message);
    prefill_sent = ipc_try_send(kernel_process, kernel_send_send_handle, &prefill_message);
    print_test("SEND MAILBOX PREFILLED", prefill_sent);

    if (!prefill_sent) goto cleanup;

    third_schedule = schedule_once();
    receive2_returned = third_schedule && result_receive2_ok(result);

    send_blocked = third_schedule && thread_current() == main_thread && result &&
        result->phase == JCOS_RTB_PHASE_WAIT_SEND && user.state == THREAD_STATE_BLOCKED &&
        !user.on_run_queue && user.interrupt_context_ready && user.interrupt_rsp &&
        send_endpoint.waiting_sender == &user && send_endpoint. waiting_sender_message_ready &&
        endpoint_message_ready(&send_endpoint) && scheduler_thread_count() == 1ULL;

    staged_payload = send_blocked && tx_matches(&send_endpoint. waiting_sender_message);

    print_test("RECEIVE 2 RETURNED", receive2_returned);
    print_test("RECEIVE 2 ABI REGISTERS", result && result->receive2_probe_mask == JCOS_RTB_PROBE_EXPECTED);
    print_test("RECEIVE 2 PAYLOAD", receive2_returned);
    print_test("BLOCKING SEND BLOCKED", send_blocked);
    print_test("STAGED SEND PAYLOAD", staged_payload);

    if (!receive2_returned || !send_blocked || !staged_payload) goto cleanup;

    /*
     * ------------------------------------------------
     * RELEASE BLOCKING SEND
     * ------------------------------------------------
     *
     * Consuming the pre-filled message creates
     * mailbox room. ipc_receive_locked() must:
     *
     *   1. return our original prefill
     *   2. promote the user's staged SEND
     *   3. wake the Ring3 sender
     *   4. retain waiting_sender until that exact
     *      continuation resumes
     */
    IpcMessage first_outbound;
    k_memset(&first_outbound, 0, sizeof(first_outbound));
    prefill_received = ipc_try_receive(kernel_process, kernel_send_receive_handle, &first_outbound);
    prefill_payload = prefill_received && prefill_matches(&first_outbound);

    send_promoted = prefill_received && user.state == THREAD_STATE_READY && user.on_run_queue &&
        send_endpoint.waiting_sender == &user && !send_endpoint. waiting_sender_message_ready &&
        endpoint_message_ready(&send_endpoint) && scheduler_thread_count() == 2ULL;

    print_test("PREFILL RECEIVE", prefill_received);
    print_test("PREFILL PAYLOAD", prefill_payload);
    print_test("STAGED MESSAGE PROMOTED", send_promoted);

    if (!prefill_payload || !send_promoted) goto cleanup;

    /*
     * User resumes inside ipc_send_blocking(),
     * returns through INT 0x80, checks all
     * callee-saved sentinels, returns to C,
     * writes COMPLETE, then exits.
     */
    fourth_schedule = schedule_once();

    send_returned = fourth_schedule && result && result->send_probe_mask == JCOS_RTB_PROBE_EXPECTED;

    completion_ok = fourth_schedule && result && result->phase == JCOS_RTB_PHASE_COMPLETE &&
        result->completion_magic == JCOS_RTB_COMPLETE_MAGIC;

    user_dead = fourth_schedule && thread_current() == main_thread && user.state == THREAD_STATE_DEAD &&
        !user.on_run_queue && !user.interrupt_context_ready && !user.interrupt_rsp &&
        send_endpoint.waiting_sender == 0 && scheduler_thread_count() == 1ULL;

    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    fault_captured = interrupt_last_user_fault(&fault);

    print_test("BLOCKING SEND RETURNED", send_returned);
    print_test("SEND ABI REGISTERS", send_returned);
    print_test("C COMPLETION", completion_ok);
    print_test("THREAD EXIT", user_dead);
    print_test("NO USER FAULT", !fault_captured);

    if (!send_returned || !completion_ok || !user_dead || fault_captured) goto cleanup;

    /* The user's promoted four-word message is still waiting in the mailbox. */
    IpcMessage second_outbound;
    k_memset(&second_outbound, 0, sizeof(second_outbound));

    tx_received = ipc_try_receive(kernel_process, kernel_send_receive_handle, &second_outbound);
    tx_payload = tx_received && tx_matches(&second_outbound);

    endpoints_idle = !endpoint_message_ready(&receive_endpoint) &&
        !endpoint_receiver_waiting(&receive_endpoint) && !endpoint_sender_waiting(&receive_endpoint) &&
        !endpoint_message_ready(&send_endpoint) && !endpoint_receiver_waiting(&send_endpoint) &&
        !endpoint_sender_waiting(&send_endpoint);

    print_test("PROMOTED SEND RECEIVE", tx_received);
    print_test("PROMOTED SEND PAYLOAD", tx_payload);
    print_test("ENDPOINTS IDLE", endpoints_idle);

cleanup: {
        /*
         * Until R2 exists, never reclaim a Thread
         * while an Endpoint still retains a raw
         * pointer to it.
         *
         * This includes READY-but-not-yet-resumed
         * threads, not merely BLOCKED threads.
         */
        bool thread_reserved = thread_created &&
            (endpoint_references_thread(&receive_endpoint, &user) || endpoint_references_thread(&send_endpoint, &user));

        bool thread_reaped = !thread_created;

        if (thread_created && thread_current() != &user && !thread_reserved && user.state != THREAD_STATE_BLOCKED) {
            if (user.on_run_queue) {
                u64 flags = test_interrupt_save();
                bool removed = scheduler_remove(&user);

                test_interrupt_restore(flags);

                if (!removed) thread_reaped = false;
            }

            if (!user.on_run_queue && user.state != THREAD_STATE_RUNNING && user.state != THREAD_STATE_BLOCKED) {
                thread_reaped = thread_destroy(&user);
            }
        }

        bool safe_to_release = !thread_created || thread_reaped;
        print_test("SAFE TO RELEASE", safe_to_release);

        /* Do not free a frame if its mapping could not first be removed successfully. */
        bool stack_clean = !stack_allocated;

        if (safe_to_release && stack_mapped) {
            frame_t old_stack = FRAME_INVALID;
            bool unmapped = address_space_unmap_page(space, USER_RTB_STACK, &old_stack);

            if (unmapped && old_stack == stack_frame) {
                stack_mapped = false;

                stack_clean = frame_free(stack_frame);

            } 
            else stack_clean = false;

        } 
        else if (safe_to_release && stack_allocated && !stack_mapped) stack_clean = frame_free(stack_frame);

        bool image_clean = !image_loaded;
        if (safe_to_release && image_loaded) image_clean = user_elf_unload(&process, &image);

        /*
         * A normal failure may leave one mailbox
         * message behind.
         *
         * Drain only after the Thread is gone and
         * only if no Endpoint waiter exists.
         */
        bool receive_drained = !receive_endpoint_created;
        bool send_drained = !send_endpoint_created;

        if (safe_to_release && receive_endpoint_created) receive_drained = drain_endpoint(&receive_endpoint);
        if (safe_to_release && send_endpoint_created)  send_drained = drain_endpoint(&send_endpoint);

        bool kernel_receive_send_revoked = !kernel_receive_send_cap;
        bool kernel_send_send_revoked = !kernel_send_send_cap;
        bool kernel_send_receive_revoked = !kernel_send_receive_cap;
        bool user_receive_revoked = !user_receive_cap;
        bool user_send_revoked = !user_send_cap;

        if (safe_to_release) {
            if (kernel_receive_send_cap) {
                kernel_receive_send_revoked = capability_revoke(kernel_caps, kernel_receive_send_handle);
            }
            if (kernel_send_send_cap) {
                kernel_send_send_revoked = capability_revoke(kernel_caps, kernel_send_send_handle);
            }
            if (kernel_send_receive_cap) {
                kernel_send_receive_revoked = capability_revoke(kernel_caps, kernel_send_receive_handle);
            }
            if (user_caps && user_receive_cap) {
                user_receive_revoked = capability_revoke(user_caps, user_receive_handle);
            }
            if (user_caps && user_send_cap) {
                user_send_revoked = capability_revoke(user_caps, user_send_handle);
            }
        }

        bool kernel_caps_restored = kernel_caps && capability_table_count(kernel_caps) == kernel_caps_before;
        bool user_caps_restored = !user_caps || capability_table_count(user_caps) == 0U;
        bool receive_endpoint_clean = !receive_endpoint_created;

        if (safe_to_release && receive_endpoint_created && receive_drained && kernel_receive_send_revoked &&
            user_receive_revoked) {
            receive_endpoint_clean = endpoint_destroy(&receive_endpoint);
        }

        bool send_endpoint_clean = !send_endpoint_created;

        if (safe_to_release && send_endpoint_created && send_drained && kernel_send_send_revoked &&
            kernel_send_receive_revoked && user_send_revoked) {
            send_endpoint_clean = endpoint_destroy(&send_endpoint);
        }

        bool process_clean = !process_created;

        if (safe_to_release && process_created && thread_reaped && stack_clean && image_clean &&
            user_caps_restored && receive_endpoint_clean && send_endpoint_clean) {
            process_clean = process_destroy(&process);
        }

        print_test("USER REAP", thread_reaped);
        print_test("STACK CLEANUP", stack_clean);
        print_test("ELF UNLOAD", image_clean);
        print_test("KERNEL CAPS RESTORED", kernel_caps_restored);
        print_test("USER CAPS RESTORED", user_caps_restored);
        print_test("RECEIVE ENDPOINT DESTROY", receive_endpoint_clean);
        print_test("SEND ENDPOINT DESTROY", send_endpoint_clean);
        print_test("PROCESS DESTROY", process_clean);

        PmmStats after = pmm_stats();
        bool frames_restored = before.free_pages == after.free_pages;
        terminal_write("  FREE AFTER: ");
        terminal_write_u64(after.free_pages);
        terminal_putchar('\n');

        print_test("FRAME COUNT RESTORED", frames_restored);

        bool pass = main_ok && file_ok && process_created && receive_endpoint_created &&
            send_endpoint_created && caps_ready && image_loaded && elf_layout && result_ready &&
            stack_allocated && stack_mapped == false && stack_ready && thread_created && prepared &&
            queued && first_schedule && preblock_contract && entry_alignment && thread_id_ok &&
            receive1_blocked && receive1_woken && second_schedule && receive1_returned &&
            receive2_blocked && receive2_woken && prefill_sent && third_schedule && receive2_returned &&
            send_blocked && staged_payload && prefill_received && prefill_payload && send_promoted &&
            fourth_schedule && send_returned && completion_ok && user_dead && !fault_captured &&
            tx_received && tx_payload && endpoints_idle && safe_to_release && thread_reaped &&
            stack_clean && image_clean && kernel_caps_restored && user_caps_restored &&
            receive_endpoint_clean && send_endpoint_clean && process_clean && frames_restored;

        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("USER C BLOCKING RUNTIME TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());

        if (!safe_to_release) {
            terminal_set_color(terminal_error_color());
            terminal_writeln("TEST LEFT A LIVE IPC RESERVATION.");
            terminal_writeln("REBOOT BEFORE RUNNING MORE TESTS.");
            terminal_set_color(terminal_default_color());
        }
    }
}