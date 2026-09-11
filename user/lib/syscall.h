#ifndef JCOS_USER_SYSCALL_H
#define JCOS_USER_SYSCALL_H

#include "../../include/user_abi.h"

JcosU64 jcos_thread_id(void);
int jcos_ipc_try_send(JcosCapabilityHandle handle, const JcosIpcMessage *message);
int jcos_ipc_try_receive(JcosCapabilityHandle handle, JcosIpcMessage *message);

int jcos_ipc_send_blocking(JcosCapabilityHandle handle, const JcosIpcMessage *message);
int jcos_ipc_receive_blocking(JcosCapabilityHandle handle, JcosIpcMessage *message);

__attribute__((noreturn))
void jcos_thread_exit(void);

#endif