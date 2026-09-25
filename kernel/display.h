#ifndef JA_OS_DISPLAY_H
#define JA_OS_DISPLAY_H

#include "device_resource.h"
#include "framebuffer.h"
#include "process.h"
#include "program.h"

typedef enum {
    DISPLAY_LEASE_KERNEL = 0,
    DISPLAY_LEASE_HANDOFF,
    DISPLAY_LEASE_USER,
    DISPLAY_LEASE_RECLAIM
} DisplayLeaseState;

bool display_init(void);
bool display_available(void);
DeviceResource *display_resource(void);
bool display_present_user(Process *process, CapabilityHandle handle, u64 pixels,
    u32 width, u32 height);
void display_session_reset(void);

/* R8 fixed-mode GOP ownership handoff. The borrowed mapping is kernel-created;
 * no userspace API accepts arbitrary physical addresses. prepare() suppresses
 * kernel framebuffer rendering before a Ring3 owner can become runnable. */
bool display_direct_prepare(u64 user_virtual_base,
    ProgramBorrowedMappingSpec *out_mapping, FramebufferInfo *out_info,
    u64 *out_user_framebuffer);
bool display_direct_commit(Process *process);
bool display_direct_abort(void);
bool display_direct_reclaim(u64 owner_process_id);
DisplayLeaseState display_direct_state(void);
u64 display_direct_owner_process_id(void);

#endif