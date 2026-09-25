#include "display_surface_test.h"

#include "address_space.h"
#include "capability.h"
#include "compositor_service.h"
#include "display.h"
#include "display_service.h"
#include "display_surface.h"
#include "endpoint.h"
#include "framebuffer.h"
#include "graphical_session.h"
#include "input_router.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "serial.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"
#include "vmm.h"
#include "../../include/display_service_protocol.h"

#define DISPLAY_SURFACE_TIMEOUT_SECONDS 2ULL
#define DISPLAY_SURFACE_PING_COOKIE 0x52384350494E4721ULL
#define DISPLAY_COMPOSITOR_SURFACE_COUNT 2U
#define DISPLAY_INPUT_CLIENT_COUNT 2U
#define R8D_RECOVERY_STRESS_CYCLES 8U

static DisplaySurface g_surface;
static DisplaySurface g_compositor_surfaces[DISPLAY_COMPOSITOR_SURFACE_COUNT];
static ProgramInstance g_app;
static ProcessExitQueue g_exit_queue;
static DisplayServiceClientGrant g_client_grant;
static bool g_exit_queue_created;
static ProgramInstance g_input_apps[DISPLAY_INPUT_CLIENT_COUNT];
static ProcessExitQueue g_input_exit_queues[DISPLAY_INPUT_CLIENT_COUNT];
static DisplayServiceInputClient g_input_clients[DISPLAY_INPUT_CLIENT_COUNT];
static bool g_input_exit_queue_created[DISPLAY_INPUT_CLIENT_COUNT];

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 tables;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    bool terminal_render;
} DisplaySurfaceBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool same_key_event(const KeyEvent *a, const KeyEvent *b) {
    return a && b && a->key == b->key && a->character == b->character &&
        a->pressed == b->pressed && a->shift == b->shift &&
        a->ctrl == b->ctrl && a->alt == b->alt;
}

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * DISPLAY_SURFACE_TIMEOUT_SECONDS : 0ULL;
}

static bool capture_baseline(DisplaySurfaceBaseline *out) {
    if (!out) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->spaces = address_space_object_count();
    out->threads = thread_object_count();
    out->endpoints = endpoint_object_count();
    out->tables = capability_table_object_count();
    out->exit_queues = process_exit_queue_object_count();
    out->kernel_caps = capability_table_count(caps);
    out->kernel_threads = process_thread_count(kernel);
    out->scheduler_threads = scheduler_thread_count();
    out->terminal_render = terminal_render_enabled();
    return true;
}

static bool baseline_matches(const DisplaySurfaceBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        capability_table_object_count() == baseline->tables &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        terminal_render_enabled() == baseline->terminal_render;
}

static bool close_exit_queue(void) {
    if (!g_exit_queue_created) return true;
    if (process_exit_queue_pending_count(&g_exit_queue)) {
        ProcessExitInfo discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!process_exit_queue_try_receive(&g_exit_queue, &discard)) return false;
    }
    if (process_exit_queue_watch_count(&g_exit_queue)) return false;
    if (!g_exit_queue.closed && !process_exit_queue_close(&g_exit_queue)) return false;
    if (!process_exit_queue_destroy(&g_exit_queue)) return false;
    g_exit_queue_created = false;
    return true;
}

static bool close_input_exit_queue(u32 index) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT) return false;
    ProcessExitQueue *queue = &g_input_exit_queues[index];
    if (!g_input_exit_queue_created[index]) return true;
    if (process_exit_queue_pending_count(queue)) {
        ProcessExitInfo discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!process_exit_queue_try_receive(queue, &discard)) return false;
    }
    if (process_exit_queue_watch_count(queue)) return false;
    if (!queue->closed && !process_exit_queue_close(queue)) return false;
    if (!process_exit_queue_destroy(queue)) return false;
    g_input_exit_queue_created[index] = false;
    return true;
}

static bool release_writer_ledger(DisplaySurface *surface) {
    if (!display_surface_valid(surface)) return true;
    u64 owner = display_surface_writer_process_id(surface);
    return !owner || display_surface_writer_release(surface, owner);
}

static bool cleanup_input_clients(void) {
    bool ok = true;

    for (u32 i = 0U; i < DISPLAY_INPUT_CLIENT_COUNT; ++i) {
        if (program_instance_needs_cleanup(&g_input_apps[i]) &&
            !program_terminate(&g_input_apps[i])) ok = false;
    }

    if (ok) {
        for (u32 i = 0U; i < DISPLAY_INPUT_CLIENT_COUNT; ++i) {
            if (!release_writer_ledger(&g_compositor_surfaces[i])) ok = false;
            if (!close_input_exit_queue(i)) ok = false;
        }
    }

    if (display_service_focus_slot() != JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        compositor_service_running() && !display_service_focus_clear()) ok = false;

    if (ok) {
        for (u32 i = 0U; i < DISPLAY_INPUT_CLIENT_COUNT; ++i)
            if ((g_input_clients[i].active || g_input_clients[i].endpoint_created) &&
                !display_service_input_client_end(&g_input_clients[i])) ok = false;
    }
    return ok;
}

static bool cleanup_all(void) {
    bool session_clean = graphical_session_cleanup();
    bool app_clean = cleanup_input_clients();
    if (g_client_grant.active && !display_service_client_grant_end(&g_client_grant))
        app_clean = false;
    if (program_instance_needs_cleanup(&g_app) && !program_terminate(&g_app)) app_clean = false;
    if (app_clean) {
        if (!release_writer_ledger(&g_surface)) app_clean = false;
        for (u32 i = 0U; i < DISPLAY_COMPOSITOR_SURFACE_COUNT; ++i)
            if (!release_writer_ledger(&g_compositor_surfaces[i])) app_clean = false;
    }
    if (!close_exit_queue()) app_clean = false;

    bool service_clean = compositor_service_cleanup() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_cleanup() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_surface_count() == 0U &&
        display_service_input_client_count() == 0U &&
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL;

    if (!session_clean || !app_clean || !service_clean) return false;
    if (display_surface_valid(&g_surface) && !display_surface_destroy(&g_surface)) return false;
    for (u32 i = 0U; i < DISPLAY_COMPOSITOR_SURFACE_COUNT; ++i)
        if (display_surface_valid(&g_compositor_surfaces[i]) &&
            !display_surface_destroy(&g_compositor_surfaces[i])) return false;
    return true;
}

