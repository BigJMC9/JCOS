#ifndef JA_OS_DISPLAY_H
#define JA_OS_DISPLAY_H

#include "device_resource.h"
#include "process.h"
#include "program.h"

bool display_init(void);
bool display_available(void);
DeviceResource *display_resource(void);
bool display_present_user(Process *process, CapabilityHandle handle, u64 pixels,
    u32 width, u32 height);
void display_session_reset(void);

#endif
