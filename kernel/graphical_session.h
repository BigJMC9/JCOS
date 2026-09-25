#ifndef JA_OS_GRAPHICAL_SESSION_H
#define JA_OS_GRAPHICAL_SESSION_H

#include "key_event.h"
#include "process.h"
#include "types.h"

/* R8C.5 one-client interactive graphical session. start/handle/stop are also
 * used by acceptance tests; run() owns the physical input loop until Escape. */
bool graphical_session_start(void);
bool graphical_session_handle_event(const KeyEvent *event);
bool graphical_session_stop(void);
bool graphical_session_cleanup(void);
bool graphical_session_active(void);
bool graphical_session_last_event(KeyEvent *out);
/* Acceptance-only deliberate Ring3 client #UD. The kernel retains the only
 * SEND authority for this diagnostic operation. Resources remain owned until
 * graphical_session_cleanup() proves the recovery path. */
bool graphical_session_test_fault_client(ProcessExitInfo *out);
/* Acceptance-only deliberate Ring3 compositor/display-service #UD. The
 * client must remain contained and the display lease stays retained until
 * graphical_session_cleanup() performs the recovery transaction. */
bool graphical_session_test_fault_display_service(ProcessExitInfo *out);
u64 graphical_session_run_count(void);
u64 graphical_session_event_count(void);
bool graphical_session_run(void);

#endif