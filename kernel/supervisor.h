#ifndef JA_OS_SUPERVISOR_H
#define JA_OS_SUPERVISOR_H

#include "types.h"
#include "../include/supervisor_protocol.h"

bool supervisor_start(void);
bool supervisor_ping(u64 cookie, u64 *out_cookie);
bool supervisor_stop(void);
bool supervisor_running(void);
bool supervisor_stack_guarded(void);

u64 supervisor_process_id(void);
u64 supervisor_thread_id(void);

#endif