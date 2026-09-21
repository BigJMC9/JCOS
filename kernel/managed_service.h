#ifndef JA_OS_MANAGED_SERVICE_H
#define JA_OS_MANAGED_SERVICE_H

#include "capability.h"
#include "endpoint.h"
#include "process_exit_queue.h"
#include "program.h"
#include "types.h"

typedef enum {
    MANAGED_SERVICE_STOPPED = 0,
    MANAGED_SERVICE_STARTING,
    MANAGED_SERVICE_RUNNING,
    MANAGED_SERVICE_CLOSING,
    MANAGED_SERVICE_FAILED,
    MANAGED_SERVICE_REAP_PENDING
} ManagedServiceState;

typedef enum {
    MANAGED_SERVICE_STOP_NONE = 0,
    MANAGED_SERVICE_STOP_GRACEFUL,
    MANAGED_SERVICE_STOP_FORCED,
    MANAGED_SERVICE_STOP_FAILED
} ManagedServiceStopResult;

#define MANAGED_SERVICE_BASE_STARTUP_CAPABILITIES 2U
#define MANAGED_SERVICE_BASE_STARTUP_ARGUMENTS    1U
#define MANAGED_SERVICE_MAX_EXTRA_CAPABILITIES \
    (JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES - MANAGED_SERVICE_BASE_STARTUP_CAPABILITIES)
#define MANAGED_SERVICE_MAX_EXTRA_ARGUMENTS \
    (JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS - MANAGED_SERVICE_BASE_STARTUP_ARGUMENTS)

typedef struct {
    const char *path;
    u64 shutdown_message;
    u64 shutdown_reply;
} ManagedServiceSpec;

/* Optional R7+ protocol-specific launch envelope. R6 callers continue using
 * ManagedServiceSpec alone and retain the original two-cap/one-argument ABI. */
typedef struct {
    ProgramGrantSpec startup_grants[MANAGED_SERVICE_MAX_EXTRA_CAPABILITIES];
    u32 startup_grant_count;
    u64 startup_arguments[MANAGED_SERVICE_MAX_EXTRA_ARGUMENTS];
    u32 startup_argument_count;
    u32 shutdown_request_word_count;
    u64 shutdown_request_words[IPC_MESSAGE_MAX_WORDS];
} ManagedServiceLaunchExtras;

/* Kernel-client connection snapshot. Handles belong to the kernel capability
 * table and are valid only for this exact service incarnation. A replacement
 * requires an explicit reconnect; old handles are revoked, never retargeted. */
typedef struct {
    u64 incarnation;
    CapabilityHandle send_handle;
    CapabilityHandle receive_handle;
} ManagedServiceConnection;

typedef struct {
    ProgramInstance program;
    ProcessExitQueue exit_queue;
    Endpoint command_endpoint;
    Endpoint reply_endpoint;

    CapabilityHandle kernel_send_handle;
    CapabilityHandle kernel_receive_handle;
    CapabilityHandle kernel_command_grant_handle;
    CapabilityHandle kernel_reply_grant_handle;

    ProcessExitInfo last_exit;
    u64 incarnation;
    u64 shutdown_message;
    u64 shutdown_reply;
    u32 shutdown_request_word_count;
    u64 shutdown_request_words[IPC_MESSAGE_MAX_WORDS];

    bool exit_queue_created;
    bool command_created;
    bool reply_created;
    bool kernel_send_cap;
    bool kernel_receive_cap;
    bool kernel_command_grant_cap;
    bool kernel_reply_grant_cap;
    bool last_exit_valid;

    ManagedServiceState state;
    ManagedServiceStopResult last_stop_result;
} ManagedService;

/* Standard managed-service startup profile:
 *   cap[0] = RECEIVE command endpoint
 *   cap[1] = SEND reply endpoint
 *   cap[2..] = optional protocol-specific attenuated grants
 *   arg[0] = boot-unique nonzero service incarnation
 *   arg[1..] = optional protocol-specific opaque arguments
 * Every reply must carry that incarnation in word 1. */
bool managed_service_start(ManagedService *service, const ManagedServiceSpec *spec);
bool managed_service_start_ex(ManagedService *service, const ManagedServiceSpec *spec,
    const ManagedServiceLaunchExtras *extras);
/* R7d generic mechanism entry: launch from a validated immutable file view
 * without interpreting a pathname. The file storage only needs to remain valid
 * for the synchronous launch transaction; program_launch copies mapped bytes. */
bool managed_service_start_file_ex(ManagedService *service, const VfsNode *file,
    u64 shutdown_message, u64 shutdown_reply, const ManagedServiceLaunchExtras *extras);
ManagedServiceStopResult managed_service_stop_bounded(ManagedService *service);
bool managed_service_recover(ManagedService *service);
bool managed_service_restart(ManagedService *service, const ManagedServiceSpec *spec);
bool managed_service_restart_ex(ManagedService *service, const ManagedServiceSpec *spec,
    const ManagedServiceLaunchExtras *extras);

ManagedServiceState managed_service_state(ManagedService *service);
bool managed_service_running(ManagedService *service);
bool managed_service_idle(ManagedService *service);
u64 managed_service_incarnation(const ManagedService *service);
u64 managed_service_process_id(ManagedService *service);
u64 managed_service_thread_id(ManagedService *service);

bool managed_service_connect_kernel(ManagedService *service, ManagedServiceConnection *out);
bool managed_service_connection_current(ManagedService *service, const ManagedServiceConnection *connection);

/* call() is bounded and never retries/replays. A valid reply must contain the
 * connection incarnation in word 1 and the service must return to its command
 * receive loop before success is reported. */
bool managed_service_call(ManagedService *service, const ManagedServiceConnection *connection,
    const IpcMessage *request, IpcMessage *reply);

/* One-way delivery for protocols whose next transition is fault/hang/exit.
 * Success means accepted by IPC, not that application work completed. */
bool managed_service_send_oneway(ManagedService *service, const ManagedServiceConnection *connection,
    const IpcMessage *request);

bool managed_service_last_exit_info(const ManagedService *service, ProcessExitInfo *out);
ManagedServiceStopResult managed_service_last_stop_result(const ManagedService *service);

#endif
