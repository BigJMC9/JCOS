#ifndef JA_OS_TASK_H
#define JA_OS_TASK_H

#include "thread.h"
#include "process.h"

bool task_terminate_thread(Thread *thread);

/*
 * Stop and reap every Thread owned by a user
 * Process, then revoke all of its capabilities.
 *
 * This deliberately does NOT destroy the Process
 * AddressSpace or caller-owned user mappings.
 */
bool task_quiesce_process(Process *process);

#endif