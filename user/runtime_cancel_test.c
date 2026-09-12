#include "lib/syscall.h"

#include "../include/runtime_cancel_test_abi.h"

__attribute__((section(".runtime_cancel_result"), used, aligned(8)))
static volatile JcosRuntimeCancelTestResult
    g_result = {
    .initial_magic = JCOS_RTC_INITIAL_MAGIC,
    .phase = JCOS_RTC_PHASE_START
};

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

static void fill_send(JcosIpcMessage *message) {
    if (!message) return;

    message->word_count = JCOS_IPC_MESSAGE_MAX_WORDS;
    message->words[0] = JCOS_RTC_SEND_WORD0;
    message->words[1] = JCOS_RTC_SEND_WORD1;
    message->words[2] = JCOS_RTC_SEND_WORD2;
    message->words[3] = JCOS_RTC_SEND_WORD3;
}

static void copy_message(volatile JcosU64 *out_count, volatile JcosU64 *out_words, const JcosIpcMessage *message) {
    if (!out_count || !out_words || !message) {
        return;
    }

    *out_count = message->word_count;

    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        out_words[i] = message->words[i];
    }
}

__attribute__((noreturn))
void jcos_main(JcosCapabilityHandle capability, JcosU64 mode) {
    g_result.mode = mode;
    g_result.thread_id = jcos_thread_id();

    JcosIpcMessage message;

    if (mode == JCOS_RTC_MODE_RECEIVE) {
        poison_message(&message);

        g_result.phase = JCOS_RTC_PHASE_WAITING;
        g_result.blocking_result = (JcosU64) jcos_ipc_receive_blocking(capability, &message);

        copy_message(&g_result.receive_word_count, g_result.receive_words, &message);

        /*
         * Endpoint is now closed.
         *
         * Both nonblocking and blocking retry
         * operations must fail immediately and
         * return cleared output.
         */
        poison_message(&message);

        g_result.retry_try_result = (JcosU64) jcos_ipc_try_receive(capability, &message);

        copy_message(&g_result.retry_try_word_count, g_result.retry_try_words, &message);
        poison_message(&message);

        g_result.retry_block_result = (JcosU64) jcos_ipc_receive_blocking(capability, &message);

        copy_message(&g_result.retry_block_word_count, g_result.retry_block_words, &message);

    } 
    else if (mode == JCOS_RTC_MODE_SEND) {
        clear_message(&message);
        fill_send(&message);

        g_result.phase = JCOS_RTC_PHASE_WAITING;
        g_result.blocking_result = (JcosU64) jcos_ipc_send_blocking(capability, &message);
        g_result.retry_try_result = (JcosU64) jcos_ipc_try_send(capability, &message);
        g_result.retry_block_result = (JcosU64) jcos_ipc_send_blocking(capability, &message);

    } 
    else jcos_thread_exit();

    g_result.phase = JCOS_RTC_PHASE_COMPLETE;
    g_result.completion_magic = JCOS_RTC_COMPLETE_MAGIC;

    jcos_thread_exit();
}