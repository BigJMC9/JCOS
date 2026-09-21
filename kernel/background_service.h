#ifndef JA_OS_BACKGROUND_SERVICE_H
#define JA_OS_BACKGROUND_SERVICE_H

#include "process.h"
#include "types.h"

typedef struct {
    u64 result;
    u64 state;
    u64 incarnation;
} BackgroundServiceResult;

/* R7d generic single-slot ordinary-service mechanism. Service naming, pathname
 * choice and restart decisions are supplied by Ring3 over the service broker. */
bool background_service_handle_pending(BackgroundServiceResult *out);
bool background_service_stop(void);
bool background_service_present(void);
bool background_service_running(void);
u64 background_service_incarnation(void);
u64 background_service_process_id(void);
u64 background_service_thread_id(void);
bool background_service_last_exit_info(ProcessExitInfo *out);

/* Acceptance helper over the common lifecycle protocol. */
bool background_service_ping(u64 cookie, u64 *out_cookie);
bool background_service_image_id(u64 *out_image_id);
bool background_service_protocol_contract(void);

#endif
