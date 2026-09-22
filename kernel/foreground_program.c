#include "foreground_program.h"

#include "arch.h"
#include "audio.h"
#include "display.h"
#include "input.h"
#include "lib.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "serial.h"
#include "system_console.h"
#include "thread.h"
#include "terminal.h"
#include "vfs.h"
#include "../include/console_client_protocol.h"

static ProgramInstance g_foreground_program;
static ProcessExitQueue g_foreground_exit_queue;
static bool g_foreground_exit_queue_created;
static ProcessExitInfo g_last_exit;
static bool g_last_exit_valid;
static u64 g_run_count;

#define FOREGROUND_MEDIA_BASE 0x0000010000000000ULL

typedef struct {
    CapabilityHandle display_authority;
    CapabilityHandle audio_authority;
} MediaAuthorities;

static bool media_grants_begin(ProgramLaunchSpec *spec, MediaAuthorities *authorities) {
    if (!spec || !authorities || !display_available()) return false;
    k_memset(authorities, 0, sizeof(*authorities));
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    DeviceResource *display = display_resource();
    CapabilityRights authority = CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE |
        CAPABILITY_RIGHT_TRANSFER;
    if (!caps || !display ||
        !capability_insert(caps, display, CAPABILITY_TYPE_DEVICE_RESOURCE,
            authority, &authorities->display_authority)) return false;

    spec->startup_grants[2].authority_table = caps;
    spec->startup_grants[2].authority_handle = authorities->display_authority;
    spec->startup_grants[2].object = display;
    spec->startup_grants[2].type = CAPABILITY_TYPE_DEVICE_RESOURCE;
    spec->startup_grants[2].rights = CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE;
    spec->startup_grant_count = 3U;

    /*
     * AC'97 is optional on real hardware. Modern PCs commonly expose HDA
     * instead, which JCOS does not drive yet. Keep video playback available and
     * grant audio only when the current AC'97 backend is actually present.
     */
    if (audio_available()) {
        DeviceResource *audio = audio_resource();
        if (!audio || !capability_insert(caps, audio, CAPABILITY_TYPE_DEVICE_RESOURCE,
                authority, &authorities->audio_authority)) {
            (void)capability_revoke(caps, authorities->display_authority);
            k_memset(authorities, 0, sizeof(*authorities));
            return false;
        }
        spec->startup_grants[3].authority_table = caps;
        spec->startup_grants[3].authority_handle = authorities->audio_authority;
        spec->startup_grants[3].object = audio;
        spec->startup_grants[3].type = CAPABILITY_TYPE_DEVICE_RESOURCE;
        spec->startup_grants[3].rights = CAPABILITY_RIGHT_READ | CAPABILITY_RIGHT_WRITE;
        spec->startup_grant_count = 4U;
    }
    return true;
}

static bool media_grants_end(MediaAuthorities *authorities) {
    if (!authorities) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;
    bool ok = true;
    if (authorities->display_authority &&
        !capability_revoke(caps, authorities->display_authority)) ok = false;
    if (authorities->audio_authority &&
        !capability_revoke(caps, authorities->audio_authority)) ok = false;
    if (ok) k_memset(authorities, 0, sizeof(*authorities));
    return ok;
}

static bool queue_drain(void) {
    if (!g_foreground_exit_queue_created) return true;
    while (process_exit_queue_pending_count(&g_foreground_exit_queue)) {
        ProcessExitInfo info;
        k_memset(&info, 0, sizeof(info));
        if (!process_exit_queue_try_receive(&g_foreground_exit_queue, &info)) return false;
        g_last_exit = info;
        g_last_exit_valid = info.reason != PROCESS_EXIT_NONE;
    }
    return process_exit_queue_watch_count(&g_foreground_exit_queue) == 0U;
}

static bool queue_release(void) {
    if (!g_foreground_exit_queue_created) return true;
    if (!queue_drain()) return false;
    if (!g_foreground_exit_queue.closed && !process_exit_queue_close(&g_foreground_exit_queue)) return false;
    if (!process_exit_queue_destroy(&g_foreground_exit_queue)) return false;
    g_foreground_exit_queue_created = false;
    k_memset(&g_foreground_exit_queue, 0, sizeof(g_foreground_exit_queue));
    return true;
}

bool foreground_program_cleanup(void) {
    bool ok = true;
    if (program_instance_needs_cleanup(&g_foreground_program) &&
        !program_terminate(&g_foreground_program)) ok = false;
    u64 session = system_console_app_session_id();
    if (session && !system_console_app_session_abort(session)) ok = false;
    if (!queue_release()) ok = false;
    audio_session_reset();
    display_session_reset();
    terminal_redraw();
    return ok && !program_instance_needs_cleanup(&g_foreground_program) &&
        !g_foreground_exit_queue_created && !system_console_app_session_id();
}

bool foreground_program_idle(void) {
    return !program_instance_needs_cleanup(&g_foreground_program) &&
        !g_foreground_exit_queue_created && !system_console_app_session_id();
}

