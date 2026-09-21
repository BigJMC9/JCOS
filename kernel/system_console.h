#ifndef JA_OS_SYSTEM_CONSOLE_H
#define JA_OS_SYSTEM_CONSOLE_H

#include "key_event.h"
#include "managed_service.h"
#include "process.h"
#include "types.h"

/* Persistent R7 bootstrap console policy service. The kernel retains only the
 * privileged byte/output mechanism plus a raw read-only boot-archive mechanism;
 * pathname parsing and ordinary shell/filesystem policy stay in Ring3. */
bool system_console_configure_boot_archive(const void *archive, u64 archive_size);
bool system_console_start(void);
bool system_console_stop(void);
bool system_console_restart(void);

ManagedServiceState system_console_state(void);
bool system_console_running(void);
bool system_console_idle(void);
u64 system_console_incarnation(void);
u64 system_console_process_id(void);
u64 system_console_thread_id(void);

bool system_console_putchar(char c);
bool system_console_write(const char *s);
bool system_console_writeln(const char *s);
bool system_console_write_u64(u64 value);
bool system_console_write_hex(u64 value);

/* R7a.3 normal shell session transport. Input is forwarded as a versioned
 * event, command parsing stays in Ring3, and each event has a bounded reply.
 * out_action/result use JCOS_CONSOLE_SHELL_* values from the shared protocol. */
bool system_console_shell_activate(void);
bool system_console_shell_deactivate(void);
bool system_console_input_event(const KeyEvent *event, u64 *out_action, u64 *out_result);

typedef struct {
    ProgramGrantSpec grant;
    CapabilityHandle authority_handle;
    ProgramGrantSpec input_grant;
    CapabilityHandle input_authority_handle;
    u64 session_id;
    bool active;
} SystemConsoleAppClientGrant;

/* One foreground application I/O session at a time. The console service switches
 * from its private management endpoint to the public output endpoint while the
 * privileged input broker may send only to a separate application-input endpoint.
 * Temporary TRANSFER authorities exist only while launching a client. */
bool system_console_app_session_begin(u64 *out_session_id);
bool system_console_app_client_grant_begin(SystemConsoleAppClientGrant *out);
bool system_console_app_client_grant_end(SystemConsoleAppClientGrant *grant);
bool system_console_app_input_event(u64 session_id, const KeyEvent *event);
bool system_console_app_session_wait_end(u64 session_id);
bool system_console_app_session_abort(u64 session_id);
u64 system_console_app_session_id(void);
u64 system_console_app_endpoint_id(void);
u64 system_console_app_input_endpoint_id(void);

typedef struct {
    u64 service_incarnation;
    u64 data_offset;
    u64 size;
} SystemConsoleLaunchRequest;

/* Ring3 resolves a pathname to an immutable boot-archive extent and sends only
 * that opaque extent over a reincarnating one-way policy channel. */
bool system_console_launch_request_take(SystemConsoleLaunchRequest *out);
bool system_console_archive_extent(u64 offset, u64 size, const u8 **out_data);
u64 system_console_launch_endpoint_id(void);

typedef struct {
    u64 operation;
    u64 policy_incarnation;
    u64 data_offset;
    u64 size;
} SystemConsoleServiceRequest;

/* Ring3 service naming/restart policy sends only an operation plus an optional
 * immutable executable extent. The kernel never receives a service name/path. */
bool system_console_service_request_take(SystemConsoleServiceRequest *out);
bool system_console_service_result(u64 result, u64 state, u64 service_incarnation);
u64 system_console_service_endpoint_id(void);

bool system_console_last_exit_info(ProcessExitInfo *out);
bool system_console_connection_snapshot(ManagedServiceConnection *out);
bool system_console_connection_current(const ManagedServiceConnection *connection);

bool system_console_present(void);
u64 system_console_portal_write_count(void);
u64 system_console_portal_endpoint_id(void);
u64 system_console_portal_thread_id(void);

/* R7c.1 raw boot-archive mechanism diagnostics. These expose object/counter
 * identity for acceptance tests, not pathname or file policy. */
u64 system_console_archive_size(void);
u64 system_console_archive_read_count(void);
u64 system_console_archive_request_endpoint_id(void);
u64 system_console_archive_reply_endpoint_id(void);
u64 system_console_archive_thread_id(void);

/* Acceptance-only deliberate userspace fault. No request is replayed. */
bool system_console_test_fault(void);

#endif