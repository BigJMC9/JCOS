#ifndef JCOS_USER_SYSCALL_H
#define JCOS_USER_SYSCALL_H

#include "../../include/user_abi.h"

JcosU64 jcos_thread_id(void);
int jcos_ipc_try_send(JcosCapabilityHandle handle, const JcosIpcMessage *message);
int jcos_ipc_try_receive(JcosCapabilityHandle handle, JcosIpcMessage *message);

int jcos_ipc_send_blocking(JcosCapabilityHandle handle, const JcosIpcMessage *message);
int jcos_ipc_receive_blocking(JcosCapabilityHandle handle, JcosIpcMessage *message);

JcosU64 jcos_clock_ticks(void);
JcosU64 jcos_clock_frequency(void);
int jcos_display_info(JcosCapabilityHandle handle, JcosU32 *width, JcosU32 *height);
int jcos_display_present(JcosCapabilityHandle handle, const void *rgb332,
    JcosU32 width, JcosU32 height);
int jcos_audio_info(JcosCapabilityHandle handle, JcosU32 *sample_rate,
    JcosU32 *submit_max);
int jcos_audio_write(JcosCapabilityHandle handle, const void *pcm_s16_stereo,
    JcosU32 byte_count);

__attribute__((noreturn))
void jcos_thread_exit(void);

#endif
