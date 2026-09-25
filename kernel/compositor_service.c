#include "compositor_service.h"

#include "arch.h"
#include "display_service.h"
#include "lib.h"
#include "scheduler.h"
#include "timer.h"
#include "../include/compositor_service_protocol.h"
#include "../include/display_service_protocol.h"

#define COMPOSITOR_SERVICE_PATH "/bin/compositorservice.elf"
#define COMPOSITOR_SERVICE_WAIT_SECONDS 2ULL

static ManagedService g_compositor_service;
static DisplayServiceClientGrant g_display_grant;
static u32 g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;

static u64 service_header(u64 op) {
    return JCOS_COMPOSITOR_SERVICE_HEADER(op, JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION);
}

static bool wait_idle(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * COMPOSITOR_SERVICE_WAIT_SECONDS;
    u64 started = timer_ticks();

    for (;;) {
        ManagedServiceState state = managed_service_state(&g_compositor_service);
        if (state != MANAGED_SERVICE_RUNNING) return false;
        if (managed_service_idle(&g_compositor_service)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

static bool connect(ManagedServiceConnection *connection) {
    return connection && wait_idle() &&
        managed_service_connect_kernel(&g_compositor_service, connection);
}

static bool wait_failed(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * COMPOSITOR_SERVICE_WAIT_SECONDS;
    u64 started = timer_ticks();

    for (;;) {
        ManagedServiceState state = managed_service_state(&g_compositor_service);
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if (state != MANAGED_SERVICE_RUNNING ||
            (u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

static bool release_display_grant(void) {
    return !g_display_grant.active || display_service_client_grant_end(&g_display_grant);
}

bool compositor_service_start(void) {
    if (!display_service_running() ||
        managed_service_state(&g_compositor_service) != MANAGED_SERVICE_STOPPED ||
        g_focused_slot != JCOS_DISPLAY_SURFACE_FOCUS_NONE || g_display_grant.active)
        return false;

    if (!display_service_client_grant_begin(&g_display_grant)) return false;

    ManagedServiceSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.path = COMPOSITOR_SERVICE_PATH;
    spec.shutdown_message = service_header(JCOS_COMPOSITOR_SERVICE_OP_SHUTDOWN);
    spec.shutdown_reply = JCOS_COMPOSITOR_SERVICE_REPLY_STOPPED;

    ManagedServiceLaunchExtras extras;
    k_memset(&extras, 0, sizeof(extras));
    extras.startup_grant_count = 2U;
    k_memcpy(&extras.startup_grants[0], &g_display_grant.command_grant,
        sizeof(extras.startup_grants[0]));
    k_memcpy(&extras.startup_grants[1], &g_display_grant.reply_grant,
        sizeof(extras.startup_grants[1]));
    extras.startup_argument_count = 1U;
    extras.startup_arguments[0] = g_display_grant.incarnation;
    extras.shutdown_request_word_count = 2U;
    extras.shutdown_request_words[0] = spec.shutdown_message;
    extras.shutdown_request_words[1] = 0ULL;

    bool started = managed_service_start_ex(&g_compositor_service, &spec, &extras);
    bool released = release_display_grant();
    if (started && released) return true;

    if (started) (void)managed_service_stop_bounded(&g_compositor_service);
    (void)release_display_grant();
    g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
    return false;
}

bool compositor_service_stop(void) {
    ManagedServiceState state = managed_service_state(&g_compositor_service);
    if (state == MANAGED_SERVICE_STOPPED) {
        g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
        return release_display_grant();
    }
    if (state != MANAGED_SERVICE_RUNNING) return false;

    ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_compositor_service);
    if (stopped != MANAGED_SERVICE_STOP_GRACEFUL && stopped != MANAGED_SERVICE_STOP_FORCED)
        return false;
    g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
    return release_display_grant();
}

bool compositor_service_recover(void) {
    ManagedServiceState state = managed_service_state(&g_compositor_service);
    if (state == MANAGED_SERVICE_RUNNING) return false;
    if (state != MANAGED_SERVICE_STOPPED && !managed_service_recover(&g_compositor_service))
        return false;
    g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
    return release_display_grant();
}

bool compositor_service_cleanup(void) {
    return compositor_service_running() ? compositor_service_stop() : compositor_service_recover();
}

ManagedServiceState compositor_service_state(void) {
    return managed_service_state(&g_compositor_service);
}

bool compositor_service_running(void) {
    return managed_service_running(&g_compositor_service);
}

u64 compositor_service_incarnation(void) {
    return managed_service_incarnation(&g_compositor_service);
}

u64 compositor_service_process_id(void) {
    return managed_service_process_id(&g_compositor_service);
}

u64 compositor_service_thread_id(void) {
    return managed_service_thread_id(&g_compositor_service);
}

bool compositor_service_ping(u64 cookie, u64 *out_cookie) {
    if (!out_cookie) return false;
    *out_cookie = 0ULL;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 3U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_PING);
    request.words[1] = 0ULL;
    request.words[2] = cookie;

    if (!managed_service_call(&g_compositor_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_COMPOSITOR_SERVICE_REPLY_PONG ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION ||
        reply.words[3] != cookie) return false;
    *out_cookie = reply.words[3];
    return true;
}

bool compositor_service_surface_configure(u32 slot, u32 x, u32 y,
    u32 width, u32 height, u32 z) {
    if (slot >= JCOS_DISPLAY_SURFACE_CAPACITY || !width || !height) return false;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 4U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_SURFACE_CONFIG);
    request.words[1] = JCOS_DISPLAY_SURFACE_CONFIG(slot, z);
    request.words[2] = JCOS_DISPLAY_SURFACE_POSITION(x, y);
    request.words[3] = JCOS_DISPLAY_SURFACE_EXTENT(width, height);

    return managed_service_call(&g_compositor_service, &connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_COMPOSITOR_SERVICE_REPLY_CONFIGURED &&
        reply.words[1] == connection.incarnation &&
        reply.words[2] == JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION && reply.words[3] == slot;
}

bool compositor_service_surface_remove(u32 slot) {
    if (slot >= JCOS_DISPLAY_SURFACE_CAPACITY) return false;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 3U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_SURFACE_REMOVE);
    request.words[1] = 0ULL;
    request.words[2] = slot;

    bool removed = managed_service_call(&g_compositor_service, &connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_COMPOSITOR_SERVICE_REPLY_REMOVED &&
        reply.words[1] == connection.incarnation &&
        reply.words[2] == JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION && reply.words[3] == slot;
    if (removed && g_focused_slot == slot) g_focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
    return removed;
}

static bool focus_request(u32 slot) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 3U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_FOCUS_SURFACE);
    request.words[1] = 0ULL;
    request.words[2] = slot;

    if (!managed_service_call(&g_compositor_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_COMPOSITOR_SERVICE_REPLY_FOCUSED ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION || reply.words[3] != slot)
        return false;
    g_focused_slot = slot;
    return true;
}

bool compositor_service_focus_surface(u32 slot) {
    return slot < JCOS_DISPLAY_SURFACE_CAPACITY && focus_request(slot);
}

bool compositor_service_focus_clear(void) {
    if (!compositor_service_running()) return g_focused_slot == JCOS_DISPLAY_SURFACE_FOCUS_NONE;
    if (g_focused_slot == JCOS_DISPLAY_SURFACE_FOCUS_NONE) return true;
    return focus_request(JCOS_DISPLAY_SURFACE_FOCUS_NONE);
}

u32 compositor_service_focus_slot(void) {
    return compositor_service_running() ? g_focused_slot : JCOS_DISPLAY_SURFACE_FOCUS_NONE;
}

bool compositor_service_compose(u32 *out_count, u32 *out_sample) {
    if (!out_count || !out_sample) return false;
    *out_count = 0U;
    *out_sample = 0U;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 2U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_COMPOSE);
    request.words[1] = 0ULL;

    if (!managed_service_call(&g_compositor_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_COMPOSITOR_SERVICE_REPLY_COMPOSED ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION) return false;

    *out_count = JCOS_DISPLAY_COMPOSE_COUNT(reply.words[3]);
    *out_sample = JCOS_DISPLAY_COMPOSE_SAMPLE(reply.words[3]);
    return true;
}

bool compositor_service_fault(void) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = service_header(JCOS_COMPOSITOR_SERVICE_OP_DIAG_FAULT);
    request.words[1] = 0ULL;
    return managed_service_send_oneway(&g_compositor_service, &connection, &request) && wait_failed();
}

bool compositor_service_last_exit_info(ProcessExitInfo *out) {
    return managed_service_last_exit_info(&g_compositor_service, out);
}