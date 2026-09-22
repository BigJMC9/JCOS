#include "lib/syscall.h"
#include "lib/boot_archive.h"
#include "../include/console_portal_abi.h"
#include "../include/console_client_protocol.h"
#include "../include/console_service_protocol.h"
#include "../include/program_startup.h"
#include "../include/program_broker_protocol.h"
#include "../include/service_broker_protocol.h"

#define SHELL_LINE_CAPACITY    128U
#define SHELL_HISTORY_CAPACITY 64U
#define SHELL_STATE_INACTIVE   0
#define SHELL_STATE_ACTIVE     1
#define SHELL_STATE_SUSPENDED  2

typedef struct {
    char line[SHELL_LINE_CAPACITY];
    JcosU32 length;
    JcosU32 cursor;
    JcosU32 rendered_length;
    JcosU32 rendered_cursor;

    char history[SHELL_HISTORY_CAPACITY][SHELL_LINE_CAPACITY];
    JcosU32 history_count;
    JcosU32 history_next;
    int history_offset;

    char draft[SHELL_LINE_CAPACITY];
    JcosU32 draft_length;
    int draft_saved;

    int selection_active;
    JcosU32 selection_anchor;

    char clipboard[SHELL_LINE_CAPACITY];
    JcosU32 clipboard_length;

    int active;
    JcosU64 pending_service_op;
    int worker_replacement;
    int pending_service_replacement;
} ShellState;

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_valid(const JcosProgramStartup *startup) {
    if (!startup || startup->magic != JCOS_PROGRAM_STARTUP_MAGIC ||
        startup->version != JCOS_PROGRAM_STARTUP_VERSION ||
        startup->size != sizeof(JcosProgramStartup) ||
        startup->flags != JCOS_PROGRAM_STARTUP_FLAG_NONE ||
        (startup->capability_count != 3U && startup->capability_count != 4U &&
         startup->capability_count != 6U && startup->capability_count != 7U &&
         startup->capability_count != 8U) ||
        startup->argument_count != 2U || startup->environment_count != 0U ||
        !startup->arguments[0] || startup->arguments[1] != JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION ||
        startup->capabilities[0] == JCOS_CAPABILITY_INVALID_HANDLE ||
        startup->capabilities[1] == JCOS_CAPABILITY_INVALID_HANDLE ||
        startup->capabilities[2] == JCOS_CAPABILITY_INVALID_HANDLE) return 0;

    if (startup->capability_count >= 4U &&
        startup->capabilities[3] == JCOS_CAPABILITY_INVALID_HANDLE) return 0;
    if (startup->capability_count >= 6U &&
        (startup->capabilities[4] == JCOS_CAPABILITY_INVALID_HANDLE ||
         startup->capabilities[5] == JCOS_CAPABILITY_INVALID_HANDLE)) return 0;
    if (startup->capability_count >= 7U &&
        startup->capabilities[6] == JCOS_CAPABILITY_INVALID_HANDLE) return 0;
    if (startup->capability_count == 8U &&
        startup->capabilities[7] == JCOS_CAPABILITY_INVALID_HANDLE) return 0;
    return 1;
}

static int send_reply(JcosCapabilityHandle reply_cap, JcosU64 code, JcosU64 incarnation,
    JcosU64 version, JcosU64 detail, JcosU32 words) {
    JcosIpcMessage reply;
    clear_message(&reply);
    reply.word_count = words;
    reply.words[0] = code;
    reply.words[1] = incarnation;
    reply.words[2] = version;
    reply.words[3] = detail;
    return jcos_ipc_send_blocking(reply_cap, &reply);
}

static int portal_command(JcosCapabilityHandle portal_cap, JcosU64 operation, JcosU64 argument) {
    JcosIpcMessage message;
    clear_message(&message);
    message.word_count = 2U;
    message.words[0] = operation;
    message.words[1] = argument;
    return jcos_ipc_send_blocking(portal_cap, &message);
}

static int portal_write_byte(JcosCapabilityHandle portal_cap, JcosU64 value) {
    return value <= JCOS_CONSOLE_PORTAL_MAX_BYTE &&
        portal_command(portal_cap, JCOS_CONSOLE_PORTAL_WRITE_BYTE, value);
}

static int portal_write_accent_byte(JcosCapabilityHandle portal_cap, JcosU64 value) {
    return value <= JCOS_CONSOLE_PORTAL_MAX_BYTE &&
        portal_command(portal_cap, JCOS_CONSOLE_PORTAL_WRITE_ACCENT_BYTE, value);
}

static int portal_cursor_left(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_CURSOR_LEFT, 0ULL);
}

static int portal_scrollback_line_up(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_SCROLL_LINE_UP, 0ULL);
}

static int portal_scrollback_line_down(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_SCROLL_LINE_DOWN, 0ULL);
}

static int portal_scrollback_page_up(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_SCROLL_PAGE_UP, 0ULL);
}

static int portal_scrollback_page_down(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_SCROLL_PAGE_DOWN, 0ULL);
}

static int portal_scrollback_to_bottom(JcosCapabilityHandle portal_cap) {
    return portal_command(portal_cap, JCOS_CONSOLE_PORTAL_SCROLL_TO_BOTTOM, 0ULL);
}

static int portal_write(JcosCapabilityHandle portal_cap, const char *text) {
    if (!text) return 0;
    while (*text) {
        if (!portal_write_byte(portal_cap, (JcosU64)(unsigned char)*text++)) return 0;
    }
    return 1;
}

