#include "graphical_session.h"

#include "arch.h"
#include "compositor_service.h"
#include "display.h"
#include "display_service.h"
#include "display_surface.h"
#include "framebuffer.h"
#include "input.h"
#include "input_router.h"
#include "ipc.h"
#include "lib.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "serial.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"
#include "../include/display_service_protocol.h"

#define GRAPHICAL_SESSION_SLOT 0U
#define GRAPHICAL_SESSION_TIMEOUT_SECONDS 2ULL

static DisplaySurface g_surface;
static DisplayServiceInputClient g_client;
static ProgramInstance g_app;
static ProcessExitQueue g_exit_queue;
static bool g_exit_queue_created;
static bool g_active;
static u64 g_writer_pid;
static u64 g_run_count;
static u64 g_event_count;

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * GRAPHICAL_SESSION_TIMEOUT_SECONDS : 0ULL;
}

static bool app_blocked(void) {
    Thread *thread = &g_app.thread;
    return g_app.thread_created && thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue && thread->interrupt_context_ready &&
        thread->interrupt_rsp && thread_wait_active(thread);
}

static bool wait_app_blocked(void) {
    u64 timeout = timeout_ticks();
    if (!timeout || !g_app.thread_created) return false;
    u64 started = timer_ticks();
    Thread *thread = &g_app.thread;

    while (!app_blocked()) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (thread->state == THREAD_STATE_DEAD) return false;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue ||
            !thread->interrupt_context_ready || !thread->interrupt_rsp) return false;
        /* A RECEIVE wake may be READY while its reservation is still live.
         * The resumed continuation consumes it before blocking again. */
        if (!scheduler_yield()) return false;
    }
    return true;
}

static bool wait_app_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout || !g_app.thread_created) return false;
    u64 started = timer_ticks();
    Thread *thread = &g_app.thread;

    while (thread->state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (thread->state == THREAD_STATE_READY) {
            /* SEND may wake a blocking RECEIVE while its reservation remains
             * live. Resume the continuation instead of treating that as stale. */
            if (!thread->on_run_queue || !thread->interrupt_context_ready ||
                !thread->interrupt_rsp) return false;
        } else if (thread->state == THREAD_STATE_BLOCKED) {
            if (thread->on_run_queue || !thread->interrupt_context_ready ||
                !thread->interrupt_rsp || !thread_wait_active(thread)) return false;
        } else {
            return false;
        }
        if (!scheduler_yield()) return false;
    }

    return !thread->on_run_queue && !thread->interrupt_context_ready &&
        !thread->interrupt_rsp && !thread_wait_active(thread);
}

static bool drain_app_exit(ProcessExitInfo *out) {
    if (!out || !g_exit_queue_created ||
        process_exit_queue_pending_count(&g_exit_queue) != 1U) return false;
    k_memset(out, 0, sizeof(*out));
    return process_exit_queue_try_receive(&g_exit_queue, out) &&
        process_exit_queue_pending_count(&g_exit_queue) == 0U &&
        process_exit_queue_watch_count(&g_exit_queue) == 0U;
}

static bool release_app(void) {
    if (!program_instance_needs_cleanup(&g_app)) return true;
    if (g_app.thread_created && g_app.thread.state == THREAD_STATE_DEAD)
        return program_reap(&g_app);
    return program_terminate(&g_app);
}

static bool release_exit_queue(void) {
    if (!g_exit_queue_created) return true;
    while (process_exit_queue_pending_count(&g_exit_queue)) {
        ProcessExitInfo discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!process_exit_queue_try_receive(&g_exit_queue, &discard)) return false;
    }
    if (process_exit_queue_watch_count(&g_exit_queue)) return false;
    if (!g_exit_queue.closed && !process_exit_queue_close(&g_exit_queue)) return false;
    if (!process_exit_queue_destroy(&g_exit_queue)) return false;
    g_exit_queue_created = false;
    k_memset(&g_exit_queue, 0, sizeof(g_exit_queue));
    return true;
}

