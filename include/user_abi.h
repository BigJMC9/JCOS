#ifndef JCOS_USER_ABI_H
#define JCOS_USER_ABI_H

/*
 * Shared kernel/userspace syscall ABI.
 *
 * Keep these numeric definitions assembler-safe.
 */

#define JCOS_SYSCALL_THREAD_ID             0
#define JCOS_SYSCALL_IPC_TRY_SEND          1
#define JCOS_SYSCALL_IPC_TRY_RECEIVE       2
#define JCOS_SYSCALL_THREAD_EXIT           3
#define JCOS_SYSCALL_IPC_RECEIVE_BLOCKING  4
#define JCOS_SYSCALL_IPC_SEND_BLOCKING     5

#define JCOS_SYSCALL_RESULT_FAILED         0
#define JCOS_SYSCALL_RESULT_OK             1

#define JCOS_IPC_MESSAGE_MAX_WORDS         4
#define JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET 32


#ifndef __ASSEMBLER__

typedef unsigned long long JcosU64;
typedef unsigned int JcosU32;

typedef JcosU64 JcosCapabilityHandle;

#define JCOS_CAPABILITY_INVALID_HANDLE \
    ((JcosCapabilityHandle)0ULL)

#define JCOS_SYSCALL_RESULT_INVALID \
    ((JcosU64)~0ULL)


typedef struct {
    JcosU64 words[
        JCOS_IPC_MESSAGE_MAX_WORDS
    ];

    JcosU32 word_count;
} JcosIpcMessage;


_Static_assert(
    sizeof(JcosU64) == 8,
    "JcosU64 must be 64-bit"
);

_Static_assert(
    __builtin_offsetof(
        JcosIpcMessage,
        word_count
    ) ==
        JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET,
    "JcosIpcMessage ABI mismatch"
);

#endif

#endif