#ifndef JA_OS_IPC_H
#define JA_OS_IPC_H

#include "types.h"
#include "process.h"
#include "endpoint.h"
#include "capability.h"
#include "thread.h"

bool ipc_try_send(
    Process *process,
    CapabilityHandle handle,
    const IpcMessage *message
);

bool ipc_try_receive(
    Process *process,
    CapabilityHandle handle,
    IpcMessage *out_message
);

bool ipc_receive_blocking(
    Process *process,
    CapabilityHandle handle,
    IpcMessage *out_message
);

bool ipc_send_blocking(
    Process *process,
    CapabilityHandle handle,
    const IpcMessage *message
);

/* Relative PIT-tick timeout. Zero is rejected; ordinary blocking APIs remain unbounded. */
bool ipc_receive_blocking_for(
    Process *process,
    CapabilityHandle handle,
    IpcMessage *out_message,
    u64 timeout_ticks
);

bool ipc_send_blocking_for(
    Process *process,
    CapabilityHandle handle,
    const IpcMessage *message,
    u64 timeout_ticks
);

/* Called by the monotonic PIT tick path with local interrupts already excluded. */
void ipc_timeout_poll(u64 now_ticks);

bool ipc_endpoint_close(Endpoint *endpoint);
bool ipc_close_owned_endpoints(u64 owner_process_id);
bool ipc_abort_thread_wait(Thread *thread);

#endif