static bool release_writer(void) {
    if (!g_writer_pid) return true;
    if (!display_surface_valid(&g_surface)) return false;
    if (display_surface_writer_process_id(&g_surface) != g_writer_pid) return false;
    if (!display_surface_writer_release(&g_surface, g_writer_pid)) return false;
    g_writer_pid = 0ULL;
    return true;
}

bool graphical_session_cleanup(void) {
    bool retained = g_active || g_exit_queue_created || g_writer_pid ||
        program_instance_needs_cleanup(&g_app) || g_client.active ||
        g_client.endpoint_created || display_surface_valid(&g_surface) ||
        compositor_service_state() != MANAGED_SERVICE_STOPPED ||
        display_service_state() != MANAGED_SERVICE_STOPPED;
    if (!retained) return true;

    bool ok = true;

    if (compositor_service_running() &&
        display_service_focus_slot() != JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        !display_service_focus_clear()) ok = false;

    if (!release_app()) ok = false;
    if (ok && !release_writer()) ok = false;
    if (ok && !release_exit_queue()) ok = false;

    if (g_client.active || g_client.endpoint_created) {
        if (!display_service_input_client_end(&g_client)) ok = false;
    }

    if (!compositor_service_cleanup()) ok = false;

    if (display_service_surface_slot_attached(GRAPHICAL_SESSION_SLOT) && !display_service_surface_detach_slot(GRAPHICAL_SESSION_SLOT)) ok = false;

    if (!display_service_cleanup()) ok = false;

    if (display_surface_valid(&g_surface) && !display_surface_destroy(&g_surface)) ok = false;

    if (ok) {
        g_active = false;
        g_writer_pid = 0ULL;
        k_memset(&g_client, 0, sizeof(g_client));
        k_memset(&g_app, 0, sizeof(g_app));
        terminal_redraw();
    }
    return ok;
}

static bool child_authority_exact(void) {
    if (!g_app.process_created || !g_app.published ||
        g_app.startup_handles[0] == CAPABILITY_INVALID_HANDLE) return false;
    CapabilityTable *caps = process_capabilities(&g_app.process);
    if (!caps || capability_table_count(caps) != 1U) return false;
    void *object = 0;
    if (!capability_lookup_rights(caps, g_app.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &object) || object != &g_client.input_endpoint) return false;
    object = 0;
    return !capability_lookup_rights(caps, g_app.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &object);
}

