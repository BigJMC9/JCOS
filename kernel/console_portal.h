#ifndef JA_OS_CONSOLE_PORTAL_H
#define JA_OS_CONSOLE_PORTAL_H

#include "capability.h"
#include "endpoint.h"
#include "program.h"
#include "thread.h"
#include "types.h"

/*
 * Minimal privileged console mechanism.
 *
 * The portal owns one endpoint and one kernel consumer thread. Userspace gets
 * only SEND authority to that endpoint. The consumer accepts bounded byte-write
 * messages plus minimal cursor/scrollback mechanism requests required by the
 * Ring3 line editor. History, selection, clipboard, command parsing, formatting,
 * protocol versioning and retry policy stay in userspace.
 */
typedef struct {
    Endpoint endpoint;
    Thread thread;
    CapabilityHandle kernel_receive_handle;
    CapabilityHandle kernel_transfer_handle;
    u64 write_count;
    u64 last_byte;
    bool endpoint_created;
    bool thread_created;
    bool kernel_receive_cap;
    bool kernel_transfer_cap;
    bool active;
} ConsolePortal;

bool console_portal_start(ConsolePortal *portal);
bool console_portal_stop(ConsolePortal *portal);
bool console_portal_grant_spec(const ConsolePortal *portal, ProgramGrantSpec *out);

bool console_portal_active(const ConsolePortal *portal);
bool console_portal_idle(const ConsolePortal *portal);
bool console_portal_present(const ConsolePortal *portal);
u64 console_portal_write_count(const ConsolePortal *portal);
u64 console_portal_last_byte(const ConsolePortal *portal);

#endif