#ifndef JA_OS_AUDIO_H
#define JA_OS_AUDIO_H

#include "device_resource.h"
#include "process.h"

bool audio_init(void);
bool audio_available(void);
u32 audio_sample_rate(void);
u32 audio_submit_max(void);
DeviceResource *audio_resource(void);
bool audio_write_user(Process *process, CapabilityHandle handle, u64 samples, u32 byte_count);
void audio_session_reset(void);

#endif