void display_surface_test_cleanup_run(void) {
    terminal_writeln("R8C DISPLAY SURFACE / COMPOSITOR CLEANUP RETRY:");
    report("APP / SERVICE / SURFACES RELEASED", cleanup_all());
}

static bool wait_app_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();

    while (g_app.thread.state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (g_app.thread.state == THREAD_STATE_READY) {
            if (!g_app.thread.on_run_queue || !g_app.thread.interrupt_context_ready ||
                !g_app.thread.interrupt_rsp || thread_wait_active(&g_app.thread)) return false;
        } else if (g_app.thread.state == THREAD_STATE_BLOCKED) {
            if (g_app.thread.on_run_queue || !g_app.thread.interrupt_context_ready ||
                !g_app.thread.interrupt_rsp || !thread_wait_active(&g_app.thread)) return false;
        } else {
            return false;
        }
        if (!scheduler_yield()) return false;
    }

    return !g_app.thread.on_run_queue && !g_app.thread.interrupt_context_ready &&
        !g_app.thread.interrupt_rsp && !thread_wait_active(&g_app.thread);
}

static bool drain_exit(ProcessExitInfo *out) {
    if (!out || !g_exit_queue_created || process_exit_queue_pending_count(&g_exit_queue) != 1U)
        return false;
    k_memset(out, 0, sizeof(*out));
    return process_exit_queue_try_receive(&g_exit_queue, out) &&
        process_exit_queue_pending_count(&g_exit_queue) == 0U &&
        process_exit_queue_watch_count(&g_exit_queue) == 0U;
}

static bool child_authority_exact(void) {
    if (!g_app.process_created || !g_app.published ||
        g_app.startup_handles[0] == CAPABILITY_INVALID_HANDLE ||
        g_app.startup_handles[1] == CAPABILITY_INVALID_HANDLE) return false;

    CapabilityTable *caps = process_capabilities(&g_app.process);
    if (!caps || capability_table_count(caps) != 2U) return false;

    void *command = 0;
    void *reply = 0;
    bool send = capability_lookup_rights(caps, g_app.startup_handles[0],
        CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &command);
    bool receive = capability_lookup_rights(caps, g_app.startup_handles[1],
        CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &reply);
    if (!send || !receive || !command || !reply || command == reply) return false;

    void *forbidden = 0;
    return !capability_lookup_rights(caps, g_app.startup_handles[0],
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &forbidden) &&
        !capability_lookup_rights(caps, g_app.startup_handles[1],
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &forbidden);
}

static bool writer_mapping_exact(const DisplaySurface *surface) {
    if (!g_app.process_created || !display_surface_valid(surface)) return false;
    AddressSpace *space = process_address_space(&g_app.process);
    frame_t frame = FRAME_INVALID;
    vm_flags_t flags = 0;
    return space && address_space_query_page(space, JCOS_DISPLAY_SURFACE_VIRTUAL_BASE,
            &frame, &flags) && frame == surface->frame && (flags & VM_USER) &&
        (flags & VM_WRITE) && !(flags & VM_EXEC) && !(flags & VM_UNCACHED);
}

static bool launch_surface_app(void) {
    VfsNode *file = vfs_resolve(vfs_root(), JCOS_DISPLAY_SURFACE_APP_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) return false;
    if (!display_service_client_grant_begin(&g_client_grant) || !g_client_grant.incarnation)
        return false;
    if (!process_exit_queue_create(&g_exit_queue)) return false;
    g_exit_queue_created = true;

    ProgramBorrowedMappingSpec mapping;
    if (!display_surface_writer_mapping(&g_surface, &mapping)) return false;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_exit_queue;
    spec.startup_grant_count = 2U;
    k_memcpy(&spec.startup_grants[0], &g_client_grant.command_grant,
        sizeof(spec.startup_grants[0]));
    k_memcpy(&spec.startup_grants[1], &g_client_grant.reply_grant,
        sizeof(spec.startup_grants[1]));
    spec.startup_argument_count = 1U;
    spec.startup_arguments[0] = g_client_grant.incarnation;
    spec.borrowed_mapping = mapping;

    if (!program_launch(&g_app, &spec) || !child_authority_exact() ||
        !writer_mapping_exact(&g_surface) || !display_surface_writer_claim(&g_surface, &g_app.process))
        return false;
    u64 owner_pid = g_app.process.id;
    if (!display_service_client_grant_end(&g_client_grant)) return false;

    if (!wait_app_dead()) return false;
    ProcessExitInfo exit_info;
    if (!drain_exit(&exit_info) || exit_info.reason != PROCESS_EXIT_NORMAL ||
        exit_info.process_id != owner_pid || exit_info.thread_id != g_app.thread.id) return false;

    unsigned char *surface = (unsigned char *)display_surface_data(&g_surface);
    if (!surface || surface[JCOS_DISPLAY_SURFACE_BYTES - 1U] != JCOS_DISPLAY_SURFACE_APP_MARKER)
        return false;

    if (!program_reap(&g_app) || program_instance_needs_cleanup(&g_app) ||
        !display_surface_writer_release(&g_surface, owner_pid) || !close_exit_queue()) return false;
    return true;
}

static bool produce_surface(DisplaySurface *surface, unsigned char fill, u64 *out_pid) {
    if (!surface || !out_pid) return false;
    *out_pid = 0ULL;
    VfsNode *file = vfs_resolve(vfs_root(), JCOS_DISPLAY_SURFACE_APP_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) return false;
    if (!process_exit_queue_create(&g_exit_queue)) return false;
    g_exit_queue_created = true;

    ProgramBorrowedMappingSpec mapping;
    if (!display_surface_writer_mapping(surface, &mapping)) return false;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_exit_queue;
    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = JCOS_DISPLAY_SURFACE_PRODUCER_COOKIE;
    spec.startup_arguments[1] = fill;
    spec.borrowed_mapping = mapping;

    if (!program_launch(&g_app, &spec) || !writer_mapping_exact(surface)) return false;
    CapabilityTable *caps = process_capabilities(&g_app.process);
    if (!caps || capability_table_count(caps) != 0U ||
        !display_surface_writer_claim(surface, &g_app.process)) return false;

    u64 owner_pid = g_app.process.id;
    u64 owner_tid = g_app.thread.id;
    if (!wait_app_dead()) return false;

    ProcessExitInfo exit_info;
    if (!drain_exit(&exit_info) || exit_info.reason != PROCESS_EXIT_NORMAL ||
        exit_info.process_id != owner_pid || exit_info.thread_id != owner_tid) return false;

    unsigned char *data = (unsigned char *)display_surface_data(surface);
    if (!data || data[0] != fill || data[JCOS_DISPLAY_SURFACE_BYTES - 1U] != fill) return false;

    if (!program_reap(&g_app) || program_instance_needs_cleanup(&g_app) ||
        !display_surface_writer_release(surface, owner_pid) || !close_exit_queue()) return false;
    *out_pid = owner_pid;
    return true;
}

