#include "lib/syscall.h"

#include "../include/runtime_block_test_abi.h"

__attribute__((section(".runtime_block_result"), used, aligned(8)))
static volatile JcosRuntimeBlockTestResult g_result = {.initial_magic = JCOS_RTB_INITIAL_MAGIC, .phase = JCOS_RTB_PHASE_START};

extern JcosU64 jcos_runtime_probe_entry_rsp_mod16(void);
extern JcosU64 jcos_runtime_probe_receive(JcosCapabilityHandle handle, JcosIpcMessage *message);
extern JcosU64 jcos_runtime_probe_send(JcosCapabilityHandle handle, const JcosIpcMessage *message);

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

static void fill_tx_message(JcosIpcMessage *message) {
    if (!message) return;

    message->words[0] = JCOS_RTB_TX_WORD0;
    message->words[1] = JCOS_RTB_TX_WORD1;
    message->words[2] = JCOS_RTB_TX_WORD2;
    message->words[3] = JCOS_RTB_TX_WORD3;
    message->word_count = JCOS_IPC_MESSAGE_MAX_WORDS;
}

static void record_words(volatile JcosU64 *out_words, const JcosIpcMessage *message) {
    if (!out_words || !message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) {
        out_words[i] = message->words[i];
    }
}

__attribute__((noreturn))
void jcos_main(JcosCapabilityHandle receive_cap, JcosCapabilityHandle send_cap) {
    g_result.thread_id = jcos_thread_id();

    /* A normal SysV function entered through a CALL sees RSP % 16 == 8. */
    g_result.entry_rsp_mod16 = jcos_runtime_probe_entry_rsp_mod16();

    JcosIpcMessage message;
    JcosIpcMessage received;

    clear_message(&message);
    poison_message(&received);

    /* Blocking wrappers must reject NULL before attempting to block. */
    g_result.null_receive_result = (JcosU64) jcos_ipc_receive_blocking(receive_cap, 0);
    g_result.null_send_result = (JcosU64) jcos_ipc_send_blocking(send_cap, 0);

    /*
     * SEND-only authority cannot RECEIVE.
     *
     * The receive output must be cleared.
     */
    g_result.wrong_right_receive_result = (JcosU64) jcos_ipc_receive_blocking(send_cap, &received);
    g_result.wrong_right_receive_word_count = received.word_count;
    
    record_words(g_result.wrong_right_receive_words, &received);

    /* RECEIVE-only authority cannot SEND. */
    fill_tx_message(&message);

    g_result.wrong_right_send_result = (JcosU64) jcos_ipc_send_blocking(receive_cap, &message);

    /*
     * RECEIVE #1.
     *
     * Kernel test harness will observe us
     * BLOCKED here, send a four-word message,
     * wake us, and schedule us again.
     */
    poison_message(&received);

    g_result.phase = JCOS_RTB_PHASE_WAIT_RX1;
    g_result.receive1_probe_mask = jcos_runtime_probe_receive(receive_cap, &received);
    g_result.receive1_word_count = received.word_count;

    record_words(g_result.receive1_words, &received);

    /* RECEIVE #2. */
    poison_message(&received);

    g_result.phase = JCOS_RTB_PHASE_WAIT_RX2;
    g_result.receive2_probe_mask = jcos_runtime_probe_receive(receive_cap, &received);
    g_result.receive2_word_count = received.word_count;

    record_words(g_result.receive2_words, &received);

    /*
     * Kernel test harness will pre-fill the
     * outbound endpoint before we reach this
     * call.
     *
     * Therefore this SEND must genuinely block.
     */
    fill_tx_message(&message);

    g_result.phase = JCOS_RTB_PHASE_WAIT_SEND;
    g_result.send_probe_mask = jcos_runtime_probe_send(send_cap, &message);
    g_result.phase = JCOS_RTB_PHASE_COMPLETE;
    g_result.completion_magic = JCOS_RTB_COMPLETE_MAGIC;

    jcos_thread_exit();
}