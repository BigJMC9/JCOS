#ifndef JCOS_RUNTIME_BLOCK_TEST_ABI_H
#define JCOS_RUNTIME_BLOCK_TEST_ABI_H

#include "user_abi.h"

#define JCOS_RTB_RESULT_ADDRESS 0x0000008000002000
#define JCOS_RTB_INITIAL_MAGIC 0x4A434F5342524931
#define JCOS_RTB_COMPLETE_MAGIC 0x4A434F5342524F4B

#define JCOS_RTB_PHASE_START       1
#define JCOS_RTB_PHASE_WAIT_RX1    2
#define JCOS_RTB_PHASE_WAIT_RX2    3
#define JCOS_RTB_PHASE_WAIT_SEND   4
#define JCOS_RTB_PHASE_COMPLETE    5

/* ABI probe result bits. */
#define JCOS_RTB_PROBE_RESULT_OK 0x01
#define JCOS_RTB_PROBE_STACK_ALIGNED 0x02
#define JCOS_RTB_PROBE_RBX_PRESERVED 0x04
#define JCOS_RTB_PROBE_RBP_PRESERVED 0x08
#define JCOS_RTB_PROBE_R12_PRESERVED 0x10
#define JCOS_RTB_PROBE_R13_PRESERVED 0x20
#define JCOS_RTB_PROBE_R14_PRESERVED 0x40
#define JCOS_RTB_PROBE_R15_PRESERVED 0x80
#define JCOS_RTB_PROBE_EXPECTED 0xFF

/*
 * Deliberately distinct SysV callee-saved
 * register values.
 *
 * Keep these assembler-safe.
 */
#define JCOS_RTB_SENTINEL_RBX 0x1122334455667788
#define JCOS_RTB_SENTINEL_RBP 0x2233445566778899
#define JCOS_RTB_SENTINEL_R12 0x33445566778899AA
#define JCOS_RTB_SENTINEL_R13 0x445566778899AABB
#define JCOS_RTB_SENTINEL_R14 0x5566778899AABBCC
#define JCOS_RTB_SENTINEL_R15 0x66778899AABBCCDD

/* First blocking receive payload. */
#define JCOS_RTB_RX1_WORD0 0x1011121314151617
#define JCOS_RTB_RX1_WORD1 0x2021222324252627
#define JCOS_RTB_RX1_WORD2 0x3031323334353637
#define JCOS_RTB_RX1_WORD3 0x4041424344454647

/* Second blocking receive payload. */
#define JCOS_RTB_RX2_WORD0 0x5152535455565758
#define JCOS_RTB_RX2_WORD1 0x6162636465666768
#define JCOS_RTB_RX2_WORD2 0x7172737475767778
#define JCOS_RTB_RX2_WORD3 0x8182838485868788

/* Kernel pre-fills the outbound mailbox with this message before the user's blocking SEND. */
#define JCOS_RTB_PREFILL_WORD0 0x9192939495969798
#define JCOS_RTB_PREFILL_WORD1 0xA1A2A3A4A5A6A7A8
#define JCOS_RTB_PREFILL_WORD2 0xB1B2B3B4B5B6B7B8
#define JCOS_RTB_PREFILL_WORD3 0xC1C2C3C4C5C6C7C8

/* Message sent by the Ring3 program after the two blocking receives. */
#define JCOS_RTB_TX_WORD0 0xD1D2D3D4D5D6D7D8
#define JCOS_RTB_TX_WORD1 0xE1E2E3E4E5E6E7E8
#define JCOS_RTB_TX_WORD2 0xF1F2F3F4F5F6F7F8
#define JCOS_RTB_TX_WORD3 0x0123456789ABCDEF

#ifndef __ASSEMBLER__

typedef struct {
    JcosU64 initial_magic;
    JcosU64 completion_magic;
    JcosU64 phase;
    JcosU64 thread_id;
    JcosU64 entry_rsp_mod16;
    JcosU64 null_receive_result;
    JcosU64 null_send_result;
    JcosU64 wrong_right_receive_result;
    JcosU64 wrong_right_receive_word_count;

    JcosU64 wrong_right_receive_words[JCOS_IPC_MESSAGE_MAX_WORDS];

    JcosU64 wrong_right_send_result;
    JcosU64 receive1_probe_mask;
    JcosU64 receive1_word_count;

    JcosU64 receive1_words[JCOS_IPC_MESSAGE_MAX_WORDS];

    JcosU64 receive2_probe_mask;
    JcosU64 receive2_word_count;

    JcosU64 receive2_words[JCOS_IPC_MESSAGE_MAX_WORDS];

    JcosU64 send_probe_mask;
} JcosRuntimeBlockTestResult;

_Static_assert(sizeof(JcosRuntimeBlockTestResult) == 27ULL * sizeof(JcosU64), "JcosRuntimeBlockTestResult ABI mismatch");

#endif
#endif