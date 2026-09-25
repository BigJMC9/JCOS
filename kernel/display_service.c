#include "display_service.h"

#include "arch.h"
#include "capability.h"
#include "compositor_service.h"
#include "display.h"
#include "framebuffer.h"
#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "vmm.h"
#include "../include/display_service_protocol.h"

#define DISPLAY_SERVICE_PATH "/bin/displayservice.elf"
#define DISPLAY_SERVICE_FB_BASE 0x0000022000000000ULL
#define DISPLAY_SERVICE_WAIT_SECONDS 2ULL

static ManagedService g_display_service;
static DisplaySurface *g_surfaces[JCOS_DISPLAY_SURFACE_CAPACITY];
static DisplayServiceInputClient *g_input_clients[JCOS_DISPLAY_SURFACE_CAPACITY];
static u64 g_next_input_client_id = 1ULL;

static u64 service_header(u64 op) {
    return JCOS_DISPLAY_SERVICE_HEADER(op, JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION);
}

static bool configure_pixel_masks(const FramebufferInfo *info);

static bool surface_slot_valid(u32 slot) {
    return slot < JCOS_DISPLAY_SURFACE_CAPACITY;
}

u32 display_service_surface_count(void) {
    u32 count = 0U;
    for (u32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i)
        if (g_surfaces[i]) ++count;
    return count;
}

u32 display_service_input_client_count(void) {
    u32 count = 0U;
    for (u32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i)
        if (g_input_clients[i]) ++count;
    return count;
}

u64 display_service_input_client_id(const DisplayServiceInputClient *client) {
    return client && client->active ? client->client_id : 0ULL;
}

u64 display_service_input_client_process_id(const DisplayServiceInputClient *client) {
    return client && client->active ? client->process_id : 0ULL;
}

u32 display_service_focus_slot(void) {
    return compositor_service_focus_slot();
}

static bool input_client_registered(const DisplayServiceInputClient *client) {
    return client && client->active && surface_slot_valid(client->slot) &&
        g_input_clients[client->slot] == client;
}

static bool lease_release_after_service(u64 owner_pid) {
    DisplayLeaseState lease = display_direct_state();
    if (lease == DISPLAY_LEASE_KERNEL) return true;
    if (lease == DISPLAY_LEASE_HANDOFF) return display_direct_abort();
    if (lease == DISPLAY_LEASE_USER) return owner_pid && display_direct_reclaim(owner_pid);
    return false;
}

