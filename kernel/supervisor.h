#ifndef JA_OS_SUPERVISOR_H
#define JA_OS_SUPERVISOR_H

#include "process.h"
#include "types.h"
#include "../include/supervisor_protocol.h"

typedef enum {
    SUPERVISOR_STATE_STOPPED = 0,
    SUPERVISOR_STATE_STARTING,
    SUPERVISOR_STATE_RUNNING,
    SUPERVISOR_STATE_CLOSING,
    SUPERVISOR_STATE_FAILED,
    SUPERVISOR_STATE_REAP_PENDING
} SupervisorLifecycleState;

typedef enum {
    SUPERVISOR_STOP_NONE = 0,
    SUPERVISOR_STOP_GRACEFUL,
    SUPERVISOR_STOP_FORCED,
    SUPERVISOR_STOP_FAILED
} SupervisorStopResult;

bool supervisor_start(void);
bool supervisor_ping(u64 cookie, u64 *out_cookie);

/* Bounded stop. A non-cooperating service is force-terminated and reaped.
 * The compatibility bool reports whether the service reached fully STOPPED. */
SupervisorStopResult supervisor_stop_bounded(void);
bool supervisor_stop(void);

/* Recover an already failed/partially reaped incarnation without starting a
 * replacement. restart() performs recovery when required and launches a fresh
 * incarnation through the common R5 path. */
bool supervisor_recover(void);
bool supervisor_restart(void);

SupervisorLifecycleState supervisor_state(void);
SupervisorStopResult supervisor_last_stop_result(void);
bool supervisor_last_exit_info(ProcessExitInfo *out);

bool supervisor_running(void);
bool supervisor_idle(void);
bool supervisor_stack_guarded(void);

u64 supervisor_process_id(void);
u64 supervisor_thread_id(void);

/* Kernel-only diagnostic hooks used by the R6 recovery acceptance test. The
 * supervisor command capability is not exposed to arbitrary user processes. */
bool supervisor_test_arm_shutdown_hang(void);
bool supervisor_test_fault(void);

#endif
