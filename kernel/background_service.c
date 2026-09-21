#include "background_service.h"

#include "arch.h"
#include "lib.h"
#include "managed_service.h"
#include "scheduler.h"
#include "system_console.h"
#include "timer.h"
#include "vfs.h"
#include "../include/ordinary_service_protocol.h"
#include "../include/service_broker_protocol.h"
#include "../include/service_protocol.h"

#define BACKGROUND_SERVICE_WAIT_SECONDS 2ULL

static ManagedService g_background_service;

static u64 common_header(u64 op) {
    return JCOS_ORDINARY_SERVICE_HEADER(op, JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION);
}

static u64 state_code(void) {
    ManagedServiceState state = managed_service_state(&g_background_service);
    if (state == MANAGED_SERVICE_RUNNING) return JCOS_SERVICE_BROKER_STATE_RUNNING;
    if (state == MANAGED_SERVICE_STOPPED) return JCOS_SERVICE_BROKER_STATE_STOPPED;
    return JCOS_SERVICE_BROKER_STATE_FAILED;
}

static bool release_slot(void) {
    ManagedServiceState state = managed_service_state(&g_background_service);
    if (state == MANAGED_SERVICE_STOPPED) return true;
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_background_service);
        return stopped == MANAGED_SERVICE_STOP_GRACEFUL || stopped == MANAGED_SERVICE_STOP_FORCED;
    }
    return managed_service_recover(&g_background_service);
}

static bool start_extent(u64 offset, u64 size) {
    const u8 *bytes = 0;
    if (!offset || !size || !system_console_archive_extent(offset, size, &bytes) || !bytes) return false;

    VfsNode file;
    k_memset(&file, 0, sizeof(file));
    file.type = VFS_FILE;
    file.data = bytes;
    file.size = size;

    ManagedServiceLaunchExtras extras;
    k_memset(&extras, 0, sizeof(extras));
    extras.shutdown_request_word_count = 2U;
    extras.shutdown_request_words[0] = common_header(JCOS_ORDINARY_SERVICE_OP_SHUTDOWN);
    extras.shutdown_request_words[1] = 0ULL;

    return managed_service_start_file_ex(&g_background_service, &file,
        common_header(JCOS_ORDINARY_SERVICE_OP_SHUTDOWN),
        JCOS_ORDINARY_SERVICE_REPLY_STOPPED, &extras);
}

