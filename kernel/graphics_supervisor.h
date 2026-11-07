#ifndef JA_OS_GRAPHICS_SUPERVISOR_H
#define JA_OS_GRAPHICS_SUPERVISOR_H
#include "types.h"
#include "../include/graphics_supervisor_protocol.h"
typedef struct {
    u64 process_id,thread_id,session,sequence;
    bool failed;
} GraphicsSupervisorSession;
/* Zero-initialized storage; identities are never silently reconnected. */
bool graphics_supervisor_begin(GraphicsSupervisorSession *session);
bool graphics_supervisor_request(GraphicsSupervisorSession *session,u32 event,u64 facts,u64 *action);
#endif