static bool foreground_wait(ProgramInstance *program, u64 session_id, bool *cancelled) {
    if (cancelled) *cancelled = false;
    if (!program || !program->thread_created || !session_id) return false;

    for (;;) {
        Thread *thread = &program->thread;
        if (thread->state == THREAD_STATE_DEAD) {
            return !thread->on_run_queue && !thread->interrupt_context_ready &&
                !thread->interrupt_rsp && !thread_wait_active(thread);
        }

        KeyEvent event;
        bool have_event = input_poll(&event);
        if (have_event) {
            if (event.key == KEY_ESCAPE) {
                if (cancelled) *cancelled = true;
                return false;
            }
            if (!system_console_app_input_event(session_id, &event)) return false;
        }

        if (thread->state == THREAD_STATE_READY) {
            if (!thread->on_run_queue || !thread->interrupt_context_ready || !thread->interrupt_rsp ||
                thread_wait_active(thread) || !scheduler_yield()) return false;
        } else if (thread->state == THREAD_STATE_BLOCKED) {
            if (thread->on_run_queue || !thread->interrupt_context_ready || !thread->interrupt_rsp ||
                !thread_wait_active(thread)) return false;
            /* Other service/portal threads may have become runnable after the
             * application's blocking IPC. Yield when possible; otherwise wait
             * for device input or the next interrupt/preemption opportunity. */
            if (!scheduler_yield()) arch_pause();
        } else {
            return false;
        }
    }
}

bool foreground_program_run_pending(void) {
    if (!foreground_program_idle() && !foreground_program_cleanup()) return false;

    SystemConsoleLaunchRequest request;
    k_memset(&request, 0, sizeof(request));
    if (!system_console_launch_request_take(&request) || !request.size) return false;

    const u8 *image_bytes = 0;
    if (!system_console_archive_extent(request.data_offset, request.size, &image_bytes) || !image_bytes) return false;

    if (!process_exit_queue_create(&g_foreground_exit_queue)) return false;
    g_foreground_exit_queue_created = true;

    u64 session_id = 0ULL;
    if (!system_console_app_session_begin(&session_id) || !session_id) goto fail;

    SystemConsoleAppClientGrant grants;
    k_memset(&grants, 0, sizeof(grants));
    if (!system_console_app_client_grant_begin(&grants) || grants.session_id != session_id) goto fail;

    VfsNode file;
    k_memset(&file, 0, sizeof(file));
    file.type = VFS_FILE;
    file.data = image_bytes;
    file.size = request.size;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = &file;
    spec.exit_queue = &g_foreground_exit_queue;
    spec.startup_grant_count = 2U;
    spec.startup_grants[0] = grants.grant;
    spec.startup_grants[1] = grants.input_grant;
    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION;
    spec.startup_arguments[1] = session_id;

    MediaAuthorities media_authorities;
    k_memset(&media_authorities, 0, sizeof(media_authorities));
    if (request.media_size) {
        const u8 *media = 0;
        if (!system_console_archive_extent(request.media_data_offset,
                request.media_size, &media) || !media ||
            !media_grants_begin(&spec, &media_authorities)) goto fail;
        u64 physical = (u64)media;
        u64 page_offset = physical & (VM_PAGE_SIZE - 1ULL);
        spec.readonly_data = media;
        spec.readonly_size = request.media_size;
        spec.readonly_virtual_base = FOREGROUND_MEDIA_BASE;
        spec.startup_argument_count = 4U;
        spec.startup_arguments[2] = FOREGROUND_MEDIA_BASE + page_offset;
        spec.startup_arguments[3] = request.media_size;
        audio_session_reset();
        display_session_reset();
    }

    bool launched = program_launch(&g_foreground_program, &spec);
    bool grants_released = system_console_app_client_grant_end(&grants);
    bool media_grants_released = media_grants_end(&media_authorities);
    if (!launched || !grants_released || !media_grants_released) goto fail;

    u64 pid = g_foreground_program.process.id;
    bool cancelled = false;
    bool dead = foreground_wait(&g_foreground_program, session_id, &cancelled);
    if (!dead) {
        if (cancelled) serial_write("R7 LAUNCH: foreground application cancelled by Escape.\n");
        goto fail;
    }

    if (!system_console_app_session_wait_end(session_id)) goto fail;

    ProcessExitInfo info;
    k_memset(&info, 0, sizeof(info));
    if (!process_exit_queue_try_receive(&g_foreground_exit_queue, &info) ||
        info.reason != PROCESS_EXIT_NORMAL || info.process_id != pid ||
        process_exit_queue_pending_count(&g_foreground_exit_queue) ||
        process_exit_queue_watch_count(&g_foreground_exit_queue)) goto fail;
    g_last_exit = info;
    g_last_exit_valid = true;

    if (!program_reap(&g_foreground_program) || !queue_release()) goto fail;
    audio_session_reset();
    display_session_reset();
    terminal_redraw();
    if (g_run_count != ~0ULL) ++g_run_count;
    return true;

fail:
    (void)foreground_program_cleanup();
    return false;
}

u64 foreground_program_run_count(void) {
    return g_run_count;
}

bool foreground_program_last_exit(ProcessExitInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_last_exit_valid) return false;
    *out = g_last_exit;
    return true;
}