static bool wait_failed(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * BACKGROUND_SERVICE_WAIT_SECONDS;
    u64 started = timer_ticks();

    for (;;) {
        ManagedServiceState state = managed_service_state(&g_background_service);
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if (state != MANAGED_SERVICE_RUNNING || (u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

static bool diagnostic_fault(void) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!managed_service_idle(&g_background_service) ||
        !managed_service_connect_kernel(&g_background_service, &connection)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = common_header(JCOS_ORDINARY_SERVICE_OP_DIAG_FAULT);
    request.words[1] = 0ULL;
    return managed_service_send_oneway(&g_background_service, &connection, &request) && wait_failed();
}

bool background_service_handle_pending(BackgroundServiceResult *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));

    SystemConsoleServiceRequest request;
    k_memset(&request, 0, sizeof(request));
    if (!system_console_service_request_take(&request)) return false;

    bool ok = false;
    switch (request.operation) {
        case JCOS_SERVICE_BROKER_OP_START_EXTENT:
            if (managed_service_state(&g_background_service) == MANAGED_SERVICE_RUNNING) {
                out->result = JCOS_SERVICE_BROKER_RESULT_ALREADY_RUNNING;
                ok = true;
            } else {
                if (managed_service_state(&g_background_service) != MANAGED_SERVICE_STOPPED && !release_slot()) break;
                ok = start_extent(request.data_offset, request.size);
                out->result = ok ? JCOS_SERVICE_BROKER_RESULT_OK : JCOS_SERVICE_BROKER_RESULT_FAILED;
            }
            break;

        case JCOS_SERVICE_BROKER_OP_STOP:
            if (managed_service_state(&g_background_service) == MANAGED_SERVICE_STOPPED) {
                out->result = JCOS_SERVICE_BROKER_RESULT_NOT_RUNNING;
                ok = true;
            } else {
                ok = release_slot();
                out->result = ok ? JCOS_SERVICE_BROKER_RESULT_OK : JCOS_SERVICE_BROKER_RESULT_FAILED;
            }
            break;

        case JCOS_SERVICE_BROKER_OP_RESTART_EXTENT:
            ok = release_slot() && start_extent(request.data_offset, request.size);
            out->result = ok ? JCOS_SERVICE_BROKER_RESULT_OK : JCOS_SERVICE_BROKER_RESULT_FAILED;
            break;

        case JCOS_SERVICE_BROKER_OP_STATUS:
            out->result = managed_service_state(&g_background_service) == MANAGED_SERVICE_STOPPED ?
                JCOS_SERVICE_BROKER_RESULT_NOT_RUNNING : JCOS_SERVICE_BROKER_RESULT_OK;
            ok = true;
            break;

        case JCOS_SERVICE_BROKER_OP_DIAG_FAULT:
            if (managed_service_state(&g_background_service) != MANAGED_SERVICE_RUNNING) {
                out->result = JCOS_SERVICE_BROKER_RESULT_NOT_RUNNING;
                ok = true;
            } else {
                ok = diagnostic_fault();
                out->result = ok ? JCOS_SERVICE_BROKER_RESULT_OK : JCOS_SERVICE_BROKER_RESULT_FAILED;
            }
            break;

        default:
            out->result = JCOS_SERVICE_BROKER_RESULT_FAILED;
            break;
    }

    out->state = state_code();
    out->incarnation = managed_service_incarnation(&g_background_service);
    return ok;
}

bool background_service_stop(void) {
    return release_slot();
}

bool background_service_present(void) {
    return managed_service_state(&g_background_service) != MANAGED_SERVICE_STOPPED;
}

bool background_service_running(void) {
    return managed_service_running(&g_background_service);
}

u64 background_service_incarnation(void) {
    return managed_service_incarnation(&g_background_service);
}

u64 background_service_process_id(void) {
    return managed_service_process_id(&g_background_service);
}

u64 background_service_thread_id(void) {
    return managed_service_thread_id(&g_background_service);
}

bool background_service_last_exit_info(ProcessExitInfo *out) {
    return managed_service_last_exit_info(&g_background_service, out);
}

bool background_service_ping(u64 cookie, u64 *out_cookie) {
    if (!out_cookie) return false;
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!managed_service_idle(&g_background_service) ||
        !managed_service_connect_kernel(&g_background_service, &connection)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 3U;
    request.words[0] = common_header(JCOS_ORDINARY_SERVICE_OP_PING);
    request.words[1] = 0ULL;
    request.words[2] = cookie;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    if (!managed_service_call(&g_background_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_ORDINARY_SERVICE_REPLY_PONG ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION || reply.words[3] != cookie) return false;
    *out_cookie = reply.words[3];
    return true;
}

bool background_service_image_id(u64 *out_image_id) {
    if (!out_image_id) return false;
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!managed_service_idle(&g_background_service) ||
        !managed_service_connect_kernel(&g_background_service, &connection)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = common_header(JCOS_ORDINARY_SERVICE_OP_IDENTIFY);

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    if (!managed_service_call(&g_background_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_ORDINARY_SERVICE_REPLY_IDENTITY ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION ||
        (reply.words[3] != JCOS_ORDINARY_SERVICE_IMAGE_PRIMARY &&
         reply.words[3] != JCOS_ORDINARY_SERVICE_IMAGE_REPLACEMENT)) return false;
    *out_image_id = reply.words[3];
    return true;
}

bool background_service_protocol_contract(void) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!managed_service_idle(&g_background_service) ||
        !managed_service_connect_kernel(&g_background_service, &connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    u64 incompatible = JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION + 1ULL;
    request.word_count = 2U;
    request.words[0] = JCOS_ORDINARY_SERVICE_HEADER(
        JCOS_ORDINARY_SERVICE_OP_PING, incompatible);
    if (!managed_service_call(&g_background_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION ||
        reply.words[3] != incompatible) return false;

    const u64 unknown = 0x7FFEULL;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 2U;
    request.words[0] = common_header(unknown);
    return managed_service_call(&g_background_service, &connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_SERVICE_REPLY_UNKNOWN_OPERATION &&
        reply.words[1] == connection.incarnation &&
        reply.words[2] == JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION && reply.words[3] == unknown;
}
