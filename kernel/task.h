#ifndef JA_OS_TASK_H
#define JA_OS_TASK_H

#include "thread.h"
#include "process.h"
#include "interrupts.h"

bool task_terminate_thread(Thread *thread);

/* R6a.1: terminal CPL3 handoff, with no reaping on the active kernel stack.
 * Clean exit preserves live siblings. Fatal fault stops every sibling and
 * aborts retained IPC continuations before publishing process death. Both
 * close process-owned endpoints only when the domain becomes terminal.
 * Fault CR2 is supplied from the entry-time diagnostic snapshot (#PF only).
 * Kernel faults and unsupported/system exceptions remain fatal. */
bool task_user_fault_supported(u64 vector);
InterruptFrame *task_exit_current_from_interrupt(InterruptFrame *frame);
InterruptFrame *task_fault_current_from_interrupt(InterruptFrame *frame, u64 fault_address);

/*
 * Stop and reap every Thread owned by a user
 * Process, then revoke all of its capabilities.
 *
 * This deliberately does NOT destroy the Process
 * AddressSpace or caller-owned user mappings.
 */
bool task_quiesce_process(Process *process);

#endif