static bool input_app_blocked(u32 index) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT) return false;
    Thread *thread = &g_input_apps[index].thread;
    return thread->state == THREAD_STATE_BLOCKED && !thread->on_run_queue &&
        thread->interrupt_context_ready && thread->interrupt_rsp && thread_wait_active(thread);
}

static bool wait_input_app_blocked(u32 index) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT) return false;
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    Thread *thread = &g_input_apps[index].thread;

    while (!input_app_blocked(index)) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (thread->state != THREAD_STATE_READY || !thread->on_run_queue ||
            !thread->interrupt_context_ready || !thread->interrupt_rsp ||
            thread_wait_active(thread)) return false;
        if (!scheduler_yield()) return false;
    }
    return true;
}

static bool wait_input_app_dead(u32 index) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT) return false;
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    Thread *thread = &g_input_apps[index].thread;

    while (thread->state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (thread->state == THREAD_STATE_READY) {
            /* A blocking IPC RECEIVE is legitimately READY while its wait
             * reservation is still live after SEND wakes it. The resumed
             * continuation consumes the queued message, ends the reservation,
             * and then this one-shot client exits. */
            if (!thread->on_run_queue || !thread->interrupt_context_ready ||
                !thread->interrupt_rsp) return false;
        } else if (thread->state == THREAD_STATE_BLOCKED) {
            if (thread->on_run_queue || !thread->interrupt_context_ready ||
                !thread->interrupt_rsp || !thread_wait_active(thread)) return false;
        } else return false;
        if (!scheduler_yield()) return false;
    }

    return !thread->on_run_queue && !thread->interrupt_context_ready &&
        !thread->interrupt_rsp && !thread_wait_active(thread);
}

static bool drain_input_exit(u32 index, ProcessExitInfo *out) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT || !out || !g_input_exit_queue_created[index])
        return false;
    ProcessExitQueue *queue = &g_input_exit_queues[index];
    if (process_exit_queue_pending_count(queue) != 1U) return false;
    k_memset(out, 0, sizeof(*out));
    return process_exit_queue_try_receive(queue, out) &&
        process_exit_queue_pending_count(queue) == 0U &&
        process_exit_queue_watch_count(queue) == 0U;
}

static bool input_child_authority_exact(u32 index) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT) return false;
    ProgramInstance *app = &g_input_apps[index];
    DisplayServiceInputClient *client = &g_input_clients[index];
    if (!app->process_created || !app->published ||
        app->startup_handles[0] == CAPABILITY_INVALID_HANDLE) return false;

    CapabilityTable *caps = process_capabilities(&app->process);
    if (!caps || capability_table_count(caps) != 1U) return false;
    void *object = 0;
    if (!capability_lookup_rights(caps, app->startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &object) || object != &client->input_endpoint) return false;
    object = 0;
    return !capability_lookup_rights(caps, app->startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &object);
}

static bool input_writer_mapping_exact(u32 index, const DisplaySurface *surface) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT || !surface ||
        !g_input_apps[index].process_created) return false;
    AddressSpace *space = process_address_space(&g_input_apps[index].process);
    frame_t frame = FRAME_INVALID;
    vm_flags_t flags = 0;
    return space && address_space_query_page(space, JCOS_DISPLAY_SURFACE_VIRTUAL_BASE,
            &frame, &flags) && frame == surface->frame && (flags & VM_USER) &&
        (flags & VM_WRITE) && !(flags & VM_EXEC) && !(flags & VM_UNCACHED);
}

static bool launch_input_client(u32 index, DisplaySurface *surface, u32 slot) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT || !surface || index != slot) return false;
    VfsNode *file = vfs_resolve(vfs_root(), JCOS_DISPLAY_SURFACE_APP_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) return false;
    if (!display_service_input_client_begin(&g_input_clients[index], surface, slot) ||
        !display_service_input_client_id(&g_input_clients[index])) return false;
    if (!process_exit_queue_create(&g_input_exit_queues[index])) return false;
    g_input_exit_queue_created[index] = true;

    ProgramBorrowedMappingSpec mapping;
    if (!display_surface_writer_mapping(surface, &mapping)) return false;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_input_exit_queues[index];
    spec.startup_grant_count = 1U;
    k_memcpy(&spec.startup_grants[0], &g_input_clients[index].input_grant,
        sizeof(spec.startup_grants[0]));
    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = JCOS_DISPLAY_SURFACE_INPUT_CLIENT_COOKIE;
    spec.startup_arguments[1] = display_service_input_client_id(&g_input_clients[index]);
    spec.borrowed_mapping = mapping;

    ProgramInstance *app = &g_input_apps[index];
    if (!program_launch(app, &spec) || !input_child_authority_exact(index) ||
        !input_writer_mapping_exact(index, surface) ||
        !display_surface_writer_claim(surface, &app->process) ||
        !display_service_input_client_bind(&g_input_clients[index], &app->process) ||
        display_service_input_client_process_id(&g_input_clients[index]) != app->process.id)
        return false;

    return wait_input_app_blocked(index);
}

