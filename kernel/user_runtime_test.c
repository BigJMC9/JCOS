#include "user_runtime_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
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

#include "../include/runtime_test_abi.h"

#define USER_RUNTIME_TEST_PATH "/bin/runtimetest.elf"
#define USER_RUNTIME_TEST_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define USER_RUNTIME_TEST_STACK_TOP (USER_RUNTIME_TEST_STACK + VM_PAGE_SIZE)
#define USER_RUNTIME_TEST_STARTUP_SIZE (2ULL * sizeof(u64))

_Static_assert(JCOS_RUNTIME_TEST_RESULT_ADDRESS == ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE,
    "runtime test result address mismatch");

static void print_test(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool result_words_zero(const JcosU64 words[JCOS_IPC_MESSAGE_MAX_WORDS]) {
    if (!words) return false;
    for (u32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        if (words[i] != 0ULL) return false;
    }
    return true;
}

static bool result_payload_ok(const JcosRuntimeTestResult *result) {
    if (!result) return false;

    return (
        result->receive_word_count == JCOS_IPC_MESSAGE_MAX_WORDS &&
        result->receive_words[0] == JCOS_RUNTIME_TEST_WORD0 &&
        result->receive_words[1] == JCOS_RUNTIME_TEST_WORD1 &&
        result->receive_words[2] == JCOS_RUNTIME_TEST_WORD2 &&
        result->receive_words[3] == JCOS_RUNTIME_TEST_WORD3
    );
}

void user_runtime_test_run(void) {
    terminal_writeln("USER C RUNTIME TEST:");

    PmmStats before = pmm_stats();
    terminal_write("  FREE BEFORE: ");
    terminal_write_u64(before.free_pages);
    terminal_putchar('\n');

    Thread *main_thread = thread_current();

    bool main_ok = main_thread && main_thread->state == THREAD_STATE_RUNNING && main_thread->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled();

    print_test("MAIN THREAD", main_ok);

    if (!main_ok) return;

    VfsNode *file = vfs_resolve(vfs_root(), USER_RUNTIME_TEST_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;

    print_test("ELF FILE", file_ok);

    if (!file_ok) return;

    Process process;
    Endpoint endpoint;
    UserElfImage image;
    Thread user;

    k_memset(&process, 0, sizeof(process));
    k_memset(&endpoint, 0, sizeof(endpoint));
    k_memset(&image, 0, sizeof(image));
    k_memset(&user, 0, sizeof(user));

    AddressSpace *space = 0;
    CapabilityTable *caps = 0;
    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    frame_t stack_frame = FRAME_INVALID;
    bool process_created = process_create(&process);
    bool space_ok = false;
    bool caps_ok = false;
    bool endpoint_created = false;
    bool send_cap = false;
    bool receive_cap = false;
    bool image_loaded = false;
    bool image_layout_ok = false;
    bool mappings_ok = false;
    bool initial_data_ok = false;
    bool stack_allocated = false;
    bool stack_mapped = false;
    bool stack_ready = false;
    bool thread_created = false;
    bool prepared = false;
    bool queued = false;
    bool yielded = false;
    bool shell_restored = false;
    bool user_dead = false;
    bool fault_captured = false;
    bool completion_ok = false;
    bool thread_id_ok = false;
    bool data_ok = false;
    bool bss_ok = false;
    bool null_args_ok = false;
    bool wrong_rights_ok = false;
    bool failure_clear_ok = false;
    bool count_rejection_ok = false;
    bool send_ok = false;
    bool full_rejected = false;
    bool receive_ok = false;
    bool payload_ok = false;
    bool endpoint_idle = false;
    JcosRuntimeTestResult *result = 0;

    print_test("PROCESS CREATE", process_created);

    if (!process_created) goto cleanup;

    space = process_address_space(&process);
    caps = process_capabilities(&process);
    space_ok = space && !space->kernel && address_space_cr3(space);
    caps_ok = caps && capability_table_count(caps) == 0U;

    print_test("ADDRESS SPACE", space_ok);
    print_test("CAP TABLE EMPTY", caps_ok);

    if (!space_ok || !caps_ok) goto cleanup;

    endpoint_created = endpoint_create(&endpoint);

    print_test("ENDPOINT CREATE", endpoint_created);

    if (!endpoint_created) goto cleanup;

    send_cap = capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &send_handle);

    receive_cap = send_cap &&
        capability_insert(caps, &endpoint, CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    print_test("SEND CAP", send_cap);
    print_test("RECEIVE CAP", receive_cap);

    if (!receive_cap) goto cleanup;

    image_loaded = user_elf_load(&process, file, &image);

    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    vm_flags_t data_flags = 0;
    bool text_mapping = address_space_query_page(space, ADDRESS_SPACE_USER_BASE, &text_frame, &text_flags);
    bool data_mapping = address_space_query_page(space, JCOS_RUNTIME_TEST_RESULT_ADDRESS, &data_frame, &data_flags);

    image_layout_ok = image.entry == ADDRESS_SPACE_USER_BASE && image.page_count == 2U;

    mappings_ok = text_mapping && data_mapping && !(text_flags & VM_WRITE) && (text_flags & VM_USER) &&
        (data_flags & VM_WRITE) && (data_flags & VM_USER);

    print_test("ELF LAYOUT", image_layout_ok);
    print_test("TEXT/DATA MAPPINGS", mappings_ok);

    if (!image_layout_ok || !mappings_ok) goto cleanup;

    result = (JcosRuntimeTestResult *) phys_to_virt(frame_to_phys(data_frame));

    initial_data_ok = result && result->initial_magic == JCOS_RUNTIME_TEST_INITIAL_MAGIC &&
        result->completion_magic == 0ULL;

    print_test("DATA INITIALIZER", initial_data_ok);

    if (!initial_data_ok) goto cleanup;

    stack_frame = frame_alloc();

    stack_allocated = stack_frame != FRAME_INVALID;

    print_test("USER STACK FRAME", stack_allocated);

    if (!stack_allocated) goto cleanup;

    stack_mapped = address_space_map_page(space, USER_RUNTIME_TEST_STACK, stack_frame, VM_WRITE);

    print_test("USER STACK MAP", stack_mapped);

    if (!stack_mapped) goto cleanup;

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(stack_frame));

    if (stack) {
        k_memset(stack, 0, (usize)VM_PAGE_SIZE);

        u64 initial_rsp = USER_RUNTIME_TEST_STACK_TOP - USER_RUNTIME_TEST_STARTUP_SIZE;
        u64 offset = initial_rsp - USER_RUNTIME_TEST_STACK;
        u64 *startup = (u64 *)(void *)(stack + offset);

        startup[0] = send_handle;
        startup[1] = receive_handle;

        stack_ready = true;
    }

    print_test("STARTUP STACK", stack_ready);

    if (!stack_ready) goto cleanup;

    thread_created = thread_create(&user, &process);
    print_test("USER THREAD CREATE", thread_created);

    if (!thread_created) goto cleanup;

    u64 expected_thread_id = user.id;
    u64 initial_rsp = USER_RUNTIME_TEST_STACK_TOP - USER_RUNTIME_TEST_STARTUP_SIZE;

    prepared = thread_prepare_user(&user, image.entry, initial_rsp);

    print_test("PREPARE USER", prepared);

    if (!prepared) goto cleanup;

    queued = scheduler_add(&user);

    print_test("QUEUE USER", queued);

    if (!queued) goto cleanup;

    interrupt_clear_user_fault();

    interrupts_disable();

    yielded = scheduler_yield();

    interrupts_enable();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    fault_captured = interrupt_last_user_fault(&fault);

    shell_restored = yielded && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1ULL;

    user_dead = shell_restored && user.state == THREAD_STATE_DEAD && !user.on_run_queue &&
        !user.interrupt_context_ready && !user.interrupt_rsp;

    print_test("SHELL RESTORED", shell_restored);
    print_test("THREAD EXIT", user_dead);
    print_test("NO USER FAULT", !fault_captured);

    if (result) {
        completion_ok = result->initial_magic == JCOS_RUNTIME_TEST_INITIAL_MAGIC &&
            result->completion_magic == JCOS_RUNTIME_TEST_COMPLETE_MAGIC;

        thread_id_ok = result->thread_id == expected_thread_id;

        data_ok = result->data_probe_seen == JCOS_RUNTIME_TEST_DATA_MAGIC;

        bss_ok = result->bss_probe_initial == 0ULL && result->bss_probe_after_write == JCOS_RUNTIME_TEST_BSS_MAGIC;

        null_args_ok = result->null_send_result == JCOS_SYSCALL_RESULT_FAILED &&
            result->null_receive_result == JCOS_SYSCALL_RESULT_FAILED;

        wrong_rights_ok = result->wrong_right_send_result == JCOS_SYSCALL_RESULT_FAILED &&
            result->wrong_right_receive_result == JCOS_SYSCALL_RESULT_FAILED;

        failure_clear_ok = result->wrong_right_receive_word_count == 0ULL &&
            result_words_zero(result->wrong_right_receive_words) &&
            result->empty_receive_result == JCOS_SYSCALL_RESULT_FAILED &&
            result->empty_receive_word_count == 0ULL && result_words_zero(result->empty_receive_words);

        count_rejection_ok = result->oversize_send_result == JCOS_SYSCALL_RESULT_FAILED &&
            result->zero_count_send_result == JCOS_SYSCALL_RESULT_FAILED;

        send_ok = result->send_result == JCOS_SYSCALL_RESULT_OK;

        full_rejected = result->full_send_result == JCOS_SYSCALL_RESULT_FAILED;

        receive_ok = result->receive_result == JCOS_SYSCALL_RESULT_OK;

        payload_ok = result_payload_ok(result);
    }

    endpoint_idle = endpoint_created && !endpoint_message_ready(&endpoint) &&
        !endpoint_receiver_waiting(&endpoint) && !endpoint_sender_waiting(&endpoint);

    print_test("C COMPLETION", completion_ok);
    print_test("THREAD ID WRAPPER", thread_id_ok);
    print_test("DATA VALUE", data_ok);
    print_test("BSS ZERO/WRITE", bss_ok);
    print_test("NULL ARGUMENTS", null_args_ok);
    print_test("WRONG RIGHTS", wrong_rights_ok);
    print_test("FAILURE OUTPUT CLEAR", failure_clear_ok);
    print_test("COUNT REJECTION", count_rejection_ok);
    print_test("FOUR-WORD SEND", send_ok);
    print_test("FULL SEND REJECTED", full_rejected);
    print_test("FOUR-WORD RECEIVE", receive_ok);
    print_test("PAYLOAD MATCH", payload_ok);
    print_test("ENDPOINT IDLE", endpoint_idle);

cleanup: {
        /*
         * R1A performs no blocking syscalls.
         *
         * Therefore a BLOCKED state here is an
         * unexpected failure and we deliberately
         * do not destroy through that state.
         *
         * R2 will define general cancellation.
         */
        bool thread_reaped = !thread_created;

        if (thread_created && thread_current() != &user) {
            if (user.on_run_queue) {
                interrupts_disable();

                bool removed = scheduler_remove(&user);

                interrupts_enable();

                if (!removed) thread_reaped = false;
            }

            if (!user.on_run_queue && user.state != THREAD_STATE_RUNNING && user.state != THREAD_STATE_BLOCKED) {
                thread_reaped = thread_destroy(&user);
            }
        }

        bool safe_to_release = !thread_created || thread_reaped;
        bool stack_clean = !stack_allocated;

        if (safe_to_release && stack_mapped) {
            frame_t old_stack = FRAME_INVALID;

            bool unmapped = address_space_unmap_page(space, USER_RUNTIME_TEST_STACK, &old_stack) &&
                old_stack == stack_frame;

            stack_clean = unmapped && frame_free(stack_frame);

        } else if (safe_to_release && stack_allocated) stack_clean = frame_free(stack_frame);

        bool image_clean = !image_loaded;

        if (safe_to_release && image_loaded) image_clean = user_elf_unload(&process, &image);

        /*
         * If a broken runtime left a normal
         * nonblocking message behind, drain it
         * so the harness can still reclaim its
         * temporary endpoint.
         *
         * Never do this if a waiter exists.
         */
        if (safe_to_release && endpoint_created && !endpoint_receiver_waiting(&endpoint) &&
            !endpoint_sender_waiting(&endpoint) && endpoint_message_ready(&endpoint)) {
            IpcMessage discard;

            k_memset(&discard, 0, sizeof(discard));

            (void)endpoint_try_receive(&endpoint, &discard);
        }

        bool send_revoked = !send_cap;
        bool receive_revoked = !receive_cap;

        if (safe_to_release && caps) {
            if (send_cap) send_revoked = capability_revoke(caps, send_handle);
            if (receive_cap) receive_revoked = capability_revoke(caps, receive_handle);
        }

        bool caps_clean = !caps || capability_table_count(caps) == 0U;
        bool endpoint_clean = !endpoint_created;

        if (safe_to_release && endpoint_created && send_revoked && receive_revoked && caps_clean) {
            endpoint_clean = endpoint_destroy(&endpoint);
        }

        bool process_clean = !process_created;

        if (safe_to_release && process_created && thread_reaped && image_clean && caps_clean && endpoint_clean) {
            process_clean = process_destroy(&process);
        }

        print_test("USER REAP", thread_reaped);
        print_test("STACK CLEANUP", stack_clean);
        print_test("ELF UNLOAD", image_clean);
        print_test("CAPS RESTORED", caps_clean && send_revoked && receive_revoked);
        print_test("ENDPOINT DESTROY", endpoint_clean);
        print_test("PROCESS DESTROY", process_clean);

        PmmStats after = pmm_stats();
        bool frames_restored = before.free_pages == after.free_pages;
        terminal_write("  FREE AFTER: ");
        terminal_write_u64(after.free_pages);
        terminal_putchar('\n');

        print_test("FRAME COUNT RESTORED", frames_restored);

        bool pass = main_ok && file_ok && process_created && space_ok && caps_ok && endpoint_created &&
            send_cap && receive_cap && image_loaded && image_layout_ok && mappings_ok && initial_data_ok &&
            stack_allocated && stack_mapped && stack_ready && thread_created && prepared && queued &&
            yielded && shell_restored && user_dead && !fault_captured && completion_ok && thread_id_ok &&
            data_ok && bss_ok && null_args_ok && wrong_rights_ok && failure_clear_ok &&
            count_rejection_ok && send_ok && full_rejected && receive_ok && payload_ok && endpoint_idle &&
            thread_reaped && stack_clean && image_clean && send_revoked && receive_revoked && caps_clean &&
            endpoint_clean && process_clean && frames_restored;

        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("USER C RUNTIME TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}