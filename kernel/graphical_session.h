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
/* Read-only recovery admission: all GUI/service/surface ledgers released.
 * Does not stop/reap anything; independent workers must never race cleanup. */
bool graphical_session_drained(void);
bool graphical_session_last_event(KeyEvent *out);
/* Acceptance-only deliberate Ring3 client #UD. The kernel retains the only
 * SEND authority for this diagnostic operation. Resources remain owned until
 * graphical_session_cleanup() proves the recovery path. */
bool graphical_session_test_fault_client(ProcessExitInfo *out);
/* Acceptance-only compositor #UD. The display service and client stay alive,
 * proving window policy can be recovered without surrendering the output lease. */
bool graphical_session_test_fault_compositor(ProcessExitInfo *out);
/* Acceptance-only display-service #UD. The separate compositor and client
 * remain contained while the output lease is retained for supervised teardown. */
bool graphical_session_test_fault_display_service(ProcessExitInfo *out);
u64 graphical_session_run_count(void);
u64 graphical_session_event_count(void);
bool graphical_session_run(void);
/* Explicit first native launch;refusal never starts a firmware GUI implicitly. */
bool graphical_session_native(const BootInfo *boot);
bool graphical_session_restart_driver(void);
/* Consume only a proven drained,lost native GUI generation. No state replay. */
u64 graphical_session_take_reconnect(void);
bool graphical_session_reconnect(void);

#endif