static bool finish_input_app(u32 index, DisplaySurface *surface) {
    if (index >= DISPLAY_INPUT_CLIENT_COUNT || !surface || !wait_input_app_dead(index))
        return false;
    ProgramInstance *app = &g_input_apps[index];
    u64 owner_pid = app->process.id;
    u64 owner_tid = app->thread.id;
    ProcessExitInfo exit_info;
    if (!drain_input_exit(index, &exit_info) || exit_info.reason != PROCESS_EXIT_NORMAL ||
        exit_info.process_id != owner_pid || exit_info.thread_id != owner_tid) return false;
    if (!program_reap(app) || program_instance_needs_cleanup(app) ||
        !display_surface_writer_release(surface, owner_pid) || !close_input_exit_queue(index))
        return false;
    return true;
}

static KeyEvent display_input_key(char character, bool shift, bool ctrl, bool alt) {
    KeyEvent event;
    k_memset(&event, 0, sizeof(event));
    event.key = KEY_CHARACTER;
    event.character = character;
    event.pressed = true;
    event.shift = shift;
    event.ctrl = ctrl;
    event.alt = alt;
    return event;
}

static bool input_marker_matches(DisplaySurface *surface, char character, u8 flags) {
    unsigned char *data = (unsigned char *)display_surface_data(surface);
    return data && data[JCOS_DISPLAY_SURFACE_INPUT_MARKER_OFFSET] ==
            JCOS_DISPLAY_SURFACE_INPUT_MARKER &&
        data[JCOS_DISPLAY_SURFACE_INPUT_KEY_OFFSET] == JCOS_DISPLAY_INPUT_KEY_CHARACTER &&
        data[JCOS_DISPLAY_SURFACE_INPUT_CHAR_OFFSET] == (unsigned char)character &&
        data[JCOS_DISPLAY_SURFACE_INPUT_FLAGS_OFFSET] == flags;
}

static bool input_marker_absent(DisplaySurface *surface) {
    unsigned char *data = (unsigned char *)display_surface_data(surface);
    return data && data[JCOS_DISPLAY_SURFACE_INPUT_MARKER_OFFSET] !=
        JCOS_DISPLAY_SURFACE_INPUT_MARKER;
}

static u32 masked_channel(u32 value, u32 mask) {
    if (!mask) return 0U;
    u32 shift = 0U;
    while (shift < 31U && ((mask >> shift) & 1U) == 0U) ++shift;
    u32 max = mask >> shift;
    return (u32)((((u64)(value & 0xFFU) * (u64)max) / 255ULL << shift) & mask);
}

static u32 expected_target_pixel(const FramebufferInfo *info, unsigned char pixel) {
    if (!info) return 0U;
    u32 r = (u32)((pixel >> 5) & 0x07U);
    u32 g = (u32)((pixel >> 2) & 0x07U);
    u32 b = (u32)(pixel & 0x03U);
    r = (r * 255U) / 7U;
    g = (g * 255U) / 7U;
    b = (b * 255U) / 3U;
    if (info->pixel_format == 0U) return r | (g << 8) | (b << 16);
    if (info->pixel_format == 1U) return b | (g << 8) | (r << 16);
    return masked_channel(r, info->red_mask) |
        masked_channel(g, info->green_mask) |
        masked_channel(b, info->blue_mask);
}

