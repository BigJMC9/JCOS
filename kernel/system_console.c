#include "system_console.h"

#include "console_portal.h"
#include "boot_archive_portal.h"
#include "lib.h"
#include "ipc.h"
#include "scheduler.h"
#include "serial.h"
#include "timer.h"
#include "../include/console_service_protocol.h"
#include "../include/boot_archive_portal_abi.h"
#include "../include/program_broker_protocol.h"
#include "../include/service_broker_protocol.h"

#define SYSTEM_CONSOLE_PATH "/bin/consoleservice.elf"
#define SYSTEM_CONSOLE_PORTAL_TIMEOUT_SECONDS 2ULL

static ManagedService g_console_service;
static ManagedServiceConnection g_console_connection;
static ConsolePortal g_console_portal;
static BootArchivePortal g_boot_archive_portal;
static const u8 *g_boot_archive;
static u64 g_boot_archive_size;
static bool g_boot_archive_configured;
static Endpoint g_app_endpoint;
static Endpoint g_app_input_endpoint;
static Endpoint g_launch_endpoint;
static Endpoint g_service_endpoint;
static CapabilityHandle g_app_receive_grant_handle;
static CapabilityHandle g_app_input_send_handle;
static CapabilityHandle g_launch_receive_handle;
static CapabilityHandle g_launch_send_grant_handle;
static CapabilityHandle g_service_receive_handle;
static CapabilityHandle g_service_send_grant_handle;
static u64 g_app_session_id;
static u64 g_next_app_session_id = 1ULL;
static bool g_app_endpoint_created;
static bool g_app_input_endpoint_created;
static bool g_app_receive_grant_cap;
static bool g_app_input_send_cap;
static bool g_launch_endpoint_created;
static bool g_launch_receive_cap;
static bool g_launch_send_grant_cap;
static bool g_service_endpoint_created;
static bool g_service_receive_cap;
static bool g_service_send_grant_cap;
static bool g_connection_valid;
static bool g_failure_reported;

static void system_console_report_failure(void) {
    if (g_failure_reported) return;
    serial_write("R7 CONSOLE: userspace output service unavailable; ordinary payload not replayed.\n");
    g_failure_reported = true;
}

static u64 system_console_timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * SYSTEM_CONSOLE_PORTAL_TIMEOUT_SECONDS : 0ULL;
}


static bool system_console_launch_endpoint_create(void) {
    if (g_launch_endpoint_created || g_launch_receive_cap || g_launch_send_grant_cap) return false;
    if (!endpoint_create(&g_launch_endpoint)) return false;
    g_launch_endpoint_created = true;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_launch_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_launch_receive_handle)) goto fail;
    g_launch_receive_cap = true;
    if (!capability_insert(caps, &g_launch_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER, &g_launch_send_grant_handle)) goto fail;
    g_launch_send_grant_cap = true;
    return true;

fail:
    if (g_launch_receive_cap && caps) {
        (void)capability_revoke(caps, g_launch_receive_handle);
        g_launch_receive_cap = false;
        g_launch_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_launch_endpoint_created) {
        (void)endpoint_destroy(&g_launch_endpoint);
        g_launch_endpoint_created = false;
    }
    return false;
}

static bool system_console_launch_endpoint_destroy(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (g_launch_endpoint_created && g_launch_receive_cap) {
        while (endpoint_message_ready(&g_launch_endpoint)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!ipc_try_receive(kernel, g_launch_receive_handle, &discard)) return false;
        }
    }
    if (g_launch_endpoint_created && !endpoint_closed(&g_launch_endpoint) &&
        !ipc_endpoint_close(&g_launch_endpoint)) return false;
    if (g_launch_receive_cap) {
        if (!capability_revoke(caps, g_launch_receive_handle)) return false;
        g_launch_receive_cap = false;
        g_launch_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_launch_send_grant_cap) {
        if (!capability_revoke(caps, g_launch_send_grant_handle)) return false;
        g_launch_send_grant_cap = false;
        g_launch_send_grant_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_launch_endpoint_created) {
        if (!endpoint_destroy(&g_launch_endpoint)) return false;
        g_launch_endpoint_created = false;
    }
    return true;
}

static bool system_console_service_endpoint_create(void) {
    if (g_service_endpoint_created || g_service_receive_cap || g_service_send_grant_cap) return false;
    if (!endpoint_create(&g_service_endpoint)) return false;
    g_service_endpoint_created = true;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_service_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_service_receive_handle)) goto fail;
    g_service_receive_cap = true;
    if (!capability_insert(caps, &g_service_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER, &g_service_send_grant_handle)) goto fail;
    g_service_send_grant_cap = true;
    return true;