static int text_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static const char *skip_spaces(const char *text) {
    if (!text) return "";
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

static int command_prefix(const char *line, const char *command, const char **rest) {
    if (rest) *rest = line;
    if (!line || !command) return 0;
    const char *a = line;
    const char *b = command;
    while (*b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    if (*a && *a != ' ' && *a != '\t') return 0;
    if (rest) *rest = skip_spaces(a);
    return 1;
}

static void copy_line(char *dst, const char *src, JcosU32 capacity) {
    if (!dst || !capacity) return;
    JcosU32 i = 0U;
    if (src) {
        while (i + 1U < capacity && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = 0;
}

static void shell_init(ShellState *shell) {
    if (!shell) return;
    shell->line[0] = 0;
    shell->length = 0U;
    shell->cursor = 0U;
    shell->rendered_length = 0U;
    shell->rendered_cursor = 0U;
    shell->history_count = 0U;
    shell->history_next = 0U;
    shell->history_offset = -1;
    shell->draft[0] = 0;
    shell->draft_length = 0U;
    shell->draft_saved = 0;
    shell->selection_active = 0;
    shell->selection_anchor = 0U;
    shell->clipboard[0] = 0;
    shell->clipboard_length = 0U;
    shell->active = SHELL_STATE_INACTIVE;
    shell->pending_service_op = 0ULL;
    shell->worker_replacement = 0;
    shell->pending_service_replacement = 0;
}

static void shell_clear_selection(ShellState *shell) {
    if (!shell) return;
    shell->selection_active = 0;
    shell->selection_anchor = shell->cursor;
}

static void shell_reset_line(ShellState *shell) {
    if (!shell) return;
    shell->line[0] = 0;
    shell->length = 0U;
    shell->cursor = 0U;
    shell->rendered_length = 0U;
    shell->rendered_cursor = 0U;
    shell->history_offset = -1;
    shell->draft[0] = 0;
    shell->draft_length = 0U;
    shell->draft_saved = 0;
    shell_clear_selection(shell);
}

static int shell_prompt(JcosCapabilityHandle portal_cap) {
    return portal_write(portal_cap, "JA:user> ");
}

static int shell_activate(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;
    shell_reset_line(shell);
    if (shell->active == SHELL_STATE_INACTIVE) {
        if (!portal_write(portal_cap,
                "\nJA OS USERSPACE SHELL R7A.3\n"
                "TYPE help FOR COMMANDS. TYPE monitor TO RETURN TO THE KERNEL MONITOR.\n")) return 0;
    }
    shell->active = SHELL_STATE_ACTIVE;
    return shell_prompt(portal_cap);
}

static int shell_deactivate(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;
    if (shell->active == SHELL_STATE_ACTIVE && !portal_write_byte(portal_cap, '\n')) return 0;
    shell->active = SHELL_STATE_INACTIVE;
    shell->pending_service_op = 0ULL;
    shell->pending_service_replacement = shell->worker_replacement;
    shell_reset_line(shell);
    return 1;
}

static JcosU32 shell_selection_start(const ShellState *shell) {
    return shell->cursor < shell->selection_anchor ? shell->cursor : shell->selection_anchor;
}

static JcosU32 shell_selection_end(const ShellState *shell) {
    return shell->cursor > shell->selection_anchor ? shell->cursor : shell->selection_anchor;
}

static int shell_selected_at(const ShellState *shell, JcosU32 index) {
    if (!shell || !shell->selection_active) return 0;
    JcosU32 start = shell_selection_start(shell);
    JcosU32 end = shell_selection_end(shell);
    return index >= start && index < end;
}

static int shell_redraw(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;

    while (shell->rendered_cursor) {
        if (!portal_cursor_left(portal_cap)) return 0;
        --shell->rendered_cursor;
    }

    for (JcosU32 i = 0U; i < shell->length; ++i) {
        int ok = shell_selected_at(shell, i) ?
            portal_write_accent_byte(portal_cap, (JcosU64)(unsigned char)shell->line[i]) :
            portal_write_byte(portal_cap, (JcosU64)(unsigned char)shell->line[i]);
        if (!ok) return 0;
    }

    JcosU32 display_length = shell->rendered_length;
    if (shell->length > display_length) display_length = shell->length;
    for (JcosU32 i = shell->length; i < display_length; ++i) {
        if (!portal_write_byte(portal_cap, ' ')) return 0;
    }

    shell->rendered_cursor = display_length;
    while (shell->rendered_cursor > shell->cursor) {
        if (!portal_cursor_left(portal_cap)) return 0;
        --shell->rendered_cursor;
    }

    shell->rendered_length = shell->length;
    shell->rendered_cursor = shell->cursor;
    return 1;
}

static void shell_leave_history(ShellState *shell) {
    if (!shell) return;
    shell->history_offset = -1;
    shell->draft_saved = 0;
}

static void shell_history_store(ShellState *shell) {
    if (!shell || !shell->length) return;

    if (shell->history_count) {
        JcosU32 newest = (shell->history_next + SHELL_HISTORY_CAPACITY - 1U) % SHELL_HISTORY_CAPACITY;
        if (text_equal(shell->history[newest], shell->line)) return;
    }

    copy_line(shell->history[shell->history_next], shell->line, SHELL_LINE_CAPACITY);
    shell->history_next = (shell->history_next + 1U) % SHELL_HISTORY_CAPACITY;
    if (shell->history_count < SHELL_HISTORY_CAPACITY) ++shell->history_count;
}

static JcosU32 shell_history_index(const ShellState *shell, JcosU32 offset) {
    return (shell->history_next + SHELL_HISTORY_CAPACITY - 1U - offset) % SHELL_HISTORY_CAPACITY;
}

static int shell_history_load(ShellState *shell, JcosCapabilityHandle portal_cap, JcosU32 offset) {
    if (!shell || offset >= shell->history_count) return 1;
    copy_line(shell->line, shell->history[shell_history_index(shell, offset)], SHELL_LINE_CAPACITY);
    shell->length = 0U;
    while (shell->line[shell->length]) ++shell->length;
    shell->cursor = shell->length;
    shell_clear_selection(shell);
    return shell_redraw(shell, portal_cap);
}

static int shell_history_up(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell || !shell->history_count) return 1;
    if (shell->history_offset < 0) {
        copy_line(shell->draft, shell->line, SHELL_LINE_CAPACITY);
        shell->draft_length = shell->length;
        shell->draft_saved = 1;
        shell->history_offset = 0;
    } else if ((JcosU32)(shell->history_offset + 1) < shell->history_count) {
        ++shell->history_offset;
    }
    return shell_history_load(shell, portal_cap, (JcosU32)shell->history_offset);
}

static int shell_history_down(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell || shell->history_offset < 0) return 1;
    if (shell->history_offset > 0) {
        --shell->history_offset;
        return shell_history_load(shell, portal_cap, (JcosU32)shell->history_offset);
    }

    shell->history_offset = -1;
    if (shell->draft_saved) {
        copy_line(shell->line, shell->draft, SHELL_LINE_CAPACITY);
        shell->length = shell->draft_length;
    } else {
        shell->line[0] = 0;
        shell->length = 0U;
    }
    shell->cursor = shell->length;
    shell->draft_saved = 0;
    shell_clear_selection(shell);
    return shell_redraw(shell, portal_cap);
}

static void shell_delete_range(ShellState *shell, JcosU32 start, JcosU32 end) {
    if (!shell || start >= end || end > shell->length) return;
    JcosU32 count = end - start;
    for (JcosU32 i = start; i + count <= shell->length; ++i)
        shell->line[i] = shell->line[i + count];
    shell->length -= count;
    shell->cursor = start;
    shell_clear_selection(shell);
    shell_leave_history(shell);
}

static int shell_delete_selection(ShellState *shell) {
    if (!shell || !shell->selection_active) return 0;
    JcosU32 start = shell_selection_start(shell);
    JcosU32 end = shell_selection_end(shell);
    shell_delete_range(shell, start, end);
    return 1;
}

static int shell_insert_bytes(ShellState *shell, JcosCapabilityHandle portal_cap,
    const char *bytes, JcosU32 count) {
    if (!shell || !bytes || !count) return 1;

    if (shell->selection_active) (void)shell_delete_selection(shell);

    JcosU32 available = SHELL_LINE_CAPACITY - 1U - shell->length;
    if (count > available) count = available;
    if (!count) return 1;

    int append = shell->cursor == shell->length;
    for (JcosU32 i = shell->length; i > shell->cursor; --i)
        shell->line[i + count - 1U] = shell->line[i - 1U];
    for (JcosU32 i = 0U; i < count; ++i) shell->line[shell->cursor + i] = bytes[i];
    shell->cursor += count;
    shell->length += count;
    shell->line[shell->length] = 0;
    shell_clear_selection(shell);
    shell_leave_history(shell);

    if (append && shell->rendered_cursor == shell->rendered_length &&
        shell->rendered_length + count == shell->length) {
        for (JcosU32 i = 0U; i < count; ++i) {
            if (!portal_write_byte(portal_cap, (JcosU64)(unsigned char)bytes[i])) return 0;
        }
        shell->rendered_length = shell->length;
        shell->rendered_cursor = shell->cursor;
        return 1;
    }
    return shell_redraw(shell, portal_cap);
}

static int shell_insert_character(ShellState *shell, JcosCapabilityHandle portal_cap, char c) {
    if ((unsigned char)c < 32U || (unsigned char)c > 126U) return 1;
    return shell_insert_bytes(shell, portal_cap, &c, 1U);
}

static int shell_erase_before(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;
    if (shell->selection_active) {
        (void)shell_delete_selection(shell);
        return shell_redraw(shell, portal_cap);
    }
    if (!shell->cursor) return 1;

    if (shell->cursor == shell->length && shell->rendered_cursor == shell->rendered_length) {
        --shell->cursor;
        --shell->length;
        shell->line[shell->length] = 0;
        shell->rendered_cursor = shell->cursor;
        shell->rendered_length = shell->length;
        shell_leave_history(shell);
        return portal_write_byte(portal_cap, '\b');
    }

    JcosU32 target = shell->cursor - 1U;
    shell_delete_range(shell, target, shell->cursor);
    return shell_redraw(shell, portal_cap);
}

static int shell_erase_at(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;
    if (shell->selection_active) {
        (void)shell_delete_selection(shell);
        return shell_redraw(shell, portal_cap);
    }
    if (shell->cursor >= shell->length) return 1;
    shell_delete_range(shell, shell->cursor, shell->cursor + 1U);
    return shell_redraw(shell, portal_cap);
}

static void shell_selection_begin_if_needed(ShellState *shell) {
    if (!shell || shell->selection_active) return;
    shell->selection_anchor = shell->cursor;
}

static void shell_selection_finish_move(ShellState *shell) {
    if (!shell) return;
    shell->selection_active = shell->cursor != shell->selection_anchor;
}

static int shell_move_left(ShellState *shell, JcosCapabilityHandle portal_cap, int selecting) {
    if (!shell || !shell->cursor) return 1;
    if (selecting) shell_selection_begin_if_needed(shell);
    else shell_clear_selection(shell);
    --shell->cursor;
    if (selecting) shell_selection_finish_move(shell);
    return shell_redraw(shell, portal_cap);
}

static int shell_move_right(ShellState *shell, JcosCapabilityHandle portal_cap, int selecting) {
    if (!shell || shell->cursor >= shell->length) return 1;
    if (selecting) shell_selection_begin_if_needed(shell);
    else shell_clear_selection(shell);
    ++shell->cursor;
    if (selecting) shell_selection_finish_move(shell);
    return shell_redraw(shell, portal_cap);
}

static int shell_move_home(ShellState *shell, JcosCapabilityHandle portal_cap, int selecting) {
    if (!shell) return 0;
    if (selecting) shell_selection_begin_if_needed(shell);
    else shell_clear_selection(shell);
    shell->cursor = 0U;
    if (selecting) shell_selection_finish_move(shell);
    return shell_redraw(shell, portal_cap);
}

static int shell_move_end(ShellState *shell, JcosCapabilityHandle portal_cap, int selecting) {
    if (!shell) return 0;
    if (selecting) shell_selection_begin_if_needed(shell);
    else shell_clear_selection(shell);
    shell->cursor = shell->length;
    if (selecting) shell_selection_finish_move(shell);
    return shell_redraw(shell, portal_cap);
}

static void shell_copy_selection_or_line(ShellState *shell) {
    if (!shell) return;
    JcosU32 start = shell->selection_active ? shell_selection_start(shell) : 0U;
    JcosU32 end = shell->selection_active ? shell_selection_end(shell) : shell->length;
    JcosU32 count = end - start;
    if (count >= SHELL_LINE_CAPACITY) count = SHELL_LINE_CAPACITY - 1U;
    for (JcosU32 i = 0U; i < count; ++i) shell->clipboard[i] = shell->line[start + i];
    shell->clipboard[count] = 0;
    shell->clipboard_length = count;
}

static int shell_cut_selection_or_line(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell) return 0;
    shell_copy_selection_or_line(shell);
    if (shell->selection_active) {
        (void)shell_delete_selection(shell);
    } else {
        shell->line[0] = 0;
        shell->length = 0U;
        shell->cursor = 0U;
        shell_clear_selection(shell);
        shell_leave_history(shell);
    }
    return shell_redraw(shell, portal_cap);
}

static int shell_paste_clipboard(ShellState *shell, JcosCapabilityHandle portal_cap) {
    if (!shell || !shell->clipboard_length) return 1;
    return shell_insert_bytes(shell, portal_cap, shell->clipboard, shell->clipboard_length);
}

static char shell_ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static JcosU32 text_length(const char *text) {
    JcosU32 length = 0U;
    if (!text) return 0U;
    while (text[length]) ++length;
    return length;
}

static int portal_write_u64(JcosCapabilityHandle portal_cap, JcosU64 value) {
    char buffer[21];
    JcosU32 count = 0U;
    if (!value) return portal_write_byte(portal_cap, '0');
    while (value && count < 20U) {
        buffer[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }
    while (count) {
        if (!portal_write_byte(portal_cap, (JcosU64)(unsigned char)buffer[--count])) return 0;
    }
    return 1;
}

static int archive_available(JcosBootArchive *archive) {
    return archive && jcos_boot_archive_probe(archive) && archive->ready && archive->size;
}

static int archive_list(JcosBootArchive *archive, JcosCapabilityHandle portal_cap) {
    if (!archive_available(archive)) {
        (void)portal_write(portal_cap, "ls: boot archive unavailable\n");
        return 0;
    }

    JcosU64 cursor = 0ULL;
    JcosBootArchiveEntry entry;
    int any = 0;
    for (;;) {
        int result = jcos_boot_archive_next(archive, &cursor, &entry);
        if (result < 0) {
            (void)portal_write(portal_cap, "ls: malformed boot archive\n");
            return 0;
        }
        if (!result) break;
        if (!portal_write(portal_cap, entry.path)) return 0;
        if (entry.type == JCOS_BOOT_ARCHIVE_ENTRY_DIRECTORY &&
            !portal_write_byte(portal_cap, '/')) return 0;
        if (!portal_write_byte(portal_cap, '\n')) return 0;
        any = 1;
    }
    if (!any && !portal_write(portal_cap, "(empty archive)\n")) return 0;
    return 1;
}

static int archive_stat(JcosBootArchive *archive, JcosCapabilityHandle portal_cap,
    const char *path) {
    if (!archive_available(archive) || !path || !*path) {
        (void)portal_write(portal_cap, "usage: stat PATH\n");
        return 0;
    }
    JcosBootArchiveEntry entry;
    if (!jcos_boot_archive_find(archive, path, &entry)) {
        (void)portal_write(portal_cap, "stat: path not found\n");
        return 0;
    }
    if (!portal_write(portal_cap, entry.type == JCOS_BOOT_ARCHIVE_ENTRY_DIRECTORY ? "DIR " : "FILE ") ||
        !portal_write(portal_cap, entry.path) || !portal_write(portal_cap, " SIZE=") ||
        !portal_write_u64(portal_cap, entry.size) || !portal_write_byte(portal_cap, '\n')) return 0;
    return 1;
}

static int archive_cat(JcosBootArchive *archive, JcosCapabilityHandle portal_cap,
    const char *path) {
    if (!archive_available(archive) || !path || !*path) {
        (void)portal_write(portal_cap, "usage: cat PATH\n");
        return 0;
    }
    JcosBootArchiveEntry entry;
    if (!jcos_boot_archive_find(archive, path, &entry)) {
        (void)portal_write(portal_cap, "cat: path not found\n");
        return 0;
    }
    if (entry.type != JCOS_BOOT_ARCHIVE_ENTRY_FILE) {
        (void)portal_write(portal_cap, "cat: not a regular file\n");
        return 0;
    }
    if (entry.size > 16384ULL) {
        (void)portal_write(portal_cap, "cat: file exceeds R7c.1 bounded display limit\n");
        return 0;
    }

    unsigned char buffer[64];
    JcosU64 offset = 0ULL;
    unsigned char last = 0U;
    while (offset < entry.size) {
        JcosU64 remaining = entry.size - offset;
        JcosU32 chunk = remaining > sizeof(buffer) ? (JcosU32)sizeof(buffer) : (JcosU32)remaining;
        if (!jcos_boot_archive_read_file(archive, &entry, offset, buffer, chunk)) return 0;
        for (JcosU32 i = 0; i < chunk; ++i) {
            last = buffer[i];
            if (!portal_write_byte(portal_cap, (JcosU64)buffer[i])) return 0;
        }
        offset += chunk;
    }
    if ((!entry.size || last != '\n') && !portal_write_byte(portal_cap, '\n')) return 0;
    return 1;
}

static int archive_fscheck(JcosBootArchive *archive, JcosCapabilityHandle portal_cap) {
    static const char path[] = "/etc/r7c1.txt";
    /*
     * Keep this marker free of a trailing line terminator. That makes the
     * archive fixture byte-identical even when the source tree was checked out
     * on a host with CRLF conversion enabled.
     */
    static const char expected[] = "R7C1 USERSPACE BOOT ARCHIVE NAMESPACE";
    if (!archive_available(archive)) return 0;

    JcosBootArchiveEntry entry;
    JcosU32 expected_size = text_length(expected);
    if (!jcos_boot_archive_find(archive, path, &entry) ||
        entry.type != JCOS_BOOT_ARCHIVE_ENTRY_FILE || entry.size != expected_size) {
        (void)portal_write(portal_cap, "R7C.1 USERSPACE TAR NAMESPACE: FAILED\n");
        if (jcos_boot_archive_find(archive, path, &entry) &&
            entry.type == JCOS_BOOT_ARCHIVE_ENTRY_FILE) {
            (void)portal_write(portal_cap, "  FIXTURE SIZE MISMATCH\n");
        }
        return 0;
    }

    unsigned char bytes[64];
    if (expected_size > sizeof(bytes) ||
        !jcos_boot_archive_read_file(archive, &entry, 0ULL, bytes, expected_size)) {
        (void)portal_write(portal_cap, "R7C.1 USERSPACE TAR NAMESPACE: FAILED\n");
        return 0;
    }
    for (JcosU32 i = 0; i < expected_size; ++i) {
        if (bytes[i] != (unsigned char)expected[i]) {
            (void)portal_write(portal_cap, "R7C.1 USERSPACE TAR NAMESPACE: FAILED\n");
            return 0;
        }
    }
    return portal_write(portal_cap, "R7C.1 USERSPACE TAR NAMESPACE: PASS\n");
}

static int launch_request_send(JcosCapabilityHandle launch_cap, JcosU64 incarnation,
    const JcosBootArchiveEntry *entry) {
    if (!launch_cap || !incarnation || !entry || entry->type != JCOS_BOOT_ARCHIVE_ENTRY_FILE ||
        !entry->size) return 0;
    JcosIpcMessage request;
    clear_message(&request);
    request.word_count = 4U;
    request.words[0] = JCOS_PROGRAM_BROKER_HEADER(
        JCOS_PROGRAM_BROKER_OP_FOREGROUND_EXTENT, JCOS_PROGRAM_BROKER_PROTOCOL_VERSION);
    request.words[1] = incarnation;
    request.words[2] = entry->data_offset;
    request.words[3] = entry->size;
    return jcos_ipc_send_blocking(launch_cap, &request);
}

static int media_launch_request_send(JcosCapabilityHandle launch_cap, JcosU64 incarnation,
    const JcosBootArchiveEntry *executable, const JcosBootArchiveEntry *media) {
    if (!launch_cap || !incarnation || !executable || !media ||
        executable->type != JCOS_BOOT_ARCHIVE_ENTRY_FILE ||
        media->type != JCOS_BOOT_ARCHIVE_ENTRY_FILE ||
        !executable->size || !media->size ||
        executable->data_offset > 0xFFFFFFFFULL || executable->size > 0xFFFFFFFFULL ||
        media->data_offset > 0xFFFFFFFFULL || media->size > 0xFFFFFFFFULL) return 0;
    JcosIpcMessage request;
    clear_message(&request);
    request.word_count = 4U;
    request.words[0] = JCOS_PROGRAM_BROKER_HEADER(
        JCOS_PROGRAM_BROKER_OP_FOREGROUND_MEDIA, JCOS_PROGRAM_BROKER_PROTOCOL_VERSION);
    request.words[1] = incarnation;
    request.words[2] = JCOS_PROGRAM_BROKER_PACK_EXTENT(
        executable->data_offset, executable->size);
    request.words[3] = JCOS_PROGRAM_BROKER_PACK_EXTENT(
        media->data_offset, media->size);
    return jcos_ipc_send_blocking(launch_cap, &request);
}

static int service_request_send(JcosCapabilityHandle service_cap, JcosU64 incarnation,
    JcosU64 op, const JcosBootArchiveEntry *entry) {
    if (!service_cap || !incarnation) return 0;
    JcosIpcMessage request;
    clear_message(&request);
    request.words[0] = JCOS_SERVICE_BROKER_HEADER(op, JCOS_SERVICE_BROKER_PROTOCOL_VERSION);
    request.words[1] = incarnation;
    if (op == JCOS_SERVICE_BROKER_OP_START_EXTENT ||
        op == JCOS_SERVICE_BROKER_OP_RESTART_EXTENT) {
        if (!entry || entry->type != JCOS_BOOT_ARCHIVE_ENTRY_FILE || !entry->size) return 0;
        request.word_count = 4U;
        request.words[2] = entry->data_offset;
        request.words[3] = entry->size;
    } else {
        request.word_count = 2U;
    }
    return jcos_ipc_send_blocking(service_cap, &request);
}

static int service_name_supported(const char *name) {
    return text_equal(name, "worker");
}

static const char *service_image_path(const char *name, int replacement) {
    if (!service_name_supported(name)) return "";
    return replacement ? "/bin/ordinaryservice-v2.elf" : "/bin/ordinaryservice.elf";
}

static int service_local_complete(ShellState *shell, JcosCapabilityHandle portal_cap,
    JcosU64 result, JcosU64 *out_result) {
    if (!shell || !out_result) return 0;
    shell->pending_service_op = 0ULL;
    shell->pending_service_replacement = shell->worker_replacement;
    shell_reset_line(shell);
    *out_result = result;
    return shell_prompt(portal_cap);
}

static int service_command(ShellState *shell, JcosCapabilityHandle portal_cap,
    JcosBootArchive *archive, JcosCapabilityHandle service_cap, JcosU64 incarnation,
    const char *args, JcosU64 *out_action, JcosU64 *out_result) {
    if (!shell || !portal_cap || !service_cap || !out_action || !out_result) return 0;
    const char *rest = 0;
    JcosU64 op = 0ULL;
    const char *name = 0;
    int replacement = shell->worker_replacement;

    if (command_prefix(args, "start", &rest)) op = JCOS_SERVICE_BROKER_OP_START_EXTENT;
    else if (command_prefix(args, "stop", &rest)) op = JCOS_SERVICE_BROKER_OP_STOP;
    else if (command_prefix(args, "restart", &rest)) op = JCOS_SERVICE_BROKER_OP_RESTART_EXTENT;
    else if (command_prefix(args, "replace", &rest)) {
        op = JCOS_SERVICE_BROKER_OP_RESTART_EXTENT;
        replacement = 1;
    }
    else if (command_prefix(args, "status", &rest)) op = JCOS_SERVICE_BROKER_OP_STATUS;
    else if (command_prefix(args, "fault", &rest)) op = JCOS_SERVICE_BROKER_OP_DIAG_FAULT;
    else {
        if (!portal_write(portal_cap,
                "usage: service start|stop|restart|replace|status|fault worker\n")) return 0;
        return service_local_complete(shell, portal_cap,
            JCOS_CONSOLE_SHELL_RESULT_UNKNOWN, out_result);
    }

    name = skip_spaces(rest);
    if (!service_name_supported(name)) {
        if (!portal_write(portal_cap, "service: unknown service\n")) return 0;
        return service_local_complete(shell, portal_cap,
            JCOS_CONSOLE_SHELL_RESULT_UNKNOWN, out_result);
    }

    JcosBootArchiveEntry entry;
    JcosBootArchiveEntry *entry_ptr = 0;
    if (op == JCOS_SERVICE_BROKER_OP_START_EXTENT || op == JCOS_SERVICE_BROKER_OP_RESTART_EXTENT) {
        const char *path = service_image_path(name, replacement);
        if (!archive_available(archive) || !jcos_boot_archive_find(archive, path, &entry) ||
            entry.type != JCOS_BOOT_ARCHIVE_ENTRY_FILE || !entry.size) {
            if (!portal_write(portal_cap, "service: executable not found\n")) return 0;
            return service_local_complete(shell, portal_cap,
                JCOS_CONSOLE_SHELL_RESULT_UNKNOWN, out_result);
        }
        entry_ptr = &entry;
    }

    if (!service_request_send(service_cap, incarnation, op, entry_ptr)) {
        if (!portal_write(portal_cap, "service: control request unavailable\n")) return 0;
        return service_local_complete(shell, portal_cap,
            JCOS_CONSOLE_SHELL_RESULT_UNKNOWN, out_result);
    }

    shell->pending_service_op = op;
    shell->pending_service_replacement = replacement;
    shell->active = SHELL_STATE_SUSPENDED;
    shell_reset_line(shell);
    *out_action = JCOS_CONSOLE_SHELL_ACTION_SERVICE_CONTROL;
    *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
    return 1;
}

static int service_result_present(ShellState *shell, JcosCapabilityHandle portal_cap,
    JcosU64 packed, JcosU64 service_incarnation) {
    if (!shell || shell->active != SHELL_STATE_SUSPENDED || !shell->pending_service_op) return 0;
    JcosU64 result = JCOS_CONSOLE_SERVICE_UNPACK_SERVICE_RESULT(packed);
    JcosU64 state = JCOS_CONSOLE_SERVICE_UNPACK_SERVICE_STATE(packed);

    if (!portal_write(portal_cap, "SERVICE worker: ")) return 0;
    if (result == JCOS_SERVICE_BROKER_RESULT_ALREADY_RUNNING) {
        if (!portal_write(portal_cap, "ALREADY RUNNING")) return 0;
    } else if (result == JCOS_SERVICE_BROKER_RESULT_NOT_RUNNING) {
        if (!portal_write(portal_cap, "NOT RUNNING")) return 0;
    } else if (result != JCOS_SERVICE_BROKER_RESULT_OK) {
        if (!portal_write(portal_cap, "CONTROL FAILED")) return 0;
    } else if (state == JCOS_SERVICE_BROKER_STATE_RUNNING) {
        if (!portal_write(portal_cap, "RUNNING")) return 0;
    } else if (state == JCOS_SERVICE_BROKER_STATE_STOPPED) {
        if (!portal_write(portal_cap, "STOPPED")) return 0;
    } else if (state == JCOS_SERVICE_BROKER_STATE_FAILED) {
        if (!portal_write(portal_cap, "FAILED")) return 0;
    } else {
        if (!portal_write(portal_cap, "UNKNOWN STATE")) return 0;
    }

    if (service_incarnation) {
        if (!portal_write(portal_cap, " INCARNATION ") ||
            !portal_write_u64(portal_cap, service_incarnation)) return 0;
    }
    if (!portal_write_byte(portal_cap, '\n')) return 0;

    if (result == JCOS_SERVICE_BROKER_RESULT_OK &&
        state == JCOS_SERVICE_BROKER_STATE_RUNNING &&
        (shell->pending_service_op == JCOS_SERVICE_BROKER_OP_START_EXTENT ||
         shell->pending_service_op == JCOS_SERVICE_BROKER_OP_RESTART_EXTENT)) {
        shell->worker_replacement = shell->pending_service_replacement;
    }
    shell->pending_service_op = 0ULL;
    shell->pending_service_replacement = shell->worker_replacement;
    shell->active = SHELL_STATE_ACTIVE;
    shell_reset_line(shell);
    return shell_prompt(portal_cap);
}

static int shell_execute(ShellState *shell, JcosCapabilityHandle portal_cap,
    JcosBootArchive *archive, JcosCapabilityHandle launch_cap, JcosCapabilityHandle service_cap,
    JcosU64 incarnation,
    JcosU64 *out_action, JcosU64 *out_result) {
    if (!shell || !out_action || !out_result) return 0;
    *out_action = JCOS_CONSOLE_SHELL_ACTION_NONE;
    *out_result = JCOS_CONSOLE_SHELL_RESULT_EMPTY;

    shell->line[shell->length] = 0;
    const char *line = skip_spaces(shell->line);
    JcosU32 length = 0U;
    while (line[length]) ++length;
    while (length && (line[length - 1U] == ' ' || line[length - 1U] == '\t')) --length;

    char normalized[SHELL_LINE_CAPACITY];
    if (length >= SHELL_LINE_CAPACITY) length = SHELL_LINE_CAPACITY - 1U;
    for (JcosU32 i = 0; i < length; ++i) normalized[i] = line[i];
    normalized[length] = 0;

    if (!length) {
        shell_reset_line(shell);
        return shell_prompt(portal_cap);
    }

    const char *rest = 0;
    if (text_equal(normalized, "help")) {
        if (!portal_write(portal_cap,
                "help               show this command list\n"
                "echo TEXT          print TEXT\n"
                "about              describe the Ring3 shell\n"
                "status             show the current policy boundary\n"
                "ls                 list boot-archive namespace entries\n"
                "stat PATH          inspect one boot-archive entry\n"
                "cat PATH           print a bounded regular file\n"
                "fscheck            validate Ring3 tar parsing/read policy\n"
                "run PATH           launch a foreground ELF selected in Ring3\n"
                "play TITLE         play badapple or caramel (Escape stops)\n"
                "service CMD worker manage an ordinary background service\n"
                "  CMD: start stop restart replace status fault\n"
                "monitor            return input to the kernel emergency monitor\n"
                "KEYS: UP/DOWN HISTORY, LEFT/RIGHT EDIT, HOME/END, PGUP/PGDN PAGE SCROLL.\n"
                "      CTRL+PGUP/PGDN SCROLL ONE LINE.\n"
                "CLIPBOARD: SHIFT+ARROWS SELECT, CTRL+SHIFT+C/X/V COPY/CUT/PASTE.\n")) return 0;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
    } else if (command_prefix(normalized, "echo", &rest)) {
        if (!portal_write(portal_cap, rest) || !portal_write_byte(portal_cap, '\n')) return 0;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
    } else if (text_equal(normalized, "about")) {
        if (!portal_write(portal_cap,
                "JA OS R7A.3 NORMAL SHELL POLICY IS RUNNING IN RING3.\n")) return 0;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
    } else if (text_equal(normalized, "status")) {
        if (!portal_write(portal_cap,
                "SHELL=RING3  CONSOLE=RING3  FS NAMESPACE=RING3  LAUNCH POLICY=RING3  SERVICE POLICY=RING3  BOOT ARCHIVE=RAW KERNEL PORTAL  MONITOR=KERNEL\n")) return 0;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
    } else if (text_equal(normalized, "ls")) {
        *out_result = archive_list(archive, portal_cap) ?
            JCOS_CONSOLE_SHELL_RESULT_OK : JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
    } else if (command_prefix(normalized, "stat", &rest)) {
        *out_result = archive_stat(archive, portal_cap, rest) ?
            JCOS_CONSOLE_SHELL_RESULT_OK : JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
    } else if (command_prefix(normalized, "cat", &rest)) {
        *out_result = archive_cat(archive, portal_cap, rest) ?
            JCOS_CONSOLE_SHELL_RESULT_OK : JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
    } else if (text_equal(normalized, "fscheck")) {
        *out_result = archive_fscheck(archive, portal_cap) ?
            JCOS_CONSOLE_SHELL_RESULT_OK : JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
    } else if (command_prefix(normalized, "service", &rest)) {
        return service_command(shell, portal_cap, archive, service_cap, incarnation,
            rest, out_action, out_result);
    } else if (command_prefix(normalized, "run", &rest)) {
        JcosBootArchiveEntry entry;
        if (!archive_available(archive) || !rest || !*rest) {
            if (!portal_write(portal_cap, "usage: run PATH\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else if (!jcos_boot_archive_find(archive, rest, &entry) ||
            entry.type != JCOS_BOOT_ARCHIVE_ENTRY_FILE || !entry.size) {
            if (!portal_write(portal_cap, "run: executable not found\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else if (!launch_request_send(launch_cap, incarnation, &entry)) {
            if (!portal_write(portal_cap, "run: launch request unavailable\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else {
            if (!portal_write(portal_cap, "RUNNING ") || !portal_write(portal_cap, entry.path) ||
                !portal_write_byte(portal_cap, '\n')) return 0;
            shell->active = SHELL_STATE_SUSPENDED;
            shell_reset_line(shell);
            *out_action = JCOS_CONSOLE_SHELL_ACTION_RUN_FOREGROUND;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
            return 1;
        }
    } else if (command_prefix(normalized, "play", &rest)) {
        const char *media_path = 0;
        if (text_equal(rest, "badapple")) media_path = "media/badapple.jmv";
        else if (text_equal(rest, "caramel") || text_equal(rest, "caramelldansen"))
            media_path = "media/caramel.jmv";
        JcosBootArchiveEntry executable;
        JcosBootArchiveEntry media;
        if (!archive_available(archive) || !media_path) {
            if (!portal_write(portal_cap, "usage: play badapple|caramel\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else if (!jcos_boot_archive_find(archive, "bin/mediaplayer.elf", &executable) ||
            !jcos_boot_archive_find(archive, media_path, &media)) {
            if (!portal_write(portal_cap, "play: player or media not found\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else if (!media_launch_request_send(launch_cap, incarnation, &executable, &media)) {
            if (!portal_write(portal_cap, "play: launch request unavailable\n")) return 0;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
        } else {
            if (!portal_write(portal_cap, "LAUNCHING ") || !portal_write(portal_cap, rest) ||
                !portal_write(portal_cap, " - ESCAPE TO STOP\n")) return 0;
            shell->active = SHELL_STATE_SUSPENDED;
            shell_reset_line(shell);
            *out_action = JCOS_CONSOLE_SHELL_ACTION_RUN_FOREGROUND;
            *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
            return 1;
        }
    } else if (text_equal(normalized, "monitor")) {
        if (!portal_write(portal_cap, "RETURNING INPUT TO KERNEL MONITOR.\n")) return 0;
        shell->active = SHELL_STATE_INACTIVE;
        shell_reset_line(shell);
        *out_action = JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
        return 1;
    } else {
        if (!portal_write(portal_cap, "UNKNOWN USERSPACE COMMAND: ") ||
            !portal_write(portal_cap, normalized) || !portal_write_byte(portal_cap, '\n')) return 0;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_UNKNOWN;
    }

    shell_reset_line(shell);
    return shell_prompt(portal_cap);
}

static int shell_handle_key(ShellState *shell, JcosCapabilityHandle portal_cap,
    JcosBootArchive *archive, JcosCapabilityHandle launch_cap, JcosCapabilityHandle service_cap,
    JcosU64 incarnation,
    JcosU64 key, JcosU64 event,
    JcosU64 *out_action, JcosU64 *out_result) {
    if (!shell || !out_action || !out_result) return 0;
    *out_action = JCOS_CONSOLE_SHELL_ACTION_NONE;
    *out_result = JCOS_CONSOLE_SHELL_RESULT_NONE;
    if (shell->active != SHELL_STATE_ACTIVE) return 1;

    int shift = (event & JCOS_CONSOLE_INPUT_EVENT_SHIFT) != 0ULL;
    int ctrl = (event & JCOS_CONSOLE_INPUT_EVENT_CTRL) != 0ULL;

    if (key == JCOS_CONSOLE_INPUT_KEY_PAGE_UP)
        return ctrl ? portal_scrollback_line_up(portal_cap) :
            portal_scrollback_page_up(portal_cap);
    if (key == JCOS_CONSOLE_INPUT_KEY_PAGE_DOWN)
        return ctrl ? portal_scrollback_line_down(portal_cap) :
            portal_scrollback_page_down(portal_cap);

    /* Any ordinary editing/navigation action returns to the live viewport. */
    if (!portal_scrollback_to_bottom(portal_cap)) return 0;

    if (key == JCOS_CONSOLE_INPUT_KEY_CHARACTER && ctrl && shift) {
        char lower = shell_ascii_lower((char)(event & JCOS_CONSOLE_INPUT_EVENT_CHARACTER_MASK));
        if (lower == 'c') {
            shell_copy_selection_or_line(shell);
            return 1;
        }
        if (lower == 'x') return shell_cut_selection_or_line(shell, portal_cap);
        if (lower == 'v') return shell_paste_clipboard(shell, portal_cap);
    }

    if (key == JCOS_CONSOLE_INPUT_KEY_CHARACTER) {
        return shell_insert_character(shell, portal_cap,
            (char)(event & JCOS_CONSOLE_INPUT_EVENT_CHARACTER_MASK));
    }
    if (key == JCOS_CONSOLE_INPUT_KEY_BACKSPACE) return shell_erase_before(shell, portal_cap);
    if (key == JCOS_CONSOLE_INPUT_KEY_DELETE) return shell_erase_at(shell, portal_cap);
    if (key == JCOS_CONSOLE_INPUT_KEY_LEFT) return shell_move_left(shell, portal_cap, shift);
    if (key == JCOS_CONSOLE_INPUT_KEY_RIGHT) return shell_move_right(shell, portal_cap, shift);
    if (key == JCOS_CONSOLE_INPUT_KEY_HOME) return shell_move_home(shell, portal_cap, shift);
    if (key == JCOS_CONSOLE_INPUT_KEY_END) return shell_move_end(shell, portal_cap, shift);
    if (key == JCOS_CONSOLE_INPUT_KEY_UP) return shell_history_up(shell, portal_cap);
    if (key == JCOS_CONSOLE_INPUT_KEY_DOWN) return shell_history_down(shell, portal_cap);

    if (key == JCOS_CONSOLE_INPUT_KEY_TAB) {
        static const char spaces[] = "    ";
        return shell_insert_bytes(shell, portal_cap, spaces, 4U);
    }

    if (key == JCOS_CONSOLE_INPUT_KEY_ENTER) {
        shell_history_store(shell);
        if (shell->selection_active || shell->cursor != shell->length) {
            shell_clear_selection(shell);
            shell->cursor = shell->length;
            if (!shell_redraw(shell, portal_cap)) return 0;
        }
        if (!portal_write_byte(portal_cap, '\n')) return 0;
        return shell_execute(shell, portal_cap, archive, launch_cap, service_cap,
            incarnation, out_action, out_result);
    }

    if (key == JCOS_CONSOLE_INPUT_KEY_ESCAPE) {
        if (!portal_write(portal_cap, "\nRETURNING INPUT TO KERNEL MONITOR.\n")) return 0;
        shell->active = SHELL_STATE_INACTIVE;
        shell->pending_service_op = 0ULL;
        shell->pending_service_replacement = shell->worker_replacement;
        shell_reset_line(shell);
        *out_action = JCOS_CONSOLE_SHELL_ACTION_RETURN_MONITOR;
        *out_result = JCOS_CONSOLE_SHELL_RESULT_OK;
        return 1;
    }

    return 1;
}

static int handle_app_request(const JcosIpcMessage *request, JcosCapabilityHandle portal_cap,
    JcosU64 *session_id) {
    if (!request || !session_id || !*session_id || request->word_count < 2U) return 1;

    JcosU64 header = request->words[0];
    JcosU64 operation = JCOS_CONSOLE_CLIENT_HEADER_OP(header);
    JcosU64 version = JCOS_CONSOLE_CLIENT_HEADER_VERSION(header);
    JcosU64 count = JCOS_CONSOLE_CLIENT_HEADER_COUNT(header);

    /* Application input is untrusted. Invalid/stale messages are discarded
     * without producing a management reply or changing service state. */
    if (version != JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION || request->words[1] != *session_id) return 1;

    if (operation == JCOS_CONSOLE_CLIENT_OP_WRITE) {
        if (request->word_count != 4U || !count || count > JCOS_CONSOLE_CLIENT_MAX_WRITE_BYTES) return 1;
        for (JcosU32 i = 0; i < (JcosU32)count; ++i) {
            JcosU32 word = i < 8U ? 2U : 3U;
            JcosU32 shift = (i & 7U) * 8U;
            JcosU64 value = (request->words[word] >> shift) & 0xFFULL;
            if (!portal_write_byte(portal_cap, value)) return 0;
        }
        return 1;
    }

    if (operation == JCOS_CONSOLE_CLIENT_OP_END) {
        if (request->word_count != 2U || count) return 1;
        *session_id = 0ULL;
        return 1;
    }

    return 1;
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosCapabilityHandle portal_cap = startup->capabilities[2];
    JcosCapabilityHandle app_cap = startup->capability_count >= 4U
        ? startup->capabilities[3] : JCOS_CAPABILITY_INVALID_HANDLE;
    JcosCapabilityHandle launch_cap = startup->capability_count >= 7U
        ? startup->capabilities[6] : JCOS_CAPABILITY_INVALID_HANDLE;
    JcosCapabilityHandle service_cap = startup->capability_count == 8U
        ? startup->capabilities[7] : JCOS_CAPABILITY_INVALID_HANDLE;
    JcosU64 incarnation = startup->arguments[0];
    JcosU64 app_session_id = 0ULL;
    JcosBootArchive archive_storage;
    JcosBootArchive *archive = 0;
    if (startup->capability_count >= 6U) {
        if (!jcos_boot_archive_init(&archive_storage, startup->capabilities[4],
                startup->capabilities[5], incarnation)) jcos_thread_exit();
        archive = &archive_storage;
    }
    static ShellState shell;
    shell_init(&shell);

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        JcosCapabilityHandle receive_cap = app_session_id ? app_cap : command_cap;
        if (!receive_cap || !jcos_ipc_receive_blocking(receive_cap, &request) || request.word_count < 2U) {
            jcos_thread_exit();
        }

        if (app_session_id) {
            if (!handle_app_request(&request, portal_cap, &app_session_id)) jcos_thread_exit();
            continue;
        }

        JcosU64 operation = request.words[0];
        JcosU64 requested_version = request.words[1];
        if (requested_version != JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, requested_version, 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_WRITE_BYTE) {
            if (request.word_count != 3U || request.words[2] > JCOS_CONSOLE_PORTAL_MAX_BYTE) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            if (!portal_write_byte(portal_cap, request.words[2])) jcos_thread_exit();
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_WRITTEN, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_ACTIVATE) {
            if (request.word_count != 2U || !shell_activate(&shell, portal_cap)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_SHELL_ACTIVE, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, 0ULL, 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_DEACTIVATE) {
            if (request.word_count != 2U || !shell_deactivate(&shell, portal_cap)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_SHELL_INACTIVE, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, 0ULL, 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_SHELL_KEY_EVENT) {
            if (request.word_count != 4U || request.words[2] < JCOS_CONSOLE_INPUT_KEY_CHARACTER ||
                request.words[2] > JCOS_CONSOLE_INPUT_KEY_PAGE_DOWN) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU64 action = JCOS_CONSOLE_SHELL_ACTION_NONE;
            JcosU64 result = JCOS_CONSOLE_SHELL_RESULT_NONE;
            if (!shell_handle_key(&shell, portal_cap, archive, launch_cap, service_cap, incarnation,
                    request.words[2], request.words[3], &action, &result)) {
                jcos_thread_exit();
            }
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_KEY_HANDLED, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION,
                    JCOS_CONSOLE_SHELL_PACK_RESULT(action, result), 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_SERVICE_RESULT) {
            if (request.word_count != 4U || !service_result_present(&shell, portal_cap,
                    request.words[2], request.words[3])) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_SERVICE_RESULT, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_APP_SESSION_BEGIN) {
            if (request.word_count != 3U || !app_cap || !request.words[2] ||
                shell.active == SHELL_STATE_ACTIVE) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            app_session_id = request.words[2];
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_APP_SESSION_ACTIVE, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, app_session_id, 4U)) jcos_thread_exit();
            continue;
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_DIAGNOSTIC_FAULT) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            __asm__ volatile ("ud2");
            jcos_thread_exit();
        }

        if (operation == JCOS_CONSOLE_SERVICE_MESSAGE_SHUTDOWN) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation,
                        JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_CONSOLE_SERVICE_REPLY_STOPPED, incarnation,
                    JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, 0ULL, 3U)) jcos_thread_exit();
            jcos_thread_exit();
        }

        if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_UNKNOWN_OPERATION, incarnation,
                JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION, operation, 4U)) jcos_thread_exit();
    }
}
