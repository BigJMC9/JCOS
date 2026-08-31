#ifndef JA_OS_SYSCALL_H
#define JA_OS_SYSCALL_H

#include "types.h"
#include "interrupts.h"

#define SYSCALL_VECTOR 0x80U

#define SYSCALL_THREAD_ID 0ULL

#define SYSCALL_RESULT_INVALID (~0ULL)

void syscall_dispatch(
    InterruptFrame *frame
);

#endif