fail:
    if (g_service_receive_cap && caps) {
        (void)capability_revoke(caps, g_service_receive_handle);
        g_service_receive_cap = false;
        g_service_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_service_endpoint_created) {
        (void)endpoint_destroy(&g_service_endpoint);
        g_service_endpoint_created = false;
    }
    return false;
}

static bool system_console_service_endpoint_destroy(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (g_service_endpoint_created && g_service_receive_cap) {
        while (endpoint_message_ready(&g_service_endpoint)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!ipc_try_receive(kernel, g_service_receive_handle, &discard)) return false;
        }
    }
    if (g_service_endpoint_created && !endpoint_closed(&g_service_endpoint) &&
        !ipc_endpoint_close(&g_service_endpoint)) return false;
    if (g_service_receive_cap) {
        if (!capability_revoke(caps, g_service_receive_handle)) return false;
        g_service_receive_cap = false;
        g_service_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_service_send_grant_cap) {
        if (!capability_revoke(caps, g_service_send_grant_handle)) return false;
        g_service_send_grant_cap = false;
        g_service_send_grant_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (g_service_endpoint_created) {
        if (!endpoint_destroy(&g_service_endpoint)) return false;
        g_service_endpoint_created = false;
    }
    return true;
}

static bool system_console_app_endpoint_create(void) {
    if (g_app_endpoint_created) return false;
    if (!endpoint_create(&g_app_endpoint)) return false;
    g_app_endpoint_created = true;
    return true;
}

static bool system_console_app_input_endpoint_create(void) {
    if (g_app_input_endpoint_created || g_app_input_send_cap) return false;
    if (!endpoint_create(&g_app_input_endpoint)) return false;
    g_app_input_endpoint_created = true;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_app_input_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_app_input_send_handle)) {
        (void)endpoint_destroy(&g_app_input_endpoint);
        g_app_input_endpoint_created = false;
        return false;
    }
    g_app_input_send_cap = true;
    return true;
}

static bool system_console_app_input_endpoint_reset(void) {
    if (!g_app_input_endpoint_created || !g_app_input_send_cap ||
        endpoint_receiver_waiting(&g_app_input_endpoint) ||
        endpoint_sender_waiting(&g_app_input_endpoint)) return false;
    if (endpoint_message_ready(&g_app_input_endpoint)) {
        IpcMessage discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!endpoint_try_receive(&g_app_input_endpoint, &discard)) return false;
    }
    return !endpoint_message_ready(&g_app_input_endpoint);
}

static bool system_console_app_input_endpoint_destroy(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (g_app_input_send_cap) {
        if (!caps || !capability_revoke(caps, g_app_input_send_handle)) return false;
        g_app_input_send_handle = CAPABILITY_INVALID_HANDLE;
        g_app_input_send_cap = false;
    }
    if (!g_app_input_endpoint_created) return true;
    if (!endpoint_closed(&g_app_input_endpoint) && !ipc_endpoint_close(&g_app_input_endpoint)) return false;
    if (!endpoint_destroy(&g_app_input_endpoint)) return false;
    g_app_input_endpoint_created = false;
    return true;
}

static bool system_console_app_receive_grant_begin(void) {
    if (!g_app_endpoint_created || g_app_receive_grant_cap) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_app_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &g_app_receive_grant_handle)) return false;
    g_app_receive_grant_cap = true;
    return true;
}

static bool system_console_app_receive_grant_end(void) {
    if (!g_app_receive_grant_cap) return true;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_revoke(caps, g_app_receive_grant_handle)) return false;
    g_app_receive_grant_handle = CAPABILITY_INVALID_HANDLE;
    g_app_receive_grant_cap = false;
    return true;
}

static bool system_console_app_endpoint_destroy(void) {
    if (!system_console_app_receive_grant_end()) return false;
    if (!g_app_endpoint_created) return true;
    if (!endpoint_closed(&g_app_endpoint) && !ipc_endpoint_close(&g_app_endpoint)) return false;
    if (!endpoint_destroy(&g_app_endpoint)) return false;
    g_app_endpoint_created = false;
    g_app_session_id = 0ULL;
    return true;
}

static bool system_console_app_ready(void) {
    if (!g_app_endpoint_created || !managed_service_running(&g_console_service)) return false;
    Thread *thread = &g_console_service.program.thread;
    return thread->state == THREAD_STATE_BLOCKED && !thread->on_run_queue &&
        thread->interrupt_context_ready && thread->interrupt_rsp &&
        thread_wait_matches(thread, THREAD_WAIT_IPC_RECEIVE, &g_app_endpoint) &&
        endpoint_receiver_waiting(&g_app_endpoint);
}

