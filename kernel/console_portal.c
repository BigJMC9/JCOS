#include "console_portal.h"

#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "scheduler.h"
#include "task.h"
#include "terminal.h"
#include "../include/console_portal_abi.h"

static void portal_deliver(ConsolePortal *portal, const IpcMessage *message) {
    if (!portal || !message || message->word_count != 2U) return;

    u64 operation = message->words[0];
    u64 argument = message->words[1];

    if (operation == JCOS_CONSOLE_PORTAL_WRITE_BYTE ||
        operation == JCOS_CONSOLE_PORTAL_WRITE_ACCENT_BYTE) {
        if (argument > JCOS_CONSOLE_PORTAL_MAX_BYTE) return;
        if (operation == JCOS_CONSOLE_PORTAL_WRITE_ACCENT_BYTE)
            terminal_set_color(terminal_accent_color());
        terminal_putchar((char)(u8)argument);
        if (operation == JCOS_CONSOLE_PORTAL_WRITE_ACCENT_BYTE)
            terminal_set_color(terminal_default_color());
        ++portal->write_count;
        portal->last_byte = argument;
        return;
    }

    if (argument) return;
    if (operation == JCOS_CONSOLE_PORTAL_CURSOR_LEFT) {
        (void)terminal_cursor_left();
    } else if (operation == JCOS_CONSOLE_PORTAL_SCROLL_PAGE_UP) {
        terminal_scrollback_page_up();
    } else if (operation == JCOS_CONSOLE_PORTAL_SCROLL_PAGE_DOWN) {
        terminal_scrollback_page_down();
    } else if (operation == JCOS_CONSOLE_PORTAL_SCROLL_TO_BOTTOM) {
        terminal_scrollback_to_bottom();
    }
}

static void portal_thread_main(void *argument) {
    ConsolePortal *portal = (ConsolePortal *)argument;
    Process *kernel = process_kernel();
    if (!portal || !kernel) scheduler_exit_current();

    for (;;) {
        IpcMessage message;
        k_memset(&message, 0, sizeof(message));
        if (!ipc_receive_blocking(kernel, portal->kernel_receive_handle, &message)) break;
        portal_deliver(portal, &message);
    }

    scheduler_exit_current();
}

static bool portal_needs_cleanup(const ConsolePortal *portal) {
    return portal && (portal->endpoint_created || portal->thread_created ||
        portal->kernel_receive_cap || portal->kernel_transfer_cap || portal->active ||
        portal->kernel_receive_handle != CAPABILITY_INVALID_HANDLE ||
        portal->kernel_transfer_handle != CAPABILITY_INVALID_HANDLE);
}

static bool portal_drain(ConsolePortal *portal) {
    Process *kernel = process_kernel();
    if (!portal || !kernel || !portal->endpoint_created || !portal->kernel_receive_cap) return false;

    while (endpoint_message_ready(&portal->endpoint)) {
        IpcMessage message;
        k_memset(&message, 0, sizeof(message));
        if (!ipc_try_receive(kernel, portal->kernel_receive_handle, &message)) return false;
        portal_deliver(portal, &message);
    }
    return true;
}

bool console_portal_start(ConsolePortal *portal) {
    if (!portal || portal_needs_cleanup(portal)) return false;
    k_memset(portal, 0, sizeof(*portal));

    Process *kernel = process_kernel();
    Thread *current = thread_current();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !current || current->process != kernel ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || !caps) return false;

    if (!endpoint_create(&portal->endpoint)) goto fail;
    portal->endpoint_created = true;

    if (!capability_insert(caps, &portal->endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &portal->kernel_receive_handle)) goto fail;
    portal->kernel_receive_cap = true;

    if (!capability_insert(caps, &portal->endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &portal->kernel_transfer_handle)) goto fail;
    portal->kernel_transfer_cap = true;

    if (!thread_create(&portal->thread, kernel)) goto fail;
    portal->thread_created = true;
    if (!thread_prepare_kernel(&portal->thread, portal_thread_main, portal)) goto fail;
    if (!scheduler_add(&portal->thread)) goto fail;

    portal->active = true;
    return true;

fail:
    (void)console_portal_stop(portal);
    return false;
}

bool console_portal_stop(ConsolePortal *portal) {
    if (!portal) return false;
    if (!portal_needs_cleanup(portal)) return true;
    if (thread_current() == &portal->thread || portal->thread.state == THREAD_STATE_RUNNING) return false;

    if (portal->thread_created && portal->thread.state != THREAD_STATE_DEAD) {
        if (!task_terminate_thread(&portal->thread)) return false;
    }
    if (portal->thread_created && (portal->thread.on_run_queue || thread_wait_active(&portal->thread))) return false;

    if (portal->endpoint_created && portal->kernel_receive_cap && !portal_drain(portal)) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    if (portal->kernel_receive_cap) {
        if (!capability_revoke(caps, portal->kernel_receive_handle)) return false;
        portal->kernel_receive_cap = false;
        portal->kernel_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (portal->kernel_transfer_cap) {
        if (!capability_revoke(caps, portal->kernel_transfer_handle)) return false;
        portal->kernel_transfer_cap = false;
        portal->kernel_transfer_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (portal->thread_created) {
        if (!thread_destroy(&portal->thread)) return false;
        portal->thread_created = false;
    }
    if (portal->endpoint_created) {
        if (!endpoint_destroy(&portal->endpoint)) return false;
        portal->endpoint_created = false;
    }

    portal->active = false;
    portal->write_count = 0ULL;
    portal->last_byte = 0ULL;
    return true;
}

bool console_portal_grant_spec(const ConsolePortal *portal, ProgramGrantSpec *out) {
    if (!portal || !out || !console_portal_active(portal) ||
        !portal->kernel_transfer_cap) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    k_memset(out, 0, sizeof(*out));
    out->authority_table = caps;
    out->authority_handle = portal->kernel_transfer_handle;
    out->object = (void *)&portal->endpoint;
    out->type = CAPABILITY_TYPE_ENDPOINT;
    out->rights = CAPABILITY_RIGHT_SEND;
    return true;
}

bool console_portal_active(const ConsolePortal *portal) {
    return portal && portal->active && portal->endpoint_created && portal->thread_created &&
        portal->kernel_receive_cap && portal->kernel_transfer_cap &&
        portal->thread.state != THREAD_STATE_DEAD;
}

bool console_portal_idle(const ConsolePortal *portal) {
    return console_portal_active(portal) &&
        portal->thread.state == THREAD_STATE_BLOCKED && !portal->thread.on_run_queue &&
        portal->thread.interrupt_context_ready && portal->thread.interrupt_rsp &&
        endpoint_receiver_waiting(&portal->endpoint);
}

bool console_portal_present(const ConsolePortal *portal) {
    return portal_needs_cleanup(portal);
}

u64 console_portal_write_count(const ConsolePortal *portal) {
    return portal ? portal->write_count : 0ULL;
}

u64 console_portal_last_byte(const ConsolePortal *portal) {
    return portal ? portal->last_byte : 0ULL;
}