bool graphical_session_start(void) {
    const char *failure = 0;

    if (g_active) {
        serial_write("R8 GUI START FAILED: session already active.\n");
        return false;
    }
    if (!graphical_session_cleanup()) {
        serial_write("R8 GUI START FAILED: initial cleanup.\n");
        return false;
    }

    FramebufferInfo info;
    k_memset(&info, 0, sizeof(info));
    if (!framebuffer_info(&info)) {
        serial_write("R8 GUI START FAILED: framebuffer descriptor unavailable.\n");
        return false;
    }
    if (!info.direct_map_safe) {
        serial_write("R8 GUI START FAILED: framebuffer direct mapping not safe.\n");
        return false;
    }
    if (!info.width || !info.height) {
        serial_write("R8 GUI START FAILED: invalid framebuffer geometry.\n");
        return false;
    }
    if (compositor_service_state() != MANAGED_SERVICE_STOPPED) {
        serial_write("R8 GUI START FAILED: compositor service not STOPPED.\n");
        return false;
    }
    if (display_service_state() != MANAGED_SERVICE_STOPPED) {
        serial_write("R8 GUI START FAILED: display service not STOPPED.\n");
        return false;
    }
    if (display_service_running()) {
        serial_write("R8 GUI START FAILED: display service already running.\n");
        return false;
    }
    if (display_service_surface_count()) {
        serial_write("R8 GUI START FAILED: stale compositor surface slots.\n");
        return false;
    }
    if (display_service_input_client_count()) {
        serial_write("R8 GUI START FAILED: stale display input clients.\n");
        return false;
    }
    if (display_service_focus_slot() != JCOS_DISPLAY_SURFACE_FOCUS_NONE) {
        serial_write("R8 GUI START FAILED: stale compositor focus.\n");
        return false;
    }
    if (display_direct_state() != DISPLAY_LEASE_KERNEL || display_direct_owner_process_id()) {
        serial_write("R8 GUI START FAILED: framebuffer lease not kernel-owned.\n");
        return false;
    }

    VfsNode *file = vfs_resolve(vfs_root(), JCOS_DISPLAY_SURFACE_APP_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) {
        serial_write("R8 GUI START FAILED: /bin/surfaceapp.elf unavailable.\n");
        return false;
    }

    if (!display_surface_create(&g_surface)) {
        failure = "surface create";
        goto fail;
    }
    if (!display_service_start()) {
        failure = "display service start";
        goto fail;
    }
    if (!display_service_surface_attach_slot(&g_surface, GRAPHICAL_SESSION_SLOT)) {
        failure = "surface attach slot 0";
        goto fail;
    }
    if (!compositor_service_start()) {
        failure = "compositor service start";
        goto fail;
    }
    if (!display_service_input_client_begin(&g_client, &g_surface, GRAPHICAL_SESSION_SLOT)) {
        failure = "input client begin";
        goto fail;
    }
    if (!process_exit_queue_create(&g_exit_queue)) {
        failure = "app exit queue create";
        goto fail;
    }
    g_exit_queue_created = true;

    ProgramBorrowedMappingSpec mapping;
    if (!display_surface_writer_mapping(&g_surface, &mapping)) {
        failure = "surface writer mapping";
        goto fail;
    }

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_exit_queue;
    spec.startup_grant_count = 1U;
    k_memcpy(&spec.startup_grants[0], &g_client.input_grant, sizeof(spec.startup_grants[0]));
    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = JCOS_DISPLAY_SURFACE_SESSION_CLIENT_COOKIE;
    spec.startup_arguments[1] = display_service_input_client_id(&g_client);
    spec.borrowed_mapping = mapping;

    if (!program_launch(&g_app, &spec)) {
        failure = "surface app launch";
        goto fail;
    }
    if (!child_authority_exact()) {
        failure = "surface app capability attenuation";
        goto fail;
    }
    if (!display_surface_writer_claim(&g_surface, &g_app.process)) {
        failure = "surface writer claim";
        goto fail;
    }
    g_writer_pid = g_app.process.id;

    if (!display_service_input_client_bind(&g_client, &g_app.process)) {
        failure = "input client bind";
        goto fail;
    }
    if (display_service_input_client_process_id(&g_client) != g_writer_pid) {
        failure = "input client writer identity";
        goto fail;
    }
    if (!wait_app_blocked()) {
        failure = "surface app initial receive wait";
        goto fail;
    }
    if (!display_service_surface_configure(GRAPHICAL_SESSION_SLOT, 0U, 0U,
            info.width, info.height, 1U)) {
        failure = "surface configure";
        goto fail;
    }
    if (!display_service_focus_surface(GRAPHICAL_SESSION_SLOT)) {
        failure = "surface focus";
        goto fail;
    }

    u32 count = 0U;
    u32 sample = 0U;
    if (!display_service_compose(&count, &sample)) {
        failure = "initial compose";
        goto fail;
    }
    if (count != 1U) {
        failure = "initial compose surface count";
        goto fail;
    }
    (void)sample;

    g_active = true;
    g_event_count = 0ULL;
    if (g_run_count != ~0ULL) ++g_run_count;
    serial_write("R8 GUI: interactive graphical session active; Escape returns to shell.\n");
    return true;

fail:
    serial_write("R8 GUI START FAILED: ");
    serial_write(failure ? failure : "unknown stage");
    serial_write(".\n");
    if (!graphical_session_cleanup())
        serial_write("R8 GUI START CLEANUP FAILED: reboot before retrying.\n");
    return false;
}