static bool system_console_wait_app_ready(void) {
    u64 timeout = system_console_timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    for (;;) {
        if (system_console_app_ready()) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = &g_console_service.program.thread;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool system_console_launch(ManagedServiceSpec *spec, ManagedServiceLaunchExtras *extras) {
    if (!spec || !extras || !console_portal_active(&g_console_portal) ||
        !boot_archive_portal_active(&g_boot_archive_portal)) return false;
    k_memset(spec, 0, sizeof(*spec));
    k_memset(extras, 0, sizeof(*extras));
    spec->path = SYSTEM_CONSOLE_PATH;
    spec->shutdown_message = JCOS_CONSOLE_SERVICE_MESSAGE_SHUTDOWN;
    spec->shutdown_reply = JCOS_CONSOLE_SERVICE_REPLY_STOPPED;
    extras->shutdown_request_word_count = 2U;
    extras->shutdown_request_words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SHUTDOWN;
    extras->shutdown_request_words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    extras->startup_grant_count = 6U;
    if (!console_portal_grant_spec(&g_console_portal, &extras->startup_grants[0])) return false;
    if (!g_app_endpoint_created || !g_app_receive_grant_cap) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;
    extras->startup_grants[1].authority_table = caps;
    extras->startup_grants[1].authority_handle = g_app_receive_grant_handle;
    extras->startup_grants[1].object = &g_app_endpoint;
    extras->startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    extras->startup_grants[1].rights = CAPABILITY_RIGHT_RECEIVE;
    if (!boot_archive_portal_grant_specs(&g_boot_archive_portal,
            &extras->startup_grants[2], &extras->startup_grants[3])) return false;
    if (!g_launch_endpoint_created || !g_launch_send_grant_cap) return false;
    extras->startup_grants[4].authority_table = caps;
    extras->startup_grants[4].authority_handle = g_launch_send_grant_handle;
    extras->startup_grants[4].object = &g_launch_endpoint;
    extras->startup_grants[4].type = CAPABILITY_TYPE_ENDPOINT;
    extras->startup_grants[4].rights = CAPABILITY_RIGHT_SEND;
    if (!g_service_endpoint_created || !g_service_send_grant_cap) return false;
    extras->startup_grants[5].authority_table = caps;
    extras->startup_grants[5].authority_handle = g_service_send_grant_handle;
    extras->startup_grants[5].object = &g_service_endpoint;
    extras->startup_grants[5].type = CAPABILITY_TYPE_ENDPOINT;
    extras->startup_grants[5].rights = CAPABILITY_RIGHT_SEND;
    extras->startup_argument_count = 1U;
    extras->startup_arguments[0] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    return true;
}

static bool system_console_connect(void) {
    k_memset(&g_console_connection, 0, sizeof(g_console_connection));
    g_connection_valid = managed_service_connect_kernel(&g_console_service, &g_console_connection);
    return g_connection_valid;
}

static bool system_console_wait_portal_count(u64 expected) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * SYSTEM_CONSOLE_PORTAL_TIMEOUT_SECONDS;
    u64 started = timer_ticks();
    for (;;) {
        u64 count = console_portal_write_count(&g_console_portal);
        if (count > expected || !console_portal_active(&g_console_portal)) return false;
        if (count == expected && console_portal_idle(&g_console_portal)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = &g_console_portal.thread;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool system_console_wait_portal_idle(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * SYSTEM_CONSOLE_PORTAL_TIMEOUT_SECONDS;
    u64 started = timer_ticks();
    for (;;) {
        if (!console_portal_active(&g_console_portal)) return false;
        if (console_portal_idle(&g_console_portal)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = &g_console_portal.thread;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool system_console_wait_archive_idle(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * SYSTEM_CONSOLE_PORTAL_TIMEOUT_SECONDS;
    u64 started = timer_ticks();
    for (;;) {
        if (!boot_archive_portal_active(&g_boot_archive_portal)) return false;
        if (boot_archive_portal_idle(&g_boot_archive_portal)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = &g_boot_archive_portal.thread;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
}

static bool system_console_wait_policy_idle(void) {
    return system_console_wait_portal_idle() && system_console_wait_archive_idle();
}

static bool system_console_map_key(const KeyEvent *event, u64 *out_key, u64 *out_event) {
    if (!event || !out_key || !out_event || !event->pressed) return false;
    u64 key = 0ULL;
    switch (event->key) {
        case KEY_CHARACTER: key = JCOS_CONSOLE_INPUT_KEY_CHARACTER; break;
        case KEY_ENTER: key = JCOS_CONSOLE_INPUT_KEY_ENTER; break;
        case KEY_BACKSPACE: key = JCOS_CONSOLE_INPUT_KEY_BACKSPACE; break;
        case KEY_TAB: key = JCOS_CONSOLE_INPUT_KEY_TAB; break;
        case KEY_ESCAPE: key = JCOS_CONSOLE_INPUT_KEY_ESCAPE; break;
        case KEY_UP: key = JCOS_CONSOLE_INPUT_KEY_UP; break;
        case KEY_DOWN: key = JCOS_CONSOLE_INPUT_KEY_DOWN; break;
        case KEY_LEFT: key = JCOS_CONSOLE_INPUT_KEY_LEFT; break;
        case KEY_RIGHT: key = JCOS_CONSOLE_INPUT_KEY_RIGHT; break;
        case KEY_HOME: key = JCOS_CONSOLE_INPUT_KEY_HOME; break;
        case KEY_END: key = JCOS_CONSOLE_INPUT_KEY_END; break;
        case KEY_DELETE: key = JCOS_CONSOLE_INPUT_KEY_DELETE; break;
        case KEY_PAGE_UP: key = JCOS_CONSOLE_INPUT_KEY_PAGE_UP; break;
        case KEY_PAGE_DOWN: key = JCOS_CONSOLE_INPUT_KEY_PAGE_DOWN; break;
        case KEY_NONE:
        default:
            return false;
    }

    u64 packed = (u64)(u8)event->character;
    if (event->shift) packed |= JCOS_CONSOLE_INPUT_EVENT_SHIFT;
    if (event->ctrl) packed |= JCOS_CONSOLE_INPUT_EVENT_CTRL;
    if (event->alt) packed |= JCOS_CONSOLE_INPUT_EVENT_ALT;
    *out_key = key;
    *out_event = packed;
    return true;
}

static bool system_console_service_release(void) {
    ManagedServiceState state = managed_service_state(&g_console_service);
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_console_service);
        if (stopped != MANAGED_SERVICE_STOP_GRACEFUL && stopped != MANAGED_SERVICE_STOP_FORCED) return false;
    } else if (state != MANAGED_SERVICE_STOPPED && !managed_service_recover(&g_console_service)) {
        return false;
    }
    g_connection_valid = false;
    k_memset(&g_console_connection, 0, sizeof(g_console_connection));
    return true;
}

static bool system_console_service_start(void) {
    ManagedServiceSpec spec;
    ManagedServiceLaunchExtras extras;
    if (!system_console_app_receive_grant_begin()) return false;
    bool launched = system_console_launch(&spec, &extras) &&
        managed_service_start_ex(&g_console_service, &spec, &extras);
    bool grant_released = system_console_app_receive_grant_end();
    if (!launched || !grant_released || !system_console_connect() ||
        !system_console_wait_archive_idle()) {
        (void)managed_service_recover(&g_console_service);
        return false;
    }
    return true;
}

bool system_console_configure_boot_archive(const void *archive, u64 archive_size) {
    if (!archive || !archive_size || boot_archive_portal_present(&g_boot_archive_portal)) return false;
    if (g_boot_archive_configured) {
        return g_boot_archive == (const u8 *)archive && g_boot_archive_size == archive_size;
    }
    g_boot_archive = (const u8 *)archive;
    g_boot_archive_size = archive_size;
    g_boot_archive_configured = true;
    return true;
}

bool system_console_start(void) {
    if (system_console_present() || !g_boot_archive_configured || !g_boot_archive || !g_boot_archive_size) return false;
    if (!console_portal_start(&g_console_portal)) return false;
    if (!boot_archive_portal_start(&g_boot_archive_portal, g_boot_archive, g_boot_archive_size) ||
        !system_console_app_endpoint_create() || !system_console_app_input_endpoint_create() ||
        !system_console_launch_endpoint_create() || !system_console_service_endpoint_create() ||
        !system_console_service_start()) {
        (void)system_console_service_release();
        (void)system_console_app_endpoint_destroy();
        (void)system_console_app_input_endpoint_destroy();
        (void)system_console_launch_endpoint_destroy();
        (void)system_console_service_endpoint_destroy();
        (void)boot_archive_portal_stop(&g_boot_archive_portal);
        (void)console_portal_stop(&g_console_portal);
        return false;
    }

    g_failure_reported = false;
    return true;
}

bool system_console_stop(void) {
    if (g_app_session_id) return false;
    if (!system_console_service_release()) return false;
    if (!system_console_app_endpoint_destroy()) return false;
    if (!system_console_app_input_endpoint_destroy()) return false;
    if (!system_console_launch_endpoint_destroy()) return false;
    if (!system_console_service_endpoint_destroy()) return false;
    if (!boot_archive_portal_stop(&g_boot_archive_portal)) return false;
    return console_portal_stop(&g_console_portal);
}

bool system_console_restart(void) {
    if (!console_portal_active(&g_console_portal) ||
        !boot_archive_portal_active(&g_boot_archive_portal) || g_app_session_id) return false;
    if (!system_console_service_release()) return false;
    if (!system_console_app_endpoint_destroy()) return false;
    if (!system_console_app_input_endpoint_destroy()) return false;
    if (!system_console_launch_endpoint_destroy()) return false;
    if (!system_console_service_endpoint_destroy()) return false;
    if (!system_console_app_endpoint_create() || !system_console_app_input_endpoint_create() ||
        !system_console_launch_endpoint_create() || !system_console_service_endpoint_create() ||
        !system_console_service_start()) {
        (void)system_console_service_release();
        (void)system_console_app_endpoint_destroy();
        (void)system_console_app_input_endpoint_destroy();
        (void)system_console_launch_endpoint_destroy();
        (void)system_console_service_endpoint_destroy();
        return false;
    }
    g_failure_reported = false;
    return true;
}

ManagedServiceState system_console_state(void) {
    return managed_service_state(&g_console_service);
}

bool system_console_running(void) {
    return console_portal_active(&g_console_portal) &&
        boot_archive_portal_active(&g_boot_archive_portal) && g_app_endpoint_created &&
        g_app_input_endpoint_created && g_app_input_send_cap && g_launch_endpoint_created &&
        g_launch_receive_cap && g_launch_send_grant_cap && g_service_endpoint_created &&
        g_service_receive_cap && g_service_send_grant_cap && g_connection_valid &&
        managed_service_running(&g_console_service) &&
        managed_service_connection_current(&g_console_service, &g_console_connection);
}

bool system_console_idle(void) {
    return !g_app_session_id && system_console_running() && managed_service_idle(&g_console_service) &&
        !endpoint_message_ready(&g_launch_endpoint) && !endpoint_sender_waiting(&g_launch_endpoint) &&
        !endpoint_message_ready(&g_service_endpoint) && !endpoint_sender_waiting(&g_service_endpoint) &&
        console_portal_idle(&g_console_portal) && boot_archive_portal_idle(&g_boot_archive_portal);
}

u64 system_console_incarnation(void) {
    return managed_service_incarnation(&g_console_service);
}

u64 system_console_process_id(void) {
    return managed_service_process_id(&g_console_service);
}

u64 system_console_thread_id(void) {
    return managed_service_thread_id(&g_console_service);
}

bool system_console_putchar(char c) {
    if (!system_console_idle()) {
        system_console_report_failure();
        return false;
    }

    u64 before = console_portal_write_count(&g_console_portal);
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 3U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_WRITE_BYTE;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    request.words[2] = (u64)(u8)c;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    if (!managed_service_call(&g_console_service, &g_console_connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_CONSOLE_SERVICE_REPLY_WRITTEN ||
        reply.words[1] != g_console_connection.incarnation ||
        reply.words[2] != JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION ||
        reply.words[3] != (u64)(u8)c ||
        !system_console_wait_portal_count(before + 1ULL)) {
        system_console_report_failure();
        return false;
    }

    g_failure_reported = false;
    return true;
}

bool system_console_write(const char *s) {
    if (!s) return false;
    while (*s) {
        if (!system_console_putchar(*s++)) return false;
    }
    return true;
}

bool system_console_writeln(const char *s) {
    return system_console_write(s) && system_console_putchar('\n');
}

bool system_console_write_u64(u64 value) {
    char buffer[21];
    u32 count = 0U;
    if (!value) return system_console_putchar('0');
    while (value && count < 20U) {
        buffer[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }
    while (count) {
        if (!system_console_putchar(buffer[--count])) return false;
    }
    return true;
}

bool system_console_write_hex(u64 value) {
    static const char digits[] = "0123456789ABCDEF";
    if (!system_console_write("0x")) return false;
    bool started = false;
    for (s32 shift = 60; shift >= 0; shift -= 4) {
        u8 digit = (u8)((value >> (u32)shift) & 0xFULL);
        if (digit || started || !shift) {
            if (!system_console_putchar(digits[digit])) return false;
            started = true;
        }
    }
    return true;
}


bool system_console_shell_activate(void) {
    if (!system_console_idle()) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_ACTIVATE;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    return managed_service_call(&g_console_service, &g_console_connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_SHELL_ACTIVE &&
        reply.words[1] == g_console_connection.incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        system_console_wait_policy_idle();
}

bool system_console_shell_deactivate(void) {
    if (!system_console_idle()) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_DEACTIVATE;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    return managed_service_call(&g_console_service, &g_console_connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_SHELL_INACTIVE &&
        reply.words[1] == g_console_connection.incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        system_console_wait_policy_idle();
}

bool system_console_input_event(const KeyEvent *event, u64 *out_action, u64 *out_result) {
    if (out_action) *out_action = JCOS_CONSOLE_SHELL_ACTION_NONE;
    if (out_result) *out_result = JCOS_CONSOLE_SHELL_RESULT_NONE;
    if (!system_console_idle()) return false;

    u64 key = 0ULL;
    u64 packed = 0ULL;
    if (!system_console_map_key(event, &key, &packed)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 4U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_KEY_EVENT;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    request.words[2] = key;
    request.words[3] = packed;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    if (!managed_service_call(&g_console_service, &g_console_connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_CONSOLE_SERVICE_REPLY_KEY_HANDLED ||
        reply.words[1] != g_console_connection.incarnation ||
        reply.words[2] != JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION ||
        !system_console_wait_policy_idle()) return false;

    if (out_action) *out_action = JCOS_CONSOLE_SHELL_UNPACK_ACTION(reply.words[3]);
    if (out_result) *out_result = JCOS_CONSOLE_SHELL_UNPACK_RESULT(reply.words[3]);
    return true;
}

bool system_console_app_session_begin(u64 *out_session_id) {
    if (!out_session_id) return false;
    *out_session_id = 0ULL;
    if (!system_console_idle() || !g_app_endpoint_created || !g_app_input_endpoint_created ||
        !g_app_input_send_cap || !g_next_app_session_id || !system_console_app_input_endpoint_reset()) return false;

    u64 session_id = g_next_app_session_id++;
    if (!session_id) return false;

    Process *kernel = process_kernel();
    if (!kernel) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 3U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_APP_SESSION_BEGIN;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    request.words[2] = session_id;
    if (!ipc_try_send(kernel, g_console_connection.send_handle, &request)) return false;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    u64 timeout = system_console_timeout_ticks();
    bool replied = timeout && ipc_receive_blocking_for(kernel,
        g_console_connection.receive_handle, &reply, timeout);
    bool valid = replied && reply.word_count == 4U &&
        reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_APP_SESSION_ACTIVE &&
        reply.words[1] == g_console_connection.incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        reply.words[3] == session_id && system_console_wait_app_ready();
    if (!valid) {
        (void)system_console_restart();
        return false;
    }

    g_app_session_id = session_id;
    *out_session_id = session_id;
    return true;
}

bool system_console_app_client_grant_begin(SystemConsoleAppClientGrant *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_app_session_id || !system_console_app_ready() ||
        !g_app_input_endpoint_created || !g_app_input_send_cap) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_app_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &out->authority_handle)) return false;
    if (!capability_insert(caps, &g_app_input_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &out->input_authority_handle)) {
        (void)capability_revoke(caps, out->authority_handle);
        k_memset(out, 0, sizeof(*out));
        return false;
    }

    out->grant.authority_table = caps;
    out->grant.authority_handle = out->authority_handle;
    out->grant.object = &g_app_endpoint;
    out->grant.type = CAPABILITY_TYPE_ENDPOINT;
    out->grant.rights = CAPABILITY_RIGHT_SEND;

    out->input_grant.authority_table = caps;
    out->input_grant.authority_handle = out->input_authority_handle;
    out->input_grant.object = &g_app_input_endpoint;
    out->input_grant.type = CAPABILITY_TYPE_ENDPOINT;
    out->input_grant.rights = CAPABILITY_RIGHT_RECEIVE;

    out->session_id = g_app_session_id;
    out->active = true;
    return true;
}

bool system_console_app_client_grant_end(SystemConsoleAppClientGrant *grant) {
    if (!grant) return false;
    if (!grant->active) return true;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    bool ok = true;
    if (grant->authority_handle != CAPABILITY_INVALID_HANDLE) {
        if (capability_revoke(caps, grant->authority_handle)) {
            grant->authority_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }
    if (grant->input_authority_handle != CAPABILITY_INVALID_HANDLE) {
        if (capability_revoke(caps, grant->input_authority_handle)) {
            grant->input_authority_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }

    if (ok) k_memset(grant, 0, sizeof(*grant));
    return ok;
}

bool system_console_app_input_event(u64 session_id, const KeyEvent *event) {
    if (!session_id || session_id != g_app_session_id || !event || !event->pressed ||
        !g_app_input_endpoint_created || !g_app_input_send_cap) return false;

    u64 key = 0ULL;
    u64 packed = 0ULL;
    if (!system_console_map_key(event, &key, &packed)) return false;

    Process *kernel = process_kernel();
    u64 timeout = system_console_timeout_ticks();
    if (!kernel || !timeout) return false;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = 4U;
    message.words[0] = JCOS_CONSOLE_INPUT_HEADER(
        JCOS_CONSOLE_INPUT_OP_KEY_EVENT, JCOS_CONSOLE_INPUT_PROTOCOL_VERSION);
    message.words[1] = session_id;
    message.words[2] = key;
    message.words[3] = packed;
    return ipc_send_blocking_for(kernel, g_app_input_send_handle, &message, timeout);
}

bool system_console_app_session_wait_end(u64 session_id) {
    if (!session_id || session_id != g_app_session_id) return false;
    u64 timeout = system_console_timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    for (;;) {
        if (managed_service_idle(&g_console_service) && system_console_wait_policy_idle()) {
            g_app_session_id = 0ULL;
            return true;
        }
        if ((u64)(timer_ticks() - started) >= timeout ||
            !managed_service_running(&g_console_service)) return false;

        Thread *thread = &g_console_service.program.thread;
        bool schedulable = false;
        if (thread->state == THREAD_STATE_READY) {
            schedulable = thread->on_run_queue && thread->interrupt_context_ready &&
                thread->interrupt_rsp && !thread_wait_active(thread);
        } else if (thread->state == THREAD_STATE_BLOCKED) {
            /* During an application session the console may legitimately block
             * either waiting for the next application message or back-pressured
             * while sending a byte to the privileged portal. Those are progress
             * states, not service failure; keep scheduling until END returns the
             * service to its ordinary command receive loop or the deadline wins. */
            schedulable = !thread->on_run_queue && thread->interrupt_context_ready &&
                thread->interrupt_rsp && thread_wait_active(thread);
        }
        if (!schedulable || !scheduler_yield()) return false;
    }
}

bool system_console_app_session_abort(u64 session_id) {
    if (!session_id || session_id != g_app_session_id) return false;
    g_app_session_id = 0ULL;
    return system_console_restart();
}

u64 system_console_app_session_id(void) {
    return g_app_session_id;
}

u64 system_console_app_endpoint_id(void) {
    return g_app_endpoint_created ? g_app_endpoint.id : 0ULL;
}

u64 system_console_app_input_endpoint_id(void) {
    return g_app_input_endpoint_created ? g_app_input_endpoint.id : 0ULL;
}

bool system_console_launch_request_take(SystemConsoleLaunchRequest *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!system_console_running() || !g_launch_endpoint_created || !g_launch_receive_cap) return false;
    Process *kernel = process_kernel();
    if (!kernel) return false;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    if (!ipc_try_receive(kernel, g_launch_receive_handle, &message) || message.word_count != 4U) return false;
    u64 operation = JCOS_PROGRAM_BROKER_HEADER_OP(message.words[0]);
    if (JCOS_PROGRAM_BROKER_HEADER_VERSION(message.words[0]) != JCOS_PROGRAM_BROKER_PROTOCOL_VERSION ||
        message.words[1] != system_console_incarnation()) return false;

    if (operation == JCOS_PROGRAM_BROKER_OP_FOREGROUND_MEDIA) {
        u64 executable_offset = JCOS_PROGRAM_BROKER_EXTENT_OFFSET(message.words[2]);
        u64 executable_size = JCOS_PROGRAM_BROKER_EXTENT_SIZE(message.words[2]);
        u64 media_offset = JCOS_PROGRAM_BROKER_EXTENT_OFFSET(message.words[3]);
        u64 media_size = JCOS_PROGRAM_BROKER_EXTENT_SIZE(message.words[3]);
        const u8 *executable = 0;
        const u8 *media = 0;
        if (!executable_size || !media_size ||
            !boot_archive_portal_extent(&g_boot_archive_portal,
                executable_offset, executable_size, &executable) || !executable ||
            !boot_archive_portal_extent(&g_boot_archive_portal,
                media_offset, media_size, &media) || !media) return false;
        out->service_incarnation = message.words[1];
        out->data_offset = executable_offset;
        out->size = executable_size;
        out->media_data_offset = media_offset;
        out->media_size = media_size;
        return true;
    }

    if (operation != JCOS_PROGRAM_BROKER_OP_FOREGROUND_EXTENT || !message.words[3]) return false;

    const u8 *data = 0;
    if (!boot_archive_portal_extent(&g_boot_archive_portal, message.words[2], message.words[3], &data) || !data)
        return false;
    out->service_incarnation = message.words[1];
    out->data_offset = message.words[2];
    out->size = message.words[3];
    return true;
}

bool system_console_archive_extent(u64 offset, u64 size, const u8 **out_data) {
    return boot_archive_portal_extent(&g_boot_archive_portal, offset, size, out_data);
}

u64 system_console_launch_endpoint_id(void) {
    return g_launch_endpoint_created ? g_launch_endpoint.id : 0ULL;
}

bool system_console_service_request_take(SystemConsoleServiceRequest *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!system_console_running() || !g_service_endpoint_created || !g_service_receive_cap) return false;
    Process *kernel = process_kernel();
    if (!kernel) return false;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    if (!ipc_try_receive(kernel, g_service_receive_handle, &message) || message.word_count < 2U) return false;
    u64 op = JCOS_SERVICE_BROKER_HEADER_OP(message.words[0]);
    u64 version = JCOS_SERVICE_BROKER_HEADER_VERSION(message.words[0]);
    if (version != JCOS_SERVICE_BROKER_PROTOCOL_VERSION ||
        message.words[1] != system_console_incarnation()) return false;

    if (op == JCOS_SERVICE_BROKER_OP_START_EXTENT || op == JCOS_SERVICE_BROKER_OP_RESTART_EXTENT) {
        if (message.word_count != 4U || !message.words[2] || !message.words[3]) return false;
        const u8 *data = 0;
        if (!boot_archive_portal_extent(&g_boot_archive_portal, message.words[2], message.words[3], &data) || !data)
            return false;
        out->data_offset = message.words[2];
        out->size = message.words[3];
    } else {
        if (message.word_count != 2U) return false;
    }

    switch (op) {
        case JCOS_SERVICE_BROKER_OP_START_EXTENT:
        case JCOS_SERVICE_BROKER_OP_STOP:
        case JCOS_SERVICE_BROKER_OP_RESTART_EXTENT:
        case JCOS_SERVICE_BROKER_OP_STATUS:
        case JCOS_SERVICE_BROKER_OP_DIAG_FAULT:
            out->operation = op;
            out->policy_incarnation = message.words[1];
            return true;
        default:
            return false;
    }
}

bool system_console_service_result(u64 result, u64 state, u64 service_incarnation) {
    if (!system_console_running() || !managed_service_idle(&g_console_service)) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 4U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SERVICE_RESULT;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    request.words[2] = JCOS_CONSOLE_SERVICE_PACK_SERVICE_RESULT(result, state);
    request.words[3] = service_incarnation;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    return managed_service_call(&g_console_service, &g_console_connection, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_SERVICE_RESULT &&
        reply.words[1] == g_console_connection.incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION && system_console_wait_policy_idle();
}

u64 system_console_service_endpoint_id(void) {
    return g_service_endpoint_created ? g_service_endpoint.id : 0ULL;
}

bool system_console_last_exit_info(ProcessExitInfo *out) {
    return managed_service_last_exit_info(&g_console_service, out);
}

bool system_console_connection_snapshot(ManagedServiceConnection *out) {
    return out && system_console_running() && managed_service_connect_kernel(&g_console_service, out);
}

bool system_console_connection_current(const ManagedServiceConnection *connection) {
    return managed_service_connection_current(&g_console_service, connection);
}

bool system_console_present(void) {
    return console_portal_present(&g_console_portal) || boot_archive_portal_present(&g_boot_archive_portal) ||
        g_app_endpoint_created || g_app_input_endpoint_created || g_launch_endpoint_created ||
        g_service_endpoint_created || managed_service_state(&g_console_service) != MANAGED_SERVICE_STOPPED;
}

u64 system_console_portal_write_count(void) {
    return console_portal_write_count(&g_console_portal);
}

u64 system_console_portal_endpoint_id(void) {
    return g_console_portal.endpoint_created ? g_console_portal.endpoint.id : 0ULL;
}

u64 system_console_portal_thread_id(void) {
    return g_console_portal.thread_created ? g_console_portal.thread.id : 0ULL;
}

u64 system_console_archive_size(void) {
    return boot_archive_portal_archive_size(&g_boot_archive_portal);
}

u64 system_console_archive_read_count(void) {
    return boot_archive_portal_read_count(&g_boot_archive_portal);
}

u64 system_console_archive_request_endpoint_id(void) {
    return g_boot_archive_portal.request_endpoint_created ? g_boot_archive_portal.request_endpoint.id : 0ULL;
}

u64 system_console_archive_reply_endpoint_id(void) {
    return g_boot_archive_portal.reply_endpoint_created ? g_boot_archive_portal.reply_endpoint.id : 0ULL;
}

u64 system_console_archive_thread_id(void) {
    return g_boot_archive_portal.thread_created ? g_boot_archive_portal.thread.id : 0ULL;
}

bool system_console_test_fault(void) {
    if (!system_console_idle()) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_DIAGNOSTIC_FAULT;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    return managed_service_send_oneway(&g_console_service, &g_console_connection, &request);
}
