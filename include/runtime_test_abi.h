#ifndef JCOS_RUNTIME_TEST_ABI_H
#define JCOS_RUNTIME_TEST_ABI_H

#include "user_abi.h"

#define JCOS_RUNTIME_TEST_RESULT_ADDRESS 0x0000008000001000ULL
#define JCOS_RUNTIME_TEST_INITIAL_MAGIC  0x4A434F5352544931ULL
#define JCOS_RUNTIME_TEST_COMPLETE_MAGIC 0x4A434F5352544F4BULL
#define JCOS_RUNTIME_TEST_DATA_MAGIC     0x4A434F5344415441ULL
#define JCOS_RUNTIME_TEST_BSS_MAGIC      0x4A434F5342535357ULL

#define JCOS_RUNTIME_TEST_WORD0 0x1122334455667788ULL
#define JCOS_RUNTIME_TEST_WORD1 0x8877665544332211ULL
#define JCOS_RUNTIME_TEST_WORD2 0xAABBCCDDEEFF0011ULL
#define JCOS_RUNTIME_TEST_WORD3 0x4A434F5352545734ULL

typedef struct {
    JcosU64 initial_magic;
    JcosU64 completion_magic;

    JcosU64 thread_id;

    JcosU64 data_probe_seen;
    JcosU64 bss_probe_initial;
    JcosU64 bss_probe_after_write;

    JcosU64 null_send_result;
    JcosU64 null_receive_result;

    JcosU64 wrong_right_send_result;

    JcosU64 wrong_right_receive_result;
    JcosU64 wrong_right_receive_word_count;
    JcosU64 wrong_right_receive_words[JCOS_IPC_MESSAGE_MAX_WORDS];

    JcosU64 empty_receive_result;
    JcosU64 empty_receive_word_count;
    JcosU64 empty_receive_words[JCOS_IPC_MESSAGE_MAX_WORDS];

    JcosU64 oversize_send_result;
    JcosU64 zero_count_send_result;

    JcosU64 send_result;
    JcosU64 full_send_result;

    JcosU64 receive_result;
    JcosU64 receive_word_count;
    JcosU64 receive_words[JCOS_IPC_MESSAGE_MAX_WORDS];
} JcosRuntimeTestResult;


_Static_assert(sizeof(JcosRuntimeTestResult) == 31ULL * sizeof(JcosU64), "JcosRuntimeTestResult ABI mismatch");

#endif