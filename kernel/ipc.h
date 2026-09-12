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

bool ipc_endpoint_close(Endpoint *endpoint);
bool ipc_abort_thread_wait(Thread *thread);

#endif