static bool wait_failed(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * DISPLAY_SERVICE_WAIT_SECONDS;
    u64 started = timer_ticks();

    for (;;) {
        ManagedServiceState state = managed_service_state(&g_display_service);
        if (state == MANAGED_SERVICE_FAILED || state == MANAGED_SERVICE_REAP_PENDING) return true;
        if (state != MANAGED_SERVICE_RUNNING || (u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

bool display_service_start(void) {
    if (display_service_surface_count() || display_service_input_client_count() ||
        compositor_service_state() != MANAGED_SERVICE_STOPPED ||
        managed_service_state(&g_display_service) != MANAGED_SERVICE_STOPPED ||
        display_direct_state() != DISPLAY_LEASE_KERNEL) return false;

    ProgramBorrowedMappingSpec mapping;
    FramebufferInfo info;
    u64 user_framebuffer = 0ULL;
    k_memset(&mapping, 0, sizeof(mapping));
    k_memset(&info, 0, sizeof(info));

    if (!display_direct_prepare(DISPLAY_SERVICE_FB_BASE, &mapping, &info, &user_framebuffer))
        return false;

    ManagedServiceSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.path = DISPLAY_SERVICE_PATH;
    spec.shutdown_message = service_header(JCOS_DISPLAY_SERVICE_OP_SHUTDOWN);
    spec.shutdown_reply = JCOS_DISPLAY_SERVICE_REPLY_STOPPED;

    ManagedServiceLaunchExtras extras;
    k_memset(&extras, 0, sizeof(extras));
    extras.startup_argument_count = 3U;
    extras.startup_arguments[0] = user_framebuffer;
    extras.startup_arguments[1] = JCOS_DISPLAY_GEOMETRY(info.width, info.height);
    extras.startup_arguments[2] = JCOS_DISPLAY_FORMAT(info.pixels_per_scanline, info.pixel_format);
    extras.borrowed_mapping = mapping;
    extras.shutdown_request_word_count = 2U;
    extras.shutdown_request_words[0] = spec.shutdown_message;
    extras.shutdown_request_words[1] = 0ULL;

    if (!managed_service_start_ex(&g_display_service, &spec, &extras)) {
        if (managed_service_state(&g_display_service) == MANAGED_SERVICE_STOPPED)
            (void)display_direct_abort();
        return false;
    }

    if (display_direct_commit(&g_display_service.program.process)) {
        if (configure_pixel_masks(&info)) return true;

        u64 owner_pid = g_display_service.program.process.id;
        ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_display_service);
        if (stopped == MANAGED_SERVICE_STOP_GRACEFUL ||
            stopped == MANAGED_SERVICE_STOP_FORCED)
            (void)lease_release_after_service(owner_pid);
        return false;
    }

    u64 owner_pid = g_display_service.program.process.id;
    ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_display_service);
    if (stopped == MANAGED_SERVICE_STOP_GRACEFUL || stopped == MANAGED_SERVICE_STOP_FORCED)
        (void)lease_release_after_service(owner_pid);
    return false;
}

bool display_service_surface_attach_slot(DisplaySurface *surface, u32 slot) {
    if (!surface_slot_valid(slot) || !display_surface_valid(surface) || g_surfaces[slot] ||
        !managed_service_idle(&g_display_service) ||
        !g_display_service.program.process_created) return false;

    for (u32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i)
        if (g_surfaces[i] == surface) return false;

    u64 virtual_base = JCOS_DISPLAY_SURFACE_SLOT_VA(slot);
    /* The service is blocked in RECEIVE here. Its CR3 is inactive while this
     * mapping changes; the next scheduler activation reloads that address space. */
    if (!display_surface_map_reader(surface, &g_display_service.program.process, virtual_base))
        return false;
    g_surfaces[slot] = surface;
    return true;
}

bool display_service_surface_slot_attached(u32 slot) {
    return surface_slot_valid(slot) && g_surfaces[slot] != 0;
}

bool display_service_surface_slot_mapping_valid(u32 slot) {
    return (
            surface_slot_valid(slot) && 
            g_surfaces[slot] &&
            g_display_service.program.process_created &&
            display_surface_reader_mapping_valid(g_surfaces[slot], &g_display_service.program.process, JCOS_DISPLAY_SURFACE_SLOT_VA(slot))
        );
}

bool display_service_surface_attach(DisplaySurface *surface) {
    return display_service_surface_attach_slot(surface, 0U);
}

bool display_service_surface_attached(void) {
    return display_service_surface_slot_attached(0U);
}

bool display_service_surface_mapping_valid(void) {
    return display_service_surface_slot_mapping_valid(0U);
}

static bool wait_idle(void) {
    if (!timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency) return false;
    u64 timeout = (u64)frequency * DISPLAY_SERVICE_WAIT_SECONDS;
    u64 started = timer_ticks();

    for (;;) {
        ManagedServiceState state = managed_service_state(&g_display_service);
        if (state != MANAGED_SERVICE_RUNNING) return false;
        if (managed_service_idle(&g_display_service)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;

        /* A userspace client may consume the service reply and exit before the
         * service itself gets its next turn to re-enter command RECEIVE. That
         * READY interval is normal, not a service failure. Give the service a
         * bounded chance to complete the handoff instead of making the next
         * kernel-side request depend on scheduler ordering. */
        if (!scheduler_yield()) arch_pause();
    }
}

static bool connect(ManagedServiceConnection *connection) {
    return connection && wait_idle() &&
        managed_service_connect_kernel(&g_display_service, connection);
}

static bool configure_pixel_masks(const FramebufferInfo *info) {
    if (!info || info->pixel_format > 2U) return false;
    if (info->pixel_format != 2U) return true;

    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 3U;
    request.words[0] = service_header(JCOS_DISPLAY_SERVICE_OP_SET_PIXEL_MASKS);
    request.words[1] = JCOS_DISPLAY_MASK_PAIR(info->red_mask, info->green_mask);
    request.words[2] = JCOS_DISPLAY_MASK_PAIR(info->blue_mask, info->reserved_mask);

    return managed_service_call(&g_display_service, &connection, &request, &reply) &&
        reply.word_count == 3U && reply.words[0] == JCOS_DISPLAY_SERVICE_REPLY_MASKS_SET &&
        reply.words[1] == connection.incarnation &&
        reply.words[2] == JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION;
}

bool display_service_surface_configure(u32 slot, u32 x, u32 y,
    u32 width, u32 height, u32 z) {
    if (!surface_slot_valid(slot) || !width || !height ||
        !display_service_surface_slot_mapping_valid(slot)) return false;

    FramebufferInfo info;
    k_memset(&info, 0, sizeof(info));
    if (!framebuffer_info(&info) || !info.width || !info.height ||
        x >= info.width || y >= info.height ||
        width > info.width - x || height > info.height - y) return false;

    return compositor_service_surface_configure(slot, x, y, width, height, z);
}

bool display_service_compose(u32 *out_count, u32 *out_sample) {
    return compositor_service_compose(out_count, out_sample);
}

static bool focus_request(u32 slot) {
    if (slot != JCOS_DISPLAY_SURFACE_FOCUS_NONE) {
        if (!surface_slot_valid(slot) || !display_service_surface_slot_mapping_valid(slot))
            return false;
        DisplayServiceInputClient *client = g_input_clients[slot];
        if (!input_client_registered(client) || !client->process_id ||
            display_surface_writer_process_id(client->surface) != client->process_id)
            return false;
    }

    return slot == JCOS_DISPLAY_SURFACE_FOCUS_NONE ?
        compositor_service_focus_clear() : compositor_service_focus_surface(slot);
}

bool display_service_focus_surface(u32 slot) {
    return slot != JCOS_DISPLAY_SURFACE_FOCUS_NONE && focus_request(slot);
}

bool display_service_focus_clear(void) {
    return focus_request(JCOS_DISPLAY_SURFACE_FOCUS_NONE);
}

static u32 input_key_code(KeyCode key) {
    switch (key) {
        case KEY_CHARACTER: return JCOS_DISPLAY_INPUT_KEY_CHARACTER;
        case KEY_ENTER: return JCOS_DISPLAY_INPUT_KEY_ENTER;
        case KEY_BACKSPACE: return JCOS_DISPLAY_INPUT_KEY_BACKSPACE;
        case KEY_TAB: return JCOS_DISPLAY_INPUT_KEY_TAB;
        case KEY_ESCAPE: return JCOS_DISPLAY_INPUT_KEY_ESCAPE;
        case KEY_UP: return JCOS_DISPLAY_INPUT_KEY_UP;
        case KEY_DOWN: return JCOS_DISPLAY_INPUT_KEY_DOWN;
        case KEY_LEFT: return JCOS_DISPLAY_INPUT_KEY_LEFT;
        case KEY_RIGHT: return JCOS_DISPLAY_INPUT_KEY_RIGHT;
        case KEY_HOME: return JCOS_DISPLAY_INPUT_KEY_HOME;
        case KEY_END: return JCOS_DISPLAY_INPUT_KEY_END;
        case KEY_DELETE: return JCOS_DISPLAY_INPUT_KEY_DELETE;
        case KEY_PAGE_UP: return JCOS_DISPLAY_INPUT_KEY_PAGE_UP;
        case KEY_PAGE_DOWN: return JCOS_DISPLAY_INPUT_KEY_PAGE_DOWN;
        default: return JCOS_DISPLAY_INPUT_KEY_NONE;
    }
}

bool display_service_input_event(const KeyEvent *event) {
    u32 focused_slot = display_service_focus_slot();
    if (!event || focused_slot == JCOS_DISPLAY_SURFACE_FOCUS_NONE ||
        !surface_slot_valid(focused_slot)) return false;

    u32 key = input_key_code(event->key);
    DisplayServiceInputClient *client = g_input_clients[focused_slot];
    if (!key || !input_client_registered(client) || !client->process_id ||
        !client->kernel_send_cap || client->kernel_send_handle == CAPABILITY_INVALID_HANDLE ||
        display_surface_writer_process_id(client->surface) != client->process_id)
        return false;

    Process *kernel = process_kernel();
    if (!kernel) return false;

    u64 flags = 0ULL;
    if (event->pressed) flags |= JCOS_DISPLAY_INPUT_FLAG_PRESSED;
    if (event->shift) flags |= JCOS_DISPLAY_INPUT_FLAG_SHIFT;
    if (event->ctrl) flags |= JCOS_DISPLAY_INPUT_FLAG_CTRL;
    if (event->alt) flags |= JCOS_DISPLAY_INPUT_FLAG_ALT;

    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = 4U;
    message.words[0] = JCOS_DISPLAY_CLIENT_HEADER(
        JCOS_DISPLAY_CLIENT_OP_KEY_EVENT, JCOS_DISPLAY_CLIENT_PROTOCOL_VERSION);
    message.words[1] = client->client_id;
    message.words[2] = JCOS_DISPLAY_INPUT_EVENT(key, event->character);
    message.words[3] = flags;
    return ipc_try_send(kernel, client->kernel_send_handle, &message);
}

static bool input_client_rollback(DisplayServiceInputClient *client) {
    if (!client) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    bool ok = caps != 0;

    if (client->receive_authority_cap) {
        if (caps && capability_revoke(caps, client->receive_authority_handle)) {
            client->receive_authority_cap = false;
            client->receive_authority_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }
    if (client->kernel_send_cap) {
        if (caps && capability_revoke(caps, client->kernel_send_handle)) {
            client->kernel_send_cap = false;
            client->kernel_send_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }
    if (client->endpoint_created) {
        if (!endpoint_closed(&client->input_endpoint) &&
            !ipc_endpoint_close(&client->input_endpoint)) ok = false;
        if (ok && endpoint_destroy(&client->input_endpoint)) {
            client->endpoint_created = false;
        } else if (client->endpoint_created) ok = false;
    }
    if (ok) k_memset(client, 0, sizeof(*client));
    return ok;
}

bool display_service_input_client_begin(DisplayServiceInputClient *client,
    DisplaySurface *surface, u32 slot) {
    if (!client || endpoint_storage_in_use(&client->input_endpoint) ||
        !surface_slot_valid(slot) || !display_surface_valid(surface) ||
        display_surface_writer_process_id(surface) ||
        g_input_clients[slot] || g_surfaces[slot] != surface ||
        !display_service_surface_slot_mapping_valid(slot) ||
        !managed_service_idle(&g_display_service) || !g_next_input_client_id)
        return false;

    k_memset(client, 0, sizeof(*client));
    client->kernel_send_handle = CAPABILITY_INVALID_HANDLE;
    client->receive_authority_handle = CAPABILITY_INVALID_HANDLE;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !endpoint_create(&client->input_endpoint)) return false;
    client->endpoint_created = true;

    if (!capability_insert(caps, &client->input_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &client->kernel_send_handle)) goto fail;
    client->kernel_send_cap = true;
    if (!capability_insert(caps, &client->input_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &client->receive_authority_handle)) goto fail;
    client->receive_authority_cap = true;

    client->input_grant.authority_table = caps;
    client->input_grant.authority_handle = client->receive_authority_handle;
    client->input_grant.object = &client->input_endpoint;
    client->input_grant.type = CAPABILITY_TYPE_ENDPOINT;
    client->input_grant.rights = CAPABILITY_RIGHT_RECEIVE;
    client->surface = surface;
    client->slot = slot;
    client->client_id = g_next_input_client_id++;
    client->active = client->client_id != 0ULL;
    if (!client->active) goto fail;
    g_input_clients[slot] = client;
    return true;

fail:
    (void)input_client_rollback(client);
    return false;
}

bool display_service_input_client_bind(DisplayServiceInputClient *client, Process *process) {
    if (!input_client_registered(client) || client->process_id || !process ||
        !process_storage_in_use(process) || !process->initialized || process->kernel || !process->id ||
        display_surface_writer_process_id(client->surface) != process->id ||
        !client->receive_authority_cap ||
        client->receive_authority_handle == CAPABILITY_INVALID_HANDLE)
        return false;

    CapabilityTable *child_caps = process_capabilities(process);
    bool child_receive = false;
    if (!child_caps) return false;
    for (u32 i = 0U; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        CapabilitySlot *slot = &child_caps->slots[i];
        if (!slot->occupied || slot->object != &client->input_endpoint ||
            slot->type != CAPABILITY_TYPE_ENDPOINT) continue;
        if (slot->rights != CAPABILITY_RIGHT_RECEIVE || child_receive) return false;
        child_receive = true;
    }
    if (!child_receive) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_revoke(caps, client->receive_authority_handle)) return false;
    client->receive_authority_cap = false;
    client->receive_authority_handle = CAPABILITY_INVALID_HANDLE;
    client->input_grant.authority_table = 0;
    client->input_grant.authority_handle = CAPABILITY_INVALID_HANDLE;
    client->process_id = process->id;
    return true;
}

bool display_service_input_client_end(DisplayServiceInputClient *client) {
    if (!client) return false;
    if (!input_client_registered(client)) {
        if (!client->endpoint_created && !client->kernel_send_cap &&
            !client->receive_authority_cap) return true;
        return input_client_rollback(client);
    }
    if (display_surface_writer_process_id(client->surface)) return false;

    u64 expected_refs = (client->kernel_send_cap ? 1ULL : 0ULL) +
        (client->receive_authority_cap ? 1ULL : 0ULL);
    if (client->input_endpoint.capability_refs != expected_refs) return false;

    if (display_service_focus_slot() == client->slot && compositor_service_running() &&
        !display_service_focus_clear()) return false;

    u32 slot = client->slot;
    g_input_clients[slot] = 0;
    if (input_client_rollback(client)) return true;
    g_input_clients[slot] = client;
    return false;
}

bool display_service_surface_remove(u32 slot) {
    return surface_slot_valid(slot) && g_surfaces[slot] && compositor_service_running() &&
        compositor_service_surface_remove(slot);
}

bool display_service_surface_detach_slot(u32 slot) {
    if (!surface_slot_valid(slot) || g_input_clients[slot]) return false;
    DisplaySurface *surface = g_surfaces[slot];
    if (!surface) return true;
    if (!g_display_service.program.process_created) return false;

    ManagedServiceState state = managed_service_state(&g_display_service);
    if (state == MANAGED_SERVICE_RUNNING && compositor_service_running()) {
        if (!managed_service_idle(&g_display_service) || !display_service_surface_remove(slot))
            return false;
    } else if (state == MANAGED_SERVICE_RUNNING &&
               compositor_service_state() != MANAGED_SERVICE_STOPPED) {
        return false;
    }

    if (!display_surface_unmap_reader(surface, &g_display_service.program.process,
            JCOS_DISPLAY_SURFACE_SLOT_VA(slot))) return false;
    g_surfaces[slot] = 0;
    return true;
}

bool display_service_surface_detach(void) {
    return display_service_surface_detach_slot(0U);
}

static bool display_service_surface_detach_all(void) {
    for (u32 i = JCOS_DISPLAY_SURFACE_CAPACITY; i > 0U; --i)
        if (!display_service_surface_detach_slot(i - 1U)) return false;
    return true;
}

bool display_service_client_grant_begin(DisplayServiceClientGrant *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!display_service_surface_mapping_valid() || !managed_service_idle(&g_display_service))
        return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_insert(caps, &g_display_service.command_endpoint,
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &out->command_authority_handle)) return false;

    if (!capability_insert(caps, &g_display_service.reply_endpoint,
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &out->reply_authority_handle)) {
        (void)capability_revoke(caps, out->command_authority_handle);
        k_memset(out, 0, sizeof(*out));
        return false;
    }

    out->command_grant.authority_table = caps;
    out->command_grant.authority_handle = out->command_authority_handle;
    out->command_grant.object = &g_display_service.command_endpoint;
    out->command_grant.type = CAPABILITY_TYPE_ENDPOINT;
    out->command_grant.rights = CAPABILITY_RIGHT_SEND;

    out->reply_grant.authority_table = caps;
    out->reply_grant.authority_handle = out->reply_authority_handle;
    out->reply_grant.object = &g_display_service.reply_endpoint;
    out->reply_grant.type = CAPABILITY_TYPE_ENDPOINT;
    out->reply_grant.rights = CAPABILITY_RIGHT_RECEIVE;

    out->incarnation = managed_service_incarnation(&g_display_service);
    out->active = out->incarnation != 0ULL;
    if (out->active) return true;

    (void)capability_revoke(caps, out->reply_authority_handle);
    (void)capability_revoke(caps, out->command_authority_handle);
    k_memset(out, 0, sizeof(*out));
    return false;
}

bool display_service_client_grant_end(DisplayServiceClientGrant *grant) {
    if (!grant) return false;
    if (!grant->active) return true;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    bool ok = true;
    if (grant->command_authority_handle != CAPABILITY_INVALID_HANDLE) {
        if (capability_revoke(caps, grant->command_authority_handle)) {
            grant->command_authority_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }
    if (grant->reply_authority_handle != CAPABILITY_INVALID_HANDLE) {
        if (capability_revoke(caps, grant->reply_authority_handle)) {
            grant->reply_authority_handle = CAPABILITY_INVALID_HANDLE;
        } else ok = false;
    }

    if (ok) k_memset(grant, 0, sizeof(*grant));
    return ok;
}

bool display_service_stop(void) {
    if (display_service_input_client_count() ||
        compositor_service_state() != MANAGED_SERVICE_STOPPED) return false;
    ManagedServiceState state = managed_service_state(&g_display_service);
    if (state == MANAGED_SERVICE_STOPPED) {
        if (display_service_surface_count()) return false;
        return lease_release_after_service(display_direct_owner_process_id());
    }
    if (state != MANAGED_SERVICE_RUNNING || !display_service_surface_detach_all()) return false;

    u64 owner_pid = g_display_service.program.process.id;
    ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_display_service);
    if (stopped != MANAGED_SERVICE_STOP_GRACEFUL && stopped != MANAGED_SERVICE_STOP_FORCED)
        return false;
    return lease_release_after_service(owner_pid);
}

bool display_service_recover(void) {
    if (display_service_input_client_count() ||
        compositor_service_state() != MANAGED_SERVICE_STOPPED) return false;
    ManagedServiceState state = managed_service_state(&g_display_service);
    u64 owner_pid = g_display_service.program.process_created ?
        g_display_service.program.process.id : display_direct_owner_process_id();

    if (!display_service_surface_detach_all()) return false;
    if (state != MANAGED_SERVICE_STOPPED && !managed_service_recover(&g_display_service))
        return false;
    return lease_release_after_service(owner_pid);
}

bool display_service_cleanup(void) {
    ManagedServiceState state = managed_service_state(&g_display_service);
    if (state == MANAGED_SERVICE_RUNNING) return display_service_stop();
    return display_service_recover();
}

ManagedServiceState display_service_state(void) {
    return managed_service_state(&g_display_service);
}

bool display_service_running(void) {
    return managed_service_running(&g_display_service);
}

u64 display_service_incarnation(void) {
    return managed_service_incarnation(&g_display_service);
}

u64 display_service_process_id(void) {
    return managed_service_process_id(&g_display_service);
}

u64 display_service_thread_id(void) {
    return managed_service_thread_id(&g_display_service);
}

bool display_service_ping(u64 cookie, u64 *out_cookie) {
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
    request.words[0] = service_header(JCOS_DISPLAY_SERVICE_OP_PING);
    request.words[1] = 0ULL;
    request.words[2] = cookie;

    if (!managed_service_call(&g_display_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_DISPLAY_SERVICE_REPLY_PONG ||
        reply.words[1] != connection.incarnation ||
        reply.words[2] != JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION || reply.words[3] != cookie)
        return false;

    *out_cookie = reply.words[3];
    return true;
}

bool display_service_redraw(void) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    IpcMessage reply;
    k_memset(&request, 0, sizeof(request));
    k_memset(&reply, 0, sizeof(reply));
    request.word_count = 2U;
    request.words[0] = service_header(JCOS_DISPLAY_SERVICE_OP_REDRAW);
    request.words[1] = 0ULL;

    return managed_service_call(&g_display_service, &connection, &request, &reply) &&
        reply.word_count == 3U && reply.words[0] == JCOS_DISPLAY_SERVICE_REPLY_DRAWN &&
        reply.words[1] == connection.incarnation &&
        reply.words[2] == JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION;
}

bool display_service_fault(void) {
    ManagedServiceConnection connection;
    k_memset(&connection, 0, sizeof(connection));
    if (!connect(&connection)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = service_header(JCOS_DISPLAY_SERVICE_OP_DIAG_FAULT);
    request.words[1] = 0ULL;

    return managed_service_send_oneway(&g_display_service, &connection, &request) && wait_failed();
}

bool display_service_last_exit_info(ProcessExitInfo *out) {
    return managed_service_last_exit_info(&g_display_service, out);
}