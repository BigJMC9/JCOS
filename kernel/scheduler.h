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
 * Block the current thread when another runnable context exists.
 *
 * Returns only after scheduler_wake() makes
 * this thread runnable and its saved interrupt
 * frame is scheduled again.
 */
bool scheduler_block_current(void);

/*
 * Predicate-loop parking primitive.
 *
 * With another runnable thread this performs a normal scheduler block. If the
 * caller is the sole runnable thread it preserves the continuation in place,
 * idles with STI;HLT for one interrupt, and returns so the caller can re-check
 * its blocking predicate. The caller's original IF state is restored exactly.
 */
bool scheduler_wait_current(void);
u64 scheduler_idle_wait_count(void);

bool scheduler_idle_context_ready(void);
bool scheduler_idle_active(void);
bool scheduler_idle_stack_guarded(void);
u64 scheduler_idle_entry_count(void);
u64 scheduler_idle_resume_count(void);

bool scheduler_wake(Thread *thread);

/*
 * Normal single-CPU runtime policy is enabled once the PIT is established,
 * before boot enables hardware interrupts. These controls remain available to
 * the diagnostic harness so deterministic tests can temporarily quiesce timer
 * switching and then restore the previous policy state.
 *
 * Enable/disable must be called with interrupts disabled.
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

bool scheduler_can_terminate_thread(const Thread *thread);
bool scheduler_terminate_thread(Thread *thread);
bool scheduler_can_terminate_current(void);

/*
 * Permanently terminate the running thread.
 *
 * Uses the kernel-only INT 0x81 scheduling
 * trap and does not return on success.
 */
NORETURN void scheduler_exit_current(void);

#endif