bool graphical_session_handle_event(const KeyEvent *event) {
    if (!g_active || !event || !display_service_running() ||
        !compositor_service_running() || !app_blocked() ||
        display_service_focus_slot() != GRAPHICAL_SESSION_SLOT) return false;

    KeyEvent fallback;
    k_memset(&fallback, 0, sizeof(fallback));
    if (input_route_event(event, &fallback) != INPUT_ROUTE_GRAPHICS) return false;
    if (!wait_app_blocked()) return false;

    u32 count = 0U;
    u32 sample = 0U;
    if (!display_service_compose(&count, &sample) || count != 1U) return false;
    (void)sample;
    if (g_event_count != ~0ULL) ++g_event_count;
    return true;
}

static KeyCode protocol_key(u32 key) {
    switch (key) {
        case JCOS_DISPLAY_INPUT_KEY_CHARACTER: return KEY_CHARACTER;
        case JCOS_DISPLAY_INPUT_KEY_ENTER: return KEY_ENTER;
        case JCOS_DISPLAY_INPUT_KEY_BACKSPACE: return KEY_BACKSPACE;
        case JCOS_DISPLAY_INPUT_KEY_TAB: return KEY_TAB;
        case JCOS_DISPLAY_INPUT_KEY_ESCAPE: return KEY_ESCAPE;
        case JCOS_DISPLAY_INPUT_KEY_UP: return KEY_UP;
        case JCOS_DISPLAY_INPUT_KEY_DOWN: return KEY_DOWN;
        case JCOS_DISPLAY_INPUT_KEY_LEFT: return KEY_LEFT;
        case JCOS_DISPLAY_INPUT_KEY_RIGHT: return KEY_RIGHT;
        case JCOS_DISPLAY_INPUT_KEY_HOME: return KEY_HOME;
        case JCOS_DISPLAY_INPUT_KEY_END: return KEY_END;
        case JCOS_DISPLAY_INPUT_KEY_DELETE: return KEY_DELETE;
        case JCOS_DISPLAY_INPUT_KEY_PAGE_UP: return KEY_PAGE_UP;
        case JCOS_DISPLAY_INPUT_KEY_PAGE_DOWN: return KEY_PAGE_DOWN;
        default: return KEY_NONE;
    }
}

bool graphical_session_last_event(KeyEvent *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!display_surface_valid(&g_surface)) return false;
    unsigned char *data = (unsigned char *)display_surface_data(&g_surface);
    if (!data || data[JCOS_DISPLAY_SURFACE_INPUT_MARKER_OFFSET] !=
            JCOS_DISPLAY_SURFACE_INPUT_MARKER) return false;

    out->key = protocol_key(data[JCOS_DISPLAY_SURFACE_INPUT_KEY_OFFSET]);
    out->character = (char)data[JCOS_DISPLAY_SURFACE_INPUT_CHAR_OFFSET];
    u8 flags = data[JCOS_DISPLAY_SURFACE_INPUT_FLAGS_OFFSET];
    out->pressed = (flags & JCOS_DISPLAY_INPUT_FLAG_PRESSED) != 0U;
    out->shift = (flags & JCOS_DISPLAY_INPUT_FLAG_SHIFT) != 0U;
    out->ctrl = (flags & JCOS_DISPLAY_INPUT_FLAG_CTRL) != 0U;
    out->alt = (flags & JCOS_DISPLAY_INPUT_FLAG_ALT) != 0U;
    return out->key != KEY_NONE;
}

bool graphical_session_test_fault_client(ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_active || !display_service_running() ||
        !compositor_service_running() || !app_blocked() ||
        display_service_focus_slot() != GRAPHICAL_SESSION_SLOT || !g_client.active ||
        !g_client.kernel_send_cap ||
        g_client.kernel_send_handle == CAPABILITY_INVALID_HANDLE ||
        !g_app.process_created || !g_app.thread_created) return false;

    u64 process_id = g_app.process.id;
    u64 thread_id = g_app.thread.id;
    Process *kernel = process_kernel();
    if (!kernel || !process_id || !thread_id) return false;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = 2U;
    message.words[0] = JCOS_DISPLAY_CLIENT_HEADER(
        JCOS_DISPLAY_CLIENT_OP_DIAG_FAULT, JCOS_DISPLAY_CLIENT_PROTOCOL_VERSION);
    message.words[1] = display_service_input_client_id(&g_client);

    if (!message.words[1] ||
        !ipc_try_send(kernel, g_client.kernel_send_handle, &message) ||
        !wait_app_dead() || !drain_app_exit(out)) return false;

    return out->process_id == process_id && out->thread_id == thread_id;
}

