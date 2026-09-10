#ifndef JA_OS_SCHEDULER_H
#define JA_OS_SCHEDULER_H

#include "types.h"
#include "thread.h"
#include "interrupts.h"

bool scheduler_init(void);

bool scheduler_add(
    Thread *thread
);

bool scheduler_remove(
    Thread *thread
);

/*
 * Voluntary full-frame reschedule.
 *
 * Uses the kernel-only INT 0x81 scheduler trap.
 */
bool scheduler_yield(void);
u64 scheduler_thread_count(void);

/*
 * Block the current thread.
 *
 * Returns only after scheduler_wake() makes
 * this thread runnable and its saved interrupt
 * frame is scheduled again.
 */
bool scheduler_block_current(void);
bool scheduler_wake(Thread *thread);

/*
 * Enable/disable must be called with interrupts
 * disabled.
 */
bool scheduler_preemption_enable(void);
bool scheduler_preemption_disable(void);

bool scheduler_preemption_enabled(void);
u64 scheduler_preemption_count(void);

InterruptFrame *scheduler_reschedule(InterruptFrame *frame);
u64 scheduler_reschedule_count(void);
InterruptFrame *scheduler_preempt(InterruptFrame *frame);

/*
 * Terminate the current thread while already
 * executing from an interrupt/exception frame.
 *
 * The returned frame belongs to the next
 * runnable thread.
 */
InterruptFrame *scheduler_terminate_current_from_interrupt(InterruptFrame *frame);

/*
 * Permanently terminate the running thread.
 *
 * Uses the kernel-only INT 0x81 scheduling
 * trap and does not return on success.
 */
NORETURN void scheduler_exit_current(void);

#endif