void display_surface_test_run(void) {
    terminal_writeln("R8C BOUNDED SURFACE / USERSPACE COMPOSITOR TEST:");

    if (!cleanup_all()) {
        report("NO RETAINED DISPLAY SURFACE STATE", false);
        return;
    }

    FramebufferInfo info;
    k_memset(&info, 0, sizeof(info));
    bool descriptor = framebuffer_info(&info) && info.physical_base && info.size &&
        info.width && info.height && info.pixels_per_scanline >= info.width;
    report("VALIDATED GOP RESOURCE AVAILABLE", descriptor);
    if (!descriptor) goto fail;

    if (!info.direct_map_safe) {
        report("NON-MMIO GOP / SAFE GRAPHICS FALLBACK", true);
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R8C BOUNDED SURFACE / USERSPACE COMPOSITOR TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

    DisplaySurfaceBaseline baseline;
    bool baseline_ok = capture_baseline(&baseline) && scheduler_preemption_enabled() && timer_initialized();
    report("KERNEL / RESOURCE BASELINE", baseline_ok);
    if (!baseline_ok) goto fail;

    /* R8C.1 regression: an independent app writes one bounded surface and
     * requests presentation without receiving direct framebuffer authority. */
    bool created = display_surface_create(&g_surface) && display_surface_valid(&g_surface) &&
        display_surface_id(&g_surface) && display_surface_data(&g_surface) &&
        pmm_stats().free_pages + 1ULL == baseline.free_pages;
    report("ONE-PAGE RGB332 SURFACE ALLOCATED", created);
    if (!created) goto fail;

    bool service = display_service_start() && display_service_running() &&
        display_direct_state() == DISPLAY_LEASE_USER && !terminal_render_enabled();
    report("RING3 DISPLAY SERVICE OWNS GOP", service);
    if (!service) goto fail;

    bool reader = display_service_surface_attach(&g_surface) &&
        display_service_surface_attached() && display_service_surface_mapping_valid();
    report("DISPLAY SERVICE MAPS SURFACE RO / NX", reader);
    if (!reader) goto fail;

    bool app = launch_surface_app();
    report("INDEPENDENT APP WRITES RW / NX SURFACE", app);
    if (!app) goto fail;

    u64 pong = 0ULL;
    bool presented = display_service_ping(DISPLAY_SURFACE_PING_COOKIE, &pong) &&
        pong == DISPLAY_SURFACE_PING_COOKIE && display_service_surface_mapping_valid();
    report("APP -> SURFACE -> DISPLAY SERVICE -> GOP", presented);
    if (!presented) goto fail;

    bool detached = display_service_surface_detach() && !display_service_surface_attached();
    report("SURFACE READER DETACHED", detached);
    if (!detached) goto fail;

    bool stopped = display_service_stop() && display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        terminal_render_enabled() == baseline.terminal_render;
    report("DISPLAY SERVICE STOP / GOP RECLAIM", stopped);
    if (!stopped) goto fail;

    bool released = display_surface_destroy(&g_surface) && baseline_matches(&baseline);
    report("R8C.1 FULL BASELINE RESTORED", released);
    if (!released) goto fail;

    /* R8C.2: two distinct producer incarnations own different surface frames.
     * The Ring3 service receives both frames read-only and owns placement/z. */
    bool two_created = display_surface_create(&g_compositor_surfaces[0]) &&
        display_surface_create(&g_compositor_surfaces[1]) &&
        display_surface_id(&g_compositor_surfaces[0]) &&
        display_surface_id(&g_compositor_surfaces[1]) &&
        display_surface_id(&g_compositor_surfaces[0]) != display_surface_id(&g_compositor_surfaces[1]) &&
        pmm_stats().free_pages + 2ULL == baseline.free_pages;
    report("TWO BOUNDED SURFACES / UNIQUE IDS", two_created);
    if (!two_created) goto fail;

    unsigned char back_color = JCOS_DISPLAY_SURFACE_COLOR_RED;
    unsigned char front_color = JCOS_DISPLAY_SURFACE_COLOR_GREEN;
    u64 first_pid = 0ULL;
    u64 second_pid = 0ULL;
    bool producers = produce_surface(&g_compositor_surfaces[0], back_color, &first_pid) &&
        produce_surface(&g_compositor_surfaces[1], front_color, &second_pid) &&
        first_pid && second_pid && first_pid != second_pid &&
        !display_surface_writer_process_id(&g_compositor_surfaces[0]) &&
        !display_surface_writer_process_id(&g_compositor_surfaces[1]);
    report("DISTINCT RING3 PRODUCERS / RW-NX OWNERSHIP", producers);
    if (!producers) goto fail;

    service = display_service_start() && display_service_running() &&
        display_direct_state() == DISPLAY_LEASE_USER && !terminal_render_enabled();
    report("RING3 DISPLAY SERVICE OWNS GOP", service);
    if (!service) goto fail;

    bool readers = display_service_surface_attach_slot(&g_compositor_surfaces[0], 0U) &&
        display_service_surface_attach_slot(&g_compositor_surfaces[1], 1U) &&
        display_service_surface_count() == 2U &&
        display_service_surface_slot_mapping_valid(0U) &&
        display_service_surface_slot_mapping_valid(1U);
    report("DISPLAY SERVICE RO-MAPS TWO SURFACE SLOTS", readers);
    if (!readers) goto fail;

    u64 display_pid = display_service_process_id();
    bool compositor = compositor_service_start() && compositor_service_running() &&
        compositor_service_process_id() && compositor_service_process_id() != display_pid &&
        display_direct_owner_process_id() == display_pid;
    report("SEPARATE RING3 COMPOSITOR / DISPLAY LEASE RETAINED", compositor);
    if (!compositor) goto fail;

    if (info.width < 4U || info.height < 4U) {
        report("GOP LARGE ENOUGH FOR PLACEMENT TEST", false);
        goto fail;
    }

    u32 front_width = info.width / 2U;
    u32 front_height = info.height / 2U;
    u32 front_x = (info.width - front_width) / 2U;
    u32 front_y = (info.height - front_height) / 2U;

    bool configured = display_service_surface_configure(0U, 0U, 0U,
            info.width, info.height, 1U) &&
        display_service_surface_configure(1U, front_x, front_y,
            front_width, front_height, 2U);
    report("USERSPACE PLACEMENT / Z POLICY CONFIGURED", configured);
    if (!configured) goto fail;

    u32 composed_count = 0U;
    u32 sample = 0U;
    bool front_on_top = display_service_compose(&composed_count, &sample) &&
        composed_count == 2U && sample == expected_target_pixel(&info, front_color);
    report("TWO-SURFACE COMPOSE / HIGHER Z WINS", front_on_top);
    if (!front_on_top) goto fail;

    u32 corner_width = info.width / 3U;
    u32 corner_height = info.height / 3U;
    if (!corner_width) corner_width = 1U;
    if (!corner_height) corner_height = 1U;
    bool moved = display_service_surface_configure(1U, 0U, 0U,
            corner_width, corner_height, 2U) &&
        display_service_compose(&composed_count, &sample) && composed_count == 2U &&
        sample == expected_target_pixel(&info, back_color);
    report("PLACEMENT MOVES TOP SURFACE OFF OVERLAP", moved);
    if (!moved) goto fail;

    bool z_swap = display_service_surface_configure(1U, front_x, front_y,
            front_width, front_height, 1U) &&
        display_service_surface_configure(0U, 0U, 0U,
            info.width, info.height, 3U) &&
        display_service_compose(&composed_count, &sample) && composed_count == 2U &&
        sample == expected_target_pixel(&info, back_color);
    report("Z-ORDER SWAP CHANGES OVERLAP OWNER", z_swap);
    if (!z_swap) goto fail;

    /* R8C.3: each graphical client receives a dedicated RECEIVE-only input
     * endpoint. Ring3 compositor focus chooses the destination; the kernel
     * transports a bounded key event only to that focused endpoint. */
    bool input_clients = launch_input_client(0U, &g_compositor_surfaces[0], 0U) &&
        launch_input_client(1U, &g_compositor_surfaces[1], 1U) &&
        display_service_input_client_count() == 2U &&
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        input_marker_absent(&g_compositor_surfaces[0]) &&
        input_marker_absent(&g_compositor_surfaces[1]);
    report("PER-CLIENT RECEIVE-ONLY INPUT CAPABILITIES", input_clients);
    if (!input_clients) goto fail;

    KeyEvent first_event = display_input_key('A', true, false, false);
    KeyEvent fallback_event;
    k_memset(&fallback_event, 0, sizeof(fallback_event));
    InputRouteResult no_focus_route = input_route_event(&first_event, &fallback_event);
    bool no_focus = no_focus_route == INPUT_ROUTE_FALLBACK &&
        same_key_event(&fallback_event, &first_event) &&
        input_app_blocked(0U) && input_app_blocked(1U);
    report("NO FOCUS / SHELL FALLBACK PRESERVED", no_focus);
    if (!no_focus) goto fail;

    bool first_focus = display_service_focus_surface(0U) &&
        display_service_focus_slot() == 0U;
    report("RING3 COMPOSITOR FOCUSES SURFACE 0", first_focus);
    if (!first_focus) goto fail;

    u8 first_flags = (u8)(JCOS_DISPLAY_INPUT_FLAG_PRESSED | JCOS_DISPLAY_INPUT_FLAG_SHIFT);
    k_memset(&fallback_event, 0xFF, sizeof(fallback_event));
    bool first_routed = input_route_event(&first_event, &fallback_event) == INPUT_ROUTE_GRAPHICS &&
        fallback_event.key == KEY_NONE &&
        wait_input_app_dead(0U) &&
        input_marker_matches(&g_compositor_surfaces[0], 'A', first_flags) &&
        input_app_blocked(1U) && input_marker_absent(&g_compositor_surfaces[1]);
    report("HARDWARE ROUTER SENDS ONLY TO FOCUSED CLIENT", first_routed);
    if (!first_routed) goto fail;

    bool second_focus = display_service_focus_surface(1U) &&
        display_service_focus_slot() == 1U;
    report("FOCUS SWITCHES TO SURFACE 1", second_focus);
    if (!second_focus) goto fail;

    bool first_reaped = finish_input_app(0U, &g_compositor_surfaces[0]) &&
        display_service_input_client_end(&g_input_clients[0]) &&
        display_service_input_client_count() == 1U;
    report("FIRST CLIENT EXIT / INPUT AUTHORITY RECLAIM", first_reaped);
    if (!first_reaped) goto fail;

    KeyEvent second_event = display_input_key('B', false, true, true);
    u8 second_flags = (u8)(JCOS_DISPLAY_INPUT_FLAG_PRESSED |
        JCOS_DISPLAY_INPUT_FLAG_CTRL | JCOS_DISPLAY_INPUT_FLAG_ALT);
    k_memset(&fallback_event, 0xFF, sizeof(fallback_event));
    bool second_routed = input_route_event(&second_event, &fallback_event) == INPUT_ROUTE_GRAPHICS &&
        fallback_event.key == KEY_NONE && wait_input_app_dead(1U) &&
        input_marker_matches(&g_compositor_surfaces[1], 'B', second_flags);
    report("FOCUS SWITCH ROUTES NEXT HARDWARE KEY", second_routed);
    if (!second_routed) goto fail;

    k_memset(&fallback_event, 0, sizeof(fallback_event));
    bool focus_cleared = display_service_focus_clear() &&
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        input_route_event(&second_event, &fallback_event) == INPUT_ROUTE_FALLBACK &&
        same_key_event(&fallback_event, &second_event);
    report("FOCUS CLEAR RESTORES SHELL FALLBACK", focus_cleared);
    if (!focus_cleared) goto fail;

    bool second_reaped = finish_input_app(1U, &g_compositor_surfaces[1]) &&
        display_service_input_client_end(&g_input_clients[1]) &&
        display_service_input_client_count() == 0U;
    report("SECOND CLIENT EXIT / INPUT AUTHORITY RECLAIM", second_reaped);
    if (!second_reaped) goto fail;

    bool removed = display_service_surface_remove(1U) &&
        display_service_compose(&composed_count, &sample) && composed_count == 1U &&
        sample == expected_target_pixel(&info, back_color);
    report("SURFACE REMOVE / SINGLE-SURFACE COMPOSE", removed);
    if (!removed) goto fail;

    bool readers_detached = display_service_surface_detach_slot(1U) &&
        display_service_surface_detach_slot(0U) && display_service_surface_count() == 0U;
    report("ALL COMPOSITOR READER MAPPINGS DETACHED", readers_detached);
    if (!readers_detached) goto fail;

    bool compositor_stopped = compositor_service_stop() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED;
    report("COMPOSITOR STOP / DISPLAY SERVICE SURVIVES", compositor_stopped);
    if (!compositor_stopped) goto fail;

    stopped = display_service_stop() && display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        terminal_render_enabled() == baseline.terminal_render;
    report("DISPLAY SERVICE STOP / GOP RECLAIM", stopped);
    if (!stopped) goto fail;

    bool final_release = display_surface_destroy(&g_compositor_surfaces[1]) &&
        display_surface_destroy(&g_compositor_surfaces[0]) && baseline_matches(&baseline);
    report("MULTI-SURFACE FULL BASELINE RESTORED", final_release);
    if (!final_release) goto fail;

    /* R8C.5: exercise the same persistent one-client session used by the `gui`
     * shell command. The acceptance path injects one event instead of polling
     * hardware, then proves Escape-style teardown restores the exact baseline. */
    u64 session_runs_before = graphical_session_run_count();
    bool session_started = graphical_session_start() && graphical_session_active() &&
        graphical_session_run_count() == session_runs_before + 1ULL &&
        display_service_running() && compositor_service_running() &&
        display_service_focus_slot() == 0U &&
        display_service_input_client_count() == 1U;
    report("PERSISTENT GRAPHICAL SESSION / FOCUSED CLIENT", session_started);
    if (!session_started) goto fail;

    KeyEvent session_event = display_input_key('J', false, false, false);
    KeyEvent observed_event;
    k_memset(&observed_event, 0, sizeof(observed_event));
    bool session_input = graphical_session_handle_event(&session_event) &&
        graphical_session_event_count() == 1ULL &&
        graphical_session_last_event(&observed_event) &&
        same_key_event(&session_event, &observed_event);
    report("PERSISTENT SESSION INPUT / REDRAW", session_input);
    if (!session_input) goto fail;

    bool session_stopped = graphical_session_stop() && !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL && baseline_matches(&baseline);
    report("SESSION EXIT / TERMINAL + RESOURCE BASELINE", session_stopped);
    if (!session_stopped) goto fail;

    /* R8D.1: deliberately #UD the persistent Ring3 graphical client. The
     * compositor and kernel stay alive, then the normal ownership transaction
     * reaps the dead client and restores the exact pre-session baseline. */
    u64 recovery_runs_before = graphical_session_run_count();
    bool fault_session_started = graphical_session_start() &&
        graphical_session_active() &&
        graphical_session_run_count() == recovery_runs_before + 1ULL &&
        display_service_running() && display_service_focus_slot() == 0U;
    report("R8D.1 CLIENT-FAULT SESSION START", fault_session_started);
    if (!fault_session_started) goto fail;

    ProcessExitInfo client_fault;
    k_memset(&client_fault, 0, sizeof(client_fault));
    bool client_fault_contained = graphical_session_test_fault_client(&client_fault) &&
        client_fault.reason == PROCESS_EXIT_FAULT && client_fault.vector == 6ULL &&
        client_fault.process_id != 0ULL && client_fault.thread_id != 0ULL &&
        display_service_running() && compositor_service_running() &&
        display_direct_state() == DISPLAY_LEASE_USER;
    report("RING3 GRAPHICAL CLIENT #UD CONTAINED", client_fault_contained);
    if (!client_fault_contained) goto fail;

    bool client_recovered = graphical_session_cleanup() &&
        !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        display_service_input_client_count() == 0U &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL && baseline_matches(&baseline);
    report("CLIENT CRASH / GOP + INPUT RECLAIM", client_recovered);
    if (!client_recovered) goto fail;

    bool post_fault_relaunch = graphical_session_start() &&
        graphical_session_active() &&
        graphical_session_run_count() == recovery_runs_before + 2ULL &&
        graphical_session_stop() && !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL && baseline_matches(&baseline);
    report("GUI RELAUNCH AFTER CLIENT CRASH", post_fault_relaunch);
    if (!post_fault_relaunch) goto fail;

    /* The compositor is now a distinct Ring3 policy service. Its failure must
     * leave the GOP-owning display process and graphical client alive, and a
     * replacement compositor must reconnect to that same display incarnation. */
    u64 compositor_fault_runs_before = graphical_session_run_count();
    bool compositor_fault_session_started = graphical_session_start() &&
        graphical_session_active() &&
        graphical_session_run_count() == compositor_fault_runs_before + 1ULL &&
        display_service_running() && compositor_service_running() &&
        display_service_focus_slot() == 0U;
    report("COMPOSITOR-FAULT SESSION START", compositor_fault_session_started);
    if (!compositor_fault_session_started) goto fail;

    u64 live_display_pid = display_service_process_id();
    u64 live_display_incarnation = display_service_incarnation();
    ProcessExitInfo compositor_fault;
    k_memset(&compositor_fault, 0, sizeof(compositor_fault));
    bool compositor_fault_contained =
        graphical_session_test_fault_compositor(&compositor_fault) &&
        compositor_fault.reason == PROCESS_EXIT_FAULT &&
        compositor_fault.vector == 6ULL && compositor_fault.process_id != 0ULL &&
        compositor_fault.thread_id != 0ULL && graphical_session_active() &&
        !compositor_service_running() &&
        (compositor_service_state() == MANAGED_SERVICE_FAILED ||
         compositor_service_state() == MANAGED_SERVICE_REAP_PENDING) &&
        display_service_running() && display_service_process_id() == live_display_pid &&
        display_service_incarnation() == live_display_incarnation &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == live_display_pid &&
        !terminal_render_enabled();
    report("COMPOSITOR #UD / DISPLAY + CLIENT SURVIVE", compositor_fault_contained);
    if (!compositor_fault_contained) goto fail;

    KeyEvent compositor_fallback_event = display_input_key('R', false, true, false);
    KeyEvent compositor_fallback;
    k_memset(&compositor_fallback, 0, sizeof(compositor_fallback));
    bool compositor_input_fallback =
        input_route_event(&compositor_fallback_event, &compositor_fallback) ==
            INPUT_ROUTE_FALLBACK &&
        same_key_event(&compositor_fallback_event, &compositor_fallback);
    report("DEAD COMPOSITOR / INPUT FALLBACK PRESERVED", compositor_input_fallback);
    if (!compositor_input_fallback) goto fail;

    bool compositor_restarted = compositor_service_recover() &&
        compositor_service_start() && compositor_service_running() &&
        display_service_process_id() == live_display_pid &&
        display_service_incarnation() == live_display_incarnation &&
        display_service_surface_configure(0U, 0U, 0U, info.width, info.height, 1U) &&
        display_service_focus_surface(0U) &&
        display_service_compose(&composed_count, &sample) && composed_count == 1U;
    report("COMPOSITOR RESTART / SAME DISPLAY INCARNATION", compositor_restarted);
    if (!compositor_restarted) goto fail;

    KeyEvent resumed_event = display_input_key('C', false, false, false);
    KeyEvent resumed_observed;
    k_memset(&resumed_observed, 0, sizeof(resumed_observed));
    bool resumed = graphical_session_handle_event(&resumed_event) &&
        graphical_session_last_event(&resumed_observed) &&
        same_key_event(&resumed_event, &resumed_observed);
    report("INPUT + COMPOSE AFTER COMPOSITOR RESTART", resumed);
    if (!resumed) goto fail;

    bool compositor_recovered = graphical_session_stop() && !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL && baseline_matches(&baseline);
    report("COMPOSITOR RECOVERY / SESSION BASELINE", compositor_recovered);
    if (!compositor_recovered) goto fail;

    /* A display-service crash is a separate failure domain. The compositor and
     * client remain contained until supervisor cleanup reclaims the GOP lease. */
    u64 display_fault_runs_before = graphical_session_run_count();
    bool display_fault_session_started = graphical_session_start() &&
        graphical_session_active() &&
        graphical_session_run_count() == display_fault_runs_before + 1ULL &&
        display_service_running() && compositor_service_running() &&
        display_service_focus_slot() == 0U;
    report("DISPLAY-FAULT SESSION START", display_fault_session_started);
    if (!display_fault_session_started) goto fail;

    ProcessExitInfo display_fault;
    k_memset(&display_fault, 0, sizeof(display_fault));
    bool display_fault_contained = graphical_session_test_fault_display_service(&display_fault) &&
        display_fault.reason == PROCESS_EXIT_FAULT && display_fault.vector == 6ULL &&
        display_fault.process_id != 0ULL && display_fault.thread_id != 0ULL &&
        graphical_session_active() && !display_service_running() &&
        compositor_service_running() &&
        (display_service_state() == MANAGED_SERVICE_FAILED ||
         display_service_state() == MANAGED_SERVICE_REAP_PENDING) &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == display_fault.process_id &&
        !terminal_render_enabled();
    report("DISPLAY SERVICE #UD / COMPOSITOR + CLIENT CONTAINED", display_fault_contained);
    if (!display_fault_contained) goto fail;

    KeyEvent display_fallback_event = display_input_key('D', false, false, true);
    KeyEvent display_fallback;
    k_memset(&display_fallback, 0, sizeof(display_fallback));
    bool display_input_fallback =
        input_route_event(&display_fallback_event, &display_fallback) == INPUT_ROUTE_FALLBACK &&
        same_key_event(&display_fallback_event, &display_fallback);
    report("DEAD DISPLAY SERVICE / INPUT FALLBACK PRESERVED", display_input_fallback);
    if (!display_input_fallback) goto fail;

    serial_write("R8 OBSERVABILITY: display-service fault contained; kernel alive; reclaim pending.\n");

    bool display_recovered = graphical_session_cleanup() && !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL && baseline_matches(&baseline);
    report("DISPLAY CRASH / GOP + SESSION RECLAIM", display_recovered);
    if (!display_recovered) goto fail;

    /* Bounded alternating recovery stress. Every cycle enters the same
     * persistent graphical session used by the shell, proves input can make a
     * round trip, then faults either the client or display service. Each recovery
     * must restore the exact pre-test resource baseline before the next start. */
    u64 stress_runs_before = graphical_session_run_count();
    bool recovery_stress = true;
    for (u32 cycle = 0U; cycle < R8D_RECOVERY_STRESS_CYCLES; ++cycle) {
        if (!graphical_session_start() || !graphical_session_active() ||
            !display_service_running() || !compositor_service_running() ||
            display_service_focus_slot() != 0U) {
            recovery_stress = false;
            break;
        }

        KeyEvent stress_event = display_input_key((char)('A' + (cycle % 26U)),
            (cycle & 1U) != 0U, (cycle & 2U) != 0U, false);
        KeyEvent stress_observed;
        k_memset(&stress_observed, 0, sizeof(stress_observed));
        if (!graphical_session_handle_event(&stress_event) ||
            !graphical_session_last_event(&stress_observed) ||
            !same_key_event(&stress_event, &stress_observed)) {
            recovery_stress = false;
            break;
        }

        ProcessExitInfo stress_fault;
        k_memset(&stress_fault, 0, sizeof(stress_fault));
        if ((cycle & 1U) == 0U) {
            if (!graphical_session_test_fault_client(&stress_fault) ||
                stress_fault.reason != PROCESS_EXIT_FAULT || stress_fault.vector != 6ULL ||
                !display_service_running() || !compositor_service_running() ||
                display_direct_state() != DISPLAY_LEASE_USER) {
                recovery_stress = false;
                break;
            }
        } else {
            if (!graphical_session_test_fault_display_service(&stress_fault) ||
                stress_fault.reason != PROCESS_EXIT_FAULT || stress_fault.vector != 6ULL ||
                !compositor_service_running() ||
                display_service_running() || terminal_render_enabled() ||
                display_direct_state() != DISPLAY_LEASE_USER ||
                display_direct_owner_process_id() != stress_fault.process_id) {
                recovery_stress = false;
                break;
            }

            /* This marker is intentionally emitted while the framebuffer is
             * unavailable to the kernel terminal. A serial console can still
             * distinguish a live/recovering kernel from a blank or wedged GUI. */
            serial_write("R8 STRESS: display-service fault contained; kernel alive; reclaim pending.\n");

            KeyEvent fallback_event = display_input_key('Z', false, false, true);
            KeyEvent fallback;
            k_memset(&fallback, 0, sizeof(fallback));
            if (input_route_event(&fallback_event, &fallback) != INPUT_ROUTE_FALLBACK ||
                !same_key_event(&fallback_event, &fallback)) {
                recovery_stress = false;
                break;
            }
        }

        if (!graphical_session_cleanup() || graphical_session_active() ||
            compositor_service_state() != MANAGED_SERVICE_STOPPED ||
            display_service_state() != MANAGED_SERVICE_STOPPED ||
            display_service_running() ||
            display_service_focus_slot() != JCOS_DISPLAY_SURFACE_FOCUS_NONE ||
            display_service_input_client_count() != 0U ||
            display_service_surface_count() != 0U ||
            display_direct_state() != DISPLAY_LEASE_KERNEL ||
            display_direct_owner_process_id() != 0ULL || !baseline_matches(&baseline)) {
            recovery_stress = false;
            break;
        }
    }
    recovery_stress = recovery_stress &&
        graphical_session_run_count() == stress_runs_before + R8D_RECOVERY_STRESS_CYCLES;
    report("ALTERNATING CLIENT / DISPLAY RECOVERY STRESS", recovery_stress);
    if (!recovery_stress) goto fail;

    /* One final ordinary session proves that repeated fault recovery did not
     * leave the display path in a degraded, recovery-only state. */
    u64 final_runs_before = graphical_session_run_count();
    KeyEvent final_event = display_input_key('G', true, false, false);
    KeyEvent final_observed;
    k_memset(&final_observed, 0, sizeof(final_observed));
    bool final_normal_session = graphical_session_start() &&
        graphical_session_run_count() == final_runs_before + 1ULL &&
        graphical_session_handle_event(&final_event) &&
        graphical_session_last_event(&final_observed) &&
        same_key_event(&final_event, &final_observed) &&
        graphical_session_stop() && !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL && baseline_matches(&baseline);
    report("R8D.3 NORMAL GUI AFTER RECOVERY STRESS", final_normal_session);
    if (!final_normal_session) goto fail;

    /* Reaching this point means the independently-built app displayed through
     * the userspace stack, client and compositor faults were contained, serial
     * recovery remained available while framebuffer rendering was unavailable,
     * output was reclaimed, and a fresh session worked without a reboot. */
    bool r8_exit_gate = !graphical_session_active() &&
        compositor_service_state() == MANAGED_SERVICE_STOPPED &&
        !compositor_service_running() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        !display_service_running() &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL &&
        display_service_surface_count() == 0U &&
        display_service_input_client_count() == 0U &&
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
        baseline_matches(&baseline);
    report("R8 BASIC USERSPACE DISPLAY EXIT GATE", r8_exit_gate);
    if (!r8_exit_gate) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("R8C BOUNDED SURFACE / USERSPACE COMPOSITOR TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    {
        bool cleaned = cleanup_all();
        report("FAILURE CLEANUP / DISPLAY BASELINE", cleaned);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R8C BOUNDED SURFACE / USERSPACE COMPOSITOR TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}