bool graphical_session_test_fault_compositor(ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_active || !display_service_running() ||
        !compositor_service_running() || !app_blocked() ||
        display_service_focus_slot() != GRAPHICAL_SESSION_SLOT || !g_client.active ||
        !g_app.process_created || !g_app.thread_created) return false;

    u64 display_pid = display_service_process_id();
    u64 process_id = compositor_service_process_id();
    u64 thread_id = compositor_service_thread_id();
    if (!display_pid || !process_id || !thread_id) return false;

    if (!compositor_service_fault() || compositor_service_running()) return false;
    ManagedServiceState state = compositor_service_state();
    if (state != MANAGED_SERVICE_FAILED && state != MANAGED_SERVICE_REAP_PENDING)
        return false;
    if (!compositor_service_last_exit_info(out) || out->process_id != process_id ||
        out->thread_id != thread_id) return false;

    /* Window policy can fail independently. The display process keeps the GOP
     * lease and the graphical client remains blocked on its private endpoint. */
    return (
        g_active && 
        app_blocked() && 
        display_service_running() &&
        display_service_process_id() == display_pid &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == display_pid
    );
}

bool graphical_session_test_fault_display_service(ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_active || !display_service_running() ||
        !compositor_service_running() || !app_blocked() ||
        display_service_focus_slot() != GRAPHICAL_SESSION_SLOT || !g_client.active ||
        !g_app.process_created || !g_app.thread_created) return false;

    u64 process_id = display_service_process_id();
    u64 thread_id = display_service_thread_id();
    if (!process_id || !thread_id) return false;

    if (!display_service_fault() || display_service_running()) return false;
    ManagedServiceState state = display_service_state();
    if (state != MANAGED_SERVICE_FAILED && state != MANAGED_SERVICE_REAP_PENDING)
        return false;
    if (!display_service_last_exit_info(out) || out->process_id != process_id ||
        out->thread_id != thread_id) return false;

    /* A display-process fault does not implicitly kill the client or separate
     * compositor. GOP ownership remains retained until supervised cleanup. */
    return g_active && app_blocked() && compositor_service_running() &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == process_id;
}

bool graphical_session_stop(void) {
    bool was_active = g_active;
    bool cleaned = graphical_session_cleanup();
    if (was_active && cleaned)
        serial_write("R8 GUI: graphical session stopped; terminal ownership restored.\n");
    return cleaned;
}

bool graphical_session_active(void) {
    return g_active;
}

u64 graphical_session_run_count(void) {
    return g_run_count;
}

u64 graphical_session_event_count(void) {
    return g_event_count;
}

bool graphical_session_run(void) {
    if (!graphical_session_start()) return false;
    bool normal_exit = false;

    for (;;) {
        if (!g_active || !display_service_running() || !compositor_service_running() ||
            g_app.thread.state == THREAD_STATE_DEAD) {
            serial_write("R8 GUI: session component stopped unexpectedly; reclaiming display.\n");
            break;
        }
        KeyEvent event;
        k_memset(&event, 0, sizeof(event));
        if (!input_poll(&event)) {
            arch_pause();
            continue;
        }
        /* Escape is intentionally kernel-owned. It remains available even if
         * the Ring3 client or compositor input parser is unhealthy. */
        if (event.pressed && event.key == KEY_ESCAPE) {
            normal_exit = true;
            break;
        }
        if (!graphical_session_handle_event(&event)) {
            serial_write("R8 GUI: focused input/present transaction failed; reclaiming display.\n");
            break;
        }
    }

    bool stopped = graphical_session_stop();
    return normal_exit && stopped;
}