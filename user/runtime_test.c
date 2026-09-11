#include "lib/syscall.h"
#include "../include/runtime_test_abi.h"

__attribute__((section(".runtime_test_result"), used, aligned(8)))
static volatile JcosRuntimeTestResult g_result = {
    .initial_magic = JCOS_RUNTIME_TEST_INITIAL_MAGIC
};

static volatile JcosU64 g_data_probe = JCOS_RUNTIME_TEST_DATA_MAGIC;
static volatile JcosU64 g_bss_probe;

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        message->words[i] = 0;
    }
    message->word_count = 0;
}

static void poison_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        message->words[i] = ~0ULL;
    }
    message->word_count = ~0U;
}

static void fill_payload(JcosIpcMessage *message, JcosU32 count) {
    if (!message) return;

    message->words[0] = JCOS_RUNTIME_TEST_WORD0;
    message->words[1] = JCOS_RUNTIME_TEST_WORD1;
    message->words[2] = JCOS_RUNTIME_TEST_WORD2;
    message->words[3] = JCOS_RUNTIME_TEST_WORD3;
    message->word_count = count;
}

__attribute__((noreturn))
void jcos_main(JcosCapabilityHandle send_cap, JcosCapabilityHandle receive_cap) {
    g_result.thread_id = jcos_thread_id();

    /* Prove normal initialized .data. */
    g_result.data_probe_seen = g_data_probe;

    /* Prove .bss was zero-filled and is writable. */
    g_result.bss_probe_initial = g_bss_probe;
    g_bss_probe = JCOS_RUNTIME_TEST_BSS_MAGIC;
    g_result.bss_probe_after_write = g_bss_probe;

    JcosIpcMessage message;
    JcosIpcMessage received;
    clear_message(&message);

    /* Userspace wrapper argument validation. */
    g_result.null_send_result = (JcosU64)jcos_ipc_try_send(send_cap, 0);
    g_result.null_receive_result = (JcosU64)jcos_ipc_try_receive(receive_cap, 0);

    /* RECEIVE-only capability must not SEND. */
    fill_payload(&message, 1U);

    g_result.wrong_right_send_result = (JcosU64)jcos_ipc_try_send(receive_cap, &message);

    /*
     * SEND-only capability must not RECEIVE.
     *
     * Poison the output first so we prove the
     * wrapper produces defined cleared output
     * after failure.
     */
    poison_message(&received);

    g_result.wrong_right_receive_result = (JcosU64)jcos_ipc_try_receive(send_cap, &received);
    g_result.wrong_right_receive_word_count = received.word_count;

    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        g_result.wrong_right_receive_words[i] = received.words[i];
    }

    /* A valid receive from an empty endpoint must fail and clear the output message. */
    poison_message(&received);

    g_result.empty_receive_result = (JcosU64)jcos_ipc_try_receive(receive_cap, &received);
    g_result.empty_receive_word_count = received.word_count;

    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        g_result.empty_receive_words[i] = received.words[i];
    }

    /* Invalid message counts. */
    fill_payload(&message, JCOS_IPC_MESSAGE_MAX_WORDS + 1U);
    g_result.oversize_send_result = (JcosU64)jcos_ipc_try_send(send_cap, &message);

    fill_payload(&message, 0U);
    g_result.zero_count_send_result = (JcosU64)jcos_ipc_try_send(send_cap, &message);

    /* Full four-word message. */
    fill_payload(&message, JCOS_IPC_MESSAGE_MAX_WORDS);
    g_result.send_result = (JcosU64)jcos_ipc_try_send(send_cap, &message);

    /* Mailbox is occupied. A second nonblocking SEND must fail. */
    g_result.full_send_result = (JcosU64)jcos_ipc_try_send(send_cap, &message);

    /* Consume the four-word message through the shared userspace RECEIVE wrapper. */
    poison_message(&received);

    g_result.receive_result = (JcosU64)jcos_ipc_try_receive(receive_cap, &received);
    g_result.receive_word_count = received.word_count;

    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        g_result.receive_words[i] = received.words[i];
    }

    /* Written last. Kernel test harness can use this to prove the C program reached the end. */
    g_result.completion_magic = JCOS_RUNTIME_TEST_COMPLETE_MAGIC;

    jcos_thread_exit();
}