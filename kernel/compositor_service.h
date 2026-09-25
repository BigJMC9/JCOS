#ifndef JA_OS_COMPOSITOR_SERVICE_H
#define JA_OS_COMPOSITOR_SERVICE_H

#include "managed_service.h"
#include "process.h"
#include "types.h"

bool compositor_service_start(void);
bool compositor_service_stop(void);
bool compositor_service_recover(void);
bool compositor_service_cleanup(void);

ManagedServiceState compositor_service_state(void);
bool compositor_service_running(void);
u64 compositor_service_incarnation(void);
u64 compositor_service_process_id(void);
u64 compositor_service_thread_id(void);

bool compositor_service_ping(u64 cookie, u64 *out_cookie);
bool compositor_service_surface_configure(u32 slot, u32 x, u32 y,
    u32 width, u32 height, u32 z);
bool compositor_service_surface_remove(u32 slot);
bool compositor_service_focus_surface(u32 slot);
bool compositor_service_focus_clear(void);
u32 compositor_service_focus_slot(void);
bool compositor_service_compose(u32 *out_count, u32 *out_sample);

bool compositor_service_fault(void);
bool compositor_service_last_exit_info(ProcessExitInfo *out);

#endif