#include "user_runtime_test.h"
#include "user_test_fixture.h"

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
    user_fixture_check(name, pass);
}

static bool result_words_zero(const volatile JcosU64 words[JCOS_IPC_MESSAGE_MAX_WORDS]) {
    if (!words) return false;
    for (u32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        if (words[i] != 0ULL) return false;
    }
    return true;
}

static bool result_payload_ok(const volatile JcosRuntimeTestResult *result) {
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
    if (!user_fixture_available()) return;
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

    UserTestFixture *fixture = user_fixture_begin("USER C RUNTIME TEST");
    if (!fixture) return;
    bool behavior_completed = false;

    AddressSpace *space = 0;
    CapabilityTable *caps = 0;
    CapabilityHandle send_handle = CAPABILITY_INVALID_HANDLE;
    CapabilityHandle receive_handle = CAPABILITY_INVALID_HANDLE;
    bool process_created = user_fixture_process_create(fixture);
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
    volatile JcosRuntimeTestResult *result = 0;

    print_test("PROCESS CREATE", process_created);

    if (!process_created) goto cleanup;

    space = process_address_space(&fixture->process);
    caps = process_capabilities(&fixture->process);
    space_ok = space && !space->kernel && address_space_cr3(space);
    caps_ok = caps && capability_table_count(caps) == 0U;

    print_test("ADDRESS SPACE", space_ok);
    print_test("CAP TABLE EMPTY", caps_ok);

    if (!space_ok || !caps_ok) goto cleanup;

    endpoint_created = user_fixture_endpoint_create(fixture, 0U);

    print_test("ENDPOINT CREATE", endpoint_created);

    if (!endpoint_created) goto cleanup;

    send_cap = user_fixture_grant(fixture, false, 0U, CAPABILITY_RIGHT_SEND, &send_handle);

    receive_cap = send_cap &&
        user_fixture_grant(fixture, false, 0U, CAPABILITY_RIGHT_RECEIVE, &receive_handle);

    print_test("SEND CAP", send_cap);
    print_test("RECEIVE CAP", receive_cap);

    if (!receive_cap) goto cleanup;

    image_loaded = user_fixture_load(fixture, file);

    print_test("ELF LOAD", image_loaded);

    if (!image_loaded) goto cleanup;

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    vm_flags_t text_flags = 0;
    vm_flags_t data_flags = 0;
    bool text_mapping = address_space_query_page(space, ADDRESS_SPACE_USER_BASE, &text_frame, &text_flags);
    bool data_mapping = address_space_query_page(space, JCOS_RUNTIME_TEST_RESULT_ADDRESS, &data_frame, &data_flags);

    image_layout_ok = fixture->image.entry == ADDRESS_SPACE_USER_BASE && fixture->image.page_count == 2U;

    mappings_ok = text_mapping && data_mapping && !(text_flags & VM_WRITE) &&
        (text_flags & VM_USER) && (text_flags & VM_EXEC) &&
        (data_flags & VM_WRITE) && (data_flags & VM_USER) && !(data_flags & VM_EXEC);

    print_test("ELF LAYOUT", image_layout_ok);
    print_test("TEXT/DATA MAPPINGS", mappings_ok);

    if (!image_layout_ok || !mappings_ok) goto cleanup;

    result = (volatile JcosRuntimeTestResult *) phys_to_virt(frame_to_phys(data_frame));

    initial_data_ok = result && result->initial_magic == JCOS_RUNTIME_TEST_INITIAL_MAGIC &&
        result->completion_magic == 0ULL;

    print_test("DATA INITIALIZER", initial_data_ok);

    if (!initial_data_ok) goto cleanup;

    stack_allocated = user_fixture_stack_allocate(fixture, 0U, USER_RUNTIME_TEST_STACK);

    print_test("USER STACK FRAME", stack_allocated);

    if (!stack_allocated) goto cleanup;

    stack_mapped = user_fixture_stack_map(fixture, 0U);

    print_test("USER STACK MAP", stack_mapped);

    if (!stack_mapped) goto cleanup;

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(fixture->stacks[0].frame));

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

    thread_created = user_fixture_thread_create(fixture, 0U);
    print_test("USER THREAD CREATE", thread_created);

    if (!thread_created) goto cleanup;

    u64 expected_thread_id = fixture->threads[0].id;
    u64 initial_rsp = USER_RUNTIME_TEST_STACK_TOP - USER_RUNTIME_TEST_STARTUP_SIZE;

    prepared = thread_prepare_user(&fixture->threads[0], fixture->image.entry, initial_rsp);

    print_test("PREPARE USER", prepared);

    if (!prepared) goto cleanup;

    queued = scheduler_add(&fixture->threads[0]);

    print_test("QUEUE USER", queued);

    if (!queued) goto cleanup;

    interrupt_clear_user_fault();

    yielded = user_fixture_schedule_once();

    UserFaultInfo fault;

    k_memset(&fault, 0, sizeof(fault));

    fault_captured = interrupt_last_user_fault(&fault);

    shell_restored = yielded && thread_current() == main_thread &&
        main_thread->state == THREAD_STATE_RUNNING && scheduler_thread_count() == 1ULL;

    user_dead = shell_restored && fixture->threads[0].state == THREAD_STATE_DEAD && !fixture->threads[0].on_run_queue &&
        !fixture->threads[0].interrupt_context_ready && !fixture->threads[0].interrupt_rsp;

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

    endpoint_idle = endpoint_created && !endpoint_message_ready(&fixture->endpoints[0]) &&
        !endpoint_receiver_waiting(&fixture->endpoints[0]) && !endpoint_sender_waiting(&fixture->endpoints[0]);

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

    behavior_completed = main_ok && file_ok && process_created && space_ok && caps_ok && endpoint_created &&
            send_cap && receive_cap && image_loaded && image_layout_ok && mappings_ok && initial_data_ok &&
            stack_allocated && stack_mapped && stack_ready && thread_created && prepared && queued &&
            yielded && shell_restored && user_dead && !fault_captured && completion_ok && thread_id_ok &&
            data_ok && bss_ok && null_args_ok && wrong_rights_ok && failure_clear_ok &&
            count_rejection_ok && send_ok && full_rejected && receive_ok && payload_ok && endpoint_idle;

cleanup: {
        bool pass = user_fixture_finish(fixture, behavior_completed);
        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("USER C RUNTIME TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}
