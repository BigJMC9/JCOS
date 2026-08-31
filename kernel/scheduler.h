#ifndef JA_OS_SCHEDULER_H
#define JA_OS_SCHEDULER_H

#include "types.h"
#include "thread.h"

bool scheduler_init(void);

bool scheduler_add(
    Thread *thread
);

bool scheduler_remove(
    Thread *thread
);

/* Cooperative yield. Interrupts must already be disabled. */
bool scheduler_yield(void);
u64 scheduler_thread_count(void);

/*
 * Permanently terminate the running thread.
 *
 * Interrupts must already be disabled.
 */
NORETURN void scheduler_exit_current(void);

#endif