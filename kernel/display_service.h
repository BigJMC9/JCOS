#ifndef JA_OS_DISPLAY_SERVICE_H
#define JA_OS_DISPLAY_SERVICE_H

#include "display_surface.h"
#include "endpoint.h"
#include "key_event.h"
#include "managed_service.h"
#include "process.h"

typedef struct {
    ProgramGrantSpec command_grant;
    ProgramGrantSpec reply_grant;
    CapabilityHandle command_authority_handle;
    CapabilityHandle reply_authority_handle;
    u64 incarnation;
    bool active;
} DisplayServiceClientGrant;

typedef struct {
    Endpoint input_endpoint;
    ProgramGrantSpec input_grant;
    CapabilityHandle kernel_send_handle;
    CapabilityHandle receive_authority_handle;
    DisplaySurface *surface;
    u64 client_id;
    u64 process_id;
    u32 slot;
    bool endpoint_created;
    bool kernel_send_cap;
    bool receive_authority_cap;
    bool active;
} DisplayServiceInputClient;

bool display_service_start(void);
bool display_service_stop(void);
bool display_service_recover(void);
bool display_service_cleanup(void);

ManagedServiceState display_service_state(void);
bool display_service_running(void);
u64 display_service_incarnation(void);
u64 display_service_process_id(void);
u64 display_service_thread_id(void);

bool display_service_ping(u64 cookie, u64 *out_cookie);
bool display_service_redraw(void);
bool display_service_fault(void);
bool display_service_last_exit_info(ProcessExitInfo *out);

/* Slot zero preserves the R8C.1 one-surface API. R8C.2 extends the same
 * mechanism to a bounded array of independently mapped read-only surfaces. */
bool display_service_surface_attach(DisplaySurface *surface);
bool display_service_surface_detach(void);
bool display_service_surface_attached(void);
bool display_service_surface_mapping_valid(void);

bool display_service_surface_attach_slot(DisplaySurface *surface, u32 slot);
bool display_service_surface_detach_slot(u32 slot);
bool display_service_surface_slot_attached(u32 slot);
bool display_service_surface_slot_mapping_valid(u32 slot);
u32 display_service_surface_count(void);

/* Userspace compositor policy. Geometry and z-order are interpreted by the
 * Ring3 service; the kernel only maps validated surface resources and carries
 * bounded control messages. */
bool display_service_surface_configure(u32 slot, u32 x, u32 y,
    u32 width, u32 height, u32 z);
bool display_service_compose(u32 *out_count, u32 *out_sample);
bool display_service_surface_remove(u32 slot);
bool display_service_focus_surface(u32 slot);
bool display_service_focus_clear(void);
u32 display_service_focus_slot(void);

/* R8C.3 per-client input portal. Client storage must remain at a stable
 * address from begin() through end() because the embedded Endpoint is a
 * registered kernel object. The application receives RECEIVE only; the kernel
 * retains SEND and routes events only to the Ring3 compositor's focused slot. */
bool display_service_input_client_begin(DisplayServiceInputClient *client,
    DisplaySurface *surface, u32 slot);
bool display_service_input_client_bind(DisplayServiceInputClient *client, Process *process);
bool display_service_input_client_end(DisplayServiceInputClient *client);
u32 display_service_input_client_count(void);
u64 display_service_input_client_id(const DisplayServiceInputClient *client);
u64 display_service_input_client_process_id(const DisplayServiceInputClient *client);
bool display_service_input_event(const KeyEvent *event);

bool display_service_client_grant_begin(DisplayServiceClientGrant *out);
bool display_service_client_grant_end(DisplayServiceClientGrant *grant);

#endif