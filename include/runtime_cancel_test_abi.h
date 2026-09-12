#ifndef JCOS_RUNTIME_CANCEL_TEST_ABI_H
#define JCOS_RUNTIME_CANCEL_TEST_ABI_H

#include "user_abi.h"

#define JCOS_RTC_RESULT_ADDRESS 0x0000008000002000ULL
#define JCOS_RTC_INITIAL_MAGIC 0x4A434F5343544931ULL
#define JCOS_RTC_COMPLETE_MAGIC 0x4A434F5343544F4BULL

#define JCOS_RTC_MODE_RECEIVE 1ULL
#define JCOS_RTC_MODE_SEND    2ULL

#define JCOS_RTC_PHASE_START    1ULL
#define JCOS_RTC_PHASE_WAITING  2ULL
#define JCOS_RTC_PHASE_COMPLETE 3ULL

#define JCOS_RTC_PREFILL_WORD0 0x1111222233334444ULL
#define JCOS_RTC_PREFILL_WORD1 0x5555666677778888ULL
#define JCOS_RTC_PREFILL_WORD2 0x9999AAAABBBBCCCCULL
#define JCOS_RTC_PREFILL_WORD3 0xDDDDEEEEFFFF0001ULL
#define JCOS_RTC_SEND_WORD0 0x1020304050607080ULL
#define JCOS_RTC_SEND_WORD1 0x1121314151617181ULL
#define JCOS_RTC_SEND_WORD2 0x1222324252627282ULL
#define JCOS_RTC_SEND_WORD3 0x1323334353637383ULL

typedef struct {
    JcosU64 initial_magic;
    JcosU64 completion_magic;
    JcosU64 phase;
    JcosU64 mode;
    JcosU64 thread_id;
    JcosU64 blocking_result;
    JcosU64 retry_try_result;
    JcosU64 retry_block_result;
    JcosU64 receive_word_count;

    JcosU64 receive_words[JCOS_IPC_MESSAGE_MAX_WORDS];
    JcosU64 retry_try_word_count;
    JcosU64 retry_try_words[JCOS_IPC_MESSAGE_MAX_WORDS];
    JcosU64 retry_block_word_count;
    JcosU64 retry_block_words[JCOS_IPC_MESSAGE_MAX_WORDS];
} JcosRuntimeCancelTestResult;

_Static_assert(sizeof(JcosRuntimeCancelTestResult) == 23ULL * sizeof(JcosU64), "JcosRuntimeCancelTestResult ABI mismatch");

#endif