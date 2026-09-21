#include "userspace_shell.h"

#include "arch.h"
#include "background_service.h"
#include "input.h"
#include "foreground_program.h"
#include "serial.h"
#include "system_console.h"
#include "terminal.h"
#include "timer.h"
#include "../include/console_service_protocol.h"

static u64 g_userspace_shell_sessions;
static u64 g_userspace_shell_boot_handoffs;

static void userspace_shell_cursor_begin(void) {
    terminal_cursor_enable(true);
    terminal_cursor_set_visible(true);
}

static void userspace_shell_cursor_end(void) {
    terminal_cursor_set_visible(false);
    terminal_cursor_enable(false);
}

static bool userspace_shell_recover(void) {
    serial_write("R7 SHELL: userspace console/shell transaction failed; input event not replayed.\n");
    if (!system_console_restart() || !system_console_shell_activate()) return false;
    ++g_userspace_shell_sessions;
    return true;
}

static bool userspace_shell_run_session(bool boot_default) {
    if (!system_console_shell_activate()) return false;

    ++g_userspace_shell_sessions;
    if (boot_default) ++g_userspace_shell_boot_handoffs;

    userspace_shell_cursor_begin();
    u64 blink_started = timer_initialized() ? timer_ticks() : 0ULL;

    for (;;) {
        KeyEvent event;
        if (!input_poll(&event)) {
            if (timer_initialized()) {
                u32 frequency = timer_frequency();
                u64 interval = frequency >= 2U ? (u64)(frequency / 2U) : 1ULL;
                u64 now = timer_ticks();
                if ((u64)(now - blink_started) >= interval) {
                    terminal_cursor_toggle();
                    blink_started = now;
                }
            }
            arch_pause();
            continue;
        }

        terminal_cursor_set_visible(true);
        if (timer_initialized()) blink_started = timer_ticks();

        /* Escape is a kernel-owned emergency return that does not depend on the
         * Ring3 parser being healthy. The service receives a deactivation only
         * to discard any partial normal-shell line. */
        if (event.key == KEY_ESCAPE) {
            userspace_shell_cursor_end();
            if (!system_console_shell_deactivate()) {
                serial_write("R7 SHELL: userspace shell deactivation failed; returning to kernel monitor anyway.\n");
            }
            return true;
        }

        u64 action = JCOS_CONSOLE_SHELL_ACTION_NONE;
        u64 result = JCOS_CONSOLE_SHELL_RESULT_NONE;
        if (!system_console_input_event(&event, &action, &result)) {
            userspace_shell_cursor_end();
            if (!userspace_shell_recover()) return false;
            userspace_shell_cursor_begin();
            if (timer_initialized()) blink_started = timer_ticks();
            continue;
        }

        (void)result;
        if (action == JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR) {
            userspace_shell_cursor_end();
            return true;
        }

        if (action == JCOS_CONSOLE_SHELL_ACTION_RUN_FOREGROUND) {
            userspace_shell_cursor_end();
            if (!foreground_program_run_pending()) {
                serial_write("R7 LAUNCH: foreground extent transaction failed; request/input not replayed.\n");
            }
            if (!system_console_shell_activate()) {
                if (!userspace_shell_recover()) return false;
            }
            userspace_shell_cursor_begin();
            if (timer_initialized()) blink_started = timer_ticks();
            continue;
        }

        if (action == JCOS_CONSOLE_SHELL_ACTION_SERVICE_CONTROL) {
            userspace_shell_cursor_end();
            BackgroundServiceResult service_result;
            if (!background_service_handle_pending(&service_result) ||
                !system_console_service_result(service_result.result, service_result.state,
                    service_result.incarnation)) {
                serial_write("R7 SERVICE: background-service control failed; request not replayed.\n");
                if (!userspace_shell_recover()) return false;
            }
            userspace_shell_cursor_begin();
            if (timer_initialized()) blink_started = timer_ticks();
            continue;
        }

        if (action != JCOS_CONSOLE_SHELL_ACTION_NONE) {
            userspace_shell_cursor_end();
            serial_write("R7 SHELL: unknown userspace shell action; returning to kernel monitor.\n");
            return false;
        }
    }
}

bool userspace_shell_run(void) {
    return userspace_shell_run_session(false);
}

bool userspace_shell_run_boot_default(void) {
    return userspace_shell_run_session(true);
}

u64 userspace_shell_session_count(void) {
    return g_userspace_shell_sessions;
}

u64 userspace_shell_boot_handoff_count(void) {
    return g_userspace_shell_boot_handoffs;
}