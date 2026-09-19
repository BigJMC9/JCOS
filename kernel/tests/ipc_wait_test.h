#ifndef JA_OS_IPC_WAIT_TEST_H
#define JA_OS_IPC_WAIT_TEST_H

#include "ipc.h"

/* Kernel-only, one-shot event delivery after publication and before the final
 * park predicate check. This is synchronous fault/interleaving instrumentation,
 * not a new interrupt source or userspace completion interface. The diagnostic
 * must retain the target objects until the hook is consumed or reset. */
typedef enum {
    IPC_WAIT_TEST_NONE = 0,
    IPC_WAIT_TEST_SEND,
    IPC_WAIT_TEST_RECEIVE,
    IPC_WAIT_TEST_CLOSE
} IpcWaitTestAction;

typedef struct {
    bool armed;
    bool fired;
    bool succeeded;
    u64 wait_id;
    ThreadWaitResult result;
    ThreadState state;
} IpcWaitTestObservation;

bool ipc_wait_test_arm_before_park(Thread *thread, Endpoint *endpoint, Process *peer,
    CapabilityHandle peer_handle, IpcWaitTestAction action, const IpcMessage *message);
void ipc_wait_test_reset(void);
IpcWaitTestObservation ipc_wait_test_observation(void);
u64 ipc_wait_test_park_attempts(void);

#endif
