#include "editor.h"

#include "lib.h"
#include "terminal.h"

static void copy_line(char *dst, const char *src, u32 capacity) {
    if (!dst || !capacity) return;

    u32 i = 0;

    if (src) {
        while (i + 1U < capacity && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }

    dst[i] = 0;
}

static u32 selection_start(const ShellEditor *editor) {
    return editor->cursor < editor->selection_anchor
        ? editor->cursor
        : editor->selection_anchor;
}

static u32 selection_end(const ShellEditor *editor) {
    return editor->cursor > editor->selection_anchor
        ? editor->cursor
        : editor->selection_anchor;
}

static bool selected_at(const ShellEditor *editor, u32 index) {
    if (!editor->selection_active) return false;

    u32 start = selection_start(editor);
    u32 end = selection_end(editor);
    return index >= start && index < end;
}

static void clear_selection(ShellEditor *editor) {
    if (!editor) return;
    editor->selection_active = false;
    editor->selection_anchor = editor->cursor;
}

static void redraw(ShellEditor *editor) {
    if (!editor) return;

    bool cursor_visible = terminal_cursor_visible();
    terminal_cursor_set_visible(false);

    while (editor->rendered_cursor) {
        if (!terminal_cursor_left()) break;
        --editor->rendered_cursor;
    }

    for (u32 i = 0; i < editor->length; ++i) {
        terminal_set_color(selected_at(editor, i)
            ? terminal_accent_color()
            : terminal_default_color());

        terminal_putchar(editor->buffer[i]);
    }

    terminal_set_color(terminal_default_color());

    u32 display_length = editor->rendered_length;
    if (editor->length > display_length) display_length = editor->length;

    for (u32 i = editor->length; i < display_length; ++i)
        terminal_putchar(' ');

    editor->rendered_cursor = display_length;

    while (editor->rendered_cursor > editor->cursor) {
        if (!terminal_cursor_left()) break;
        --editor->rendered_cursor;
    }

    editor->rendered_length = editor->length;
    editor->rendered_cursor = editor->cursor;

    if (cursor_visible) terminal_cursor_set_visible(true);
}

static void leave_history(ShellEditor *editor) {
    editor->history_offset = -1;
    editor->draft_saved = false;
}

static void history_store(ShellEditor *editor) {
    if (!editor || !editor->length) return;

    if (editor->history_count) {
        u32 newest = (editor->history_next + SHELL_HISTORY_CAPACITY - 1U) % SHELL_HISTORY_CAPACITY;
        if (k_streq(editor->history[newest], editor->buffer)) return;
    }

    copy_line(editor->history[editor->history_next], editor->buffer, SHELL_EDITOR_CAPACITY);
    editor->history_next = (editor->history_next + 1U) % SHELL_HISTORY_CAPACITY;

    if (editor->history_count < SHELL_HISTORY_CAPACITY)
        ++editor->history_count;
}

static u32 history_index(const ShellEditor *editor, u32 offset) {
    return (editor->history_next + SHELL_HISTORY_CAPACITY - 1U - offset) % SHELL_HISTORY_CAPACITY;
}

static void history_load(ShellEditor *editor, u32 offset) {
    if (!editor || offset >= editor->history_count) return;

    copy_line(editor->buffer, editor->history[history_index(editor, offset)], SHELL_EDITOR_CAPACITY);
    editor->length = (u32)k_strlen(editor->buffer);
    editor->cursor = editor->length;
    clear_selection(editor);
    redraw(editor);
}

static void history_up(ShellEditor *editor) {
    if (!editor || !editor->history_count) return;

    if (editor->history_offset < 0) {
        copy_line(editor->draft, editor->buffer, SHELL_EDITOR_CAPACITY);
        editor->draft_length = editor->length;
        editor->draft_saved = true;
        editor->history_offset = 0;
    } else if ((u32)(editor->history_offset + 1) < editor->history_count) {
        ++editor->history_offset;
    }

    history_load(editor, (u32)editor->history_offset);
}

static void history_down(ShellEditor *editor) {
    if (!editor || editor->history_offset < 0) return;

    if (editor->history_offset > 0) {
        --editor->history_offset;
        history_load(editor, (u32)editor->history_offset);
        return;
    }

    editor->history_offset = -1;

    if (editor->draft_saved) {
        copy_line(editor->buffer, editor->draft, SHELL_EDITOR_CAPACITY);
        editor->length = editor->draft_length;
    } else {
        editor->buffer[0] = 0;
        editor->length = 0;
    }

    editor->cursor = editor->length;
    editor->draft_saved = false;
    clear_selection(editor);
    redraw(editor);
}

static void delete_range(ShellEditor *editor, u32 start, u32 end) {
    if (!editor || start >= end || end > editor->length) return;

    u32 count = end - start;

    for (u32 i = start; i + count <= editor->length; ++i)
        editor->buffer[i] = editor->buffer[i + count];

    editor->length -= count;
    editor->cursor = start;
    clear_selection(editor);
    leave_history(editor);
}

static bool delete_selection(ShellEditor *editor) {
    if (!editor || !editor->selection_active) return false;

    u32 start = selection_start(editor);
    u32 end = selection_end(editor);
    delete_range(editor, start, end);
    return true;
}

static void insert_bytes(ShellEditor *editor, const char *bytes, u32 count) {
    if (!editor || !bytes || !count) return;

    if (editor->selection_active)
        delete_selection(editor);

    u32 available = SHELL_EDITOR_CAPACITY - 1U - editor->length;
    if (count > available) count = available;
    if (!count) return;

    for (u32 i = editor->length; i > editor->cursor; --i)
        editor->buffer[i + count - 1U] = editor->buffer[i - 1U];

    for (u32 i = 0; i < count; ++i)
        editor->buffer[editor->cursor + i] = bytes[i];

    editor->cursor += count;
    editor->length += count;
    editor->buffer[editor->length] = 0;
    clear_selection(editor);
    leave_history(editor);
    redraw(editor);
}

static void insert_character(ShellEditor *editor, char c) {
    insert_bytes(editor, &c, 1U);
}

static void erase_before(ShellEditor *editor) {
    if (!editor) return;

    if (delete_selection(editor)) {
        redraw(editor);
        return;
    }

    if (!editor->cursor) return;

    u32 target = editor->cursor - 1U;
    delete_range(editor, target, editor->cursor);
    redraw(editor);
}

static void erase_at(ShellEditor *editor) {
    if (!editor) return;

    if (delete_selection(editor)) {
        redraw(editor);
        return;
    }

    if (editor->cursor >= editor->length) return;

    delete_range(editor, editor->cursor, editor->cursor + 1U);
    redraw(editor);
}

static void selection_begin_if_needed(ShellEditor *editor) {
    if (editor->selection_active) return;
    editor->selection_anchor = editor->cursor;
}

static void selection_finish_move(ShellEditor *editor) {
    editor->selection_active = editor->cursor != editor->selection_anchor;
}

static void move_left(ShellEditor *editor, bool selecting) {
    if (!editor || !editor->cursor) return;

    if (selecting) selection_begin_if_needed(editor);
    else clear_selection(editor);

    if (terminal_cursor_left()) {
        --editor->cursor;
        --editor->rendered_cursor;
    }

    if (selecting) selection_finish_move(editor);
    redraw(editor);
}

static void move_right(ShellEditor *editor, bool selecting) {
    if (!editor || editor->cursor >= editor->length) return;

    if (selecting) selection_begin_if_needed(editor);
    else clear_selection(editor);

    if (terminal_cursor_right()) {
        ++editor->cursor;
        ++editor->rendered_cursor;
    }

    if (selecting) selection_finish_move(editor);
    redraw(editor);
}

static void move_home(ShellEditor *editor, bool selecting) {
    if (!editor) return;

    if (selecting) selection_begin_if_needed(editor);
    else clear_selection(editor);

    while (editor->cursor && terminal_cursor_left()) {
        --editor->cursor;
        --editor->rendered_cursor;
    }

    if (selecting) selection_finish_move(editor);
    redraw(editor);
}

static void move_end(ShellEditor *editor, bool selecting) {
    if (!editor) return;

    if (selecting) selection_begin_if_needed(editor);
    else clear_selection(editor);

    while (editor->cursor < editor->length && terminal_cursor_right()) {
        ++editor->cursor;
        ++editor->rendered_cursor;
    }

    if (selecting) selection_finish_move(editor);
    redraw(editor);
}

static void copy_selection_or_line(ShellEditor *editor) {
    if (!editor) return;

    u32 start = editor->selection_active ? selection_start(editor) : 0U;
    u32 end = editor->selection_active ? selection_end(editor) : editor->length;
    u32 count = end - start;

    if (count >= SHELL_EDITOR_CAPACITY) count = SHELL_EDITOR_CAPACITY - 1U;

    for (u32 i = 0; i < count; ++i)
        editor->clipboard[i] = editor->buffer[start + i];

    editor->clipboard[count] = 0;
    editor->clipboard_length = count;
}

static void cut_selection_or_line(ShellEditor *editor) {
    if (!editor) return;

    copy_selection_or_line(editor);

    if (editor->selection_active) {
        delete_selection(editor);
    } else {
        editor->buffer[0] = 0;
        editor->length = 0;
        editor->cursor = 0;
        clear_selection(editor);
        leave_history(editor);
    }

    redraw(editor);
}

static void paste_clipboard(ShellEditor *editor) {
    if (!editor || !editor->clipboard_length) return;
    insert_bytes(editor, editor->clipboard, editor->clipboard_length);
}

void shell_editor_init(ShellEditor *editor) {
    if (!editor) return;

    k_memset(editor, 0, sizeof(*editor));
    editor->history_offset = -1;
}

void shell_editor_reset_line(ShellEditor *editor) {
    if (!editor) return;

    editor->buffer[0] = 0;
    editor->length = 0;
    editor->cursor = 0;
    editor->rendered_length = 0;
    editor->rendered_cursor = 0;
    editor->history_offset = -1;
    editor->draft_saved = false;
    editor->draft_length = 0;
    clear_selection(editor);
}

ShellEditorResult shell_editor_handle(ShellEditor *editor, const KeyEvent *event) {
    if (!editor || !event || !event->pressed) return SHELL_EDITOR_CONTINUE;

    if (event->key == KEY_PAGE_UP) {
        terminal_scrollback_page_up();
        return SHELL_EDITOR_CONTINUE;
    }

    if (event->key == KEY_PAGE_DOWN) {
        terminal_scrollback_page_down();
        return SHELL_EDITOR_CONTINUE;
    }

    if (terminal_scrollback_active())
        terminal_scrollback_to_bottom();

    if (event->key == KEY_CHARACTER && event->ctrl && event->shift) {
        char lower = k_ascii_lower(event->character);

        if (lower == 'c') {
            copy_selection_or_line(editor);
            return SHELL_EDITOR_CONTINUE;
        }

        if (lower == 'x') {
            cut_selection_or_line(editor);
            return SHELL_EDITOR_CONTINUE;
        }

        if (lower == 'v') {
            paste_clipboard(editor);
            return SHELL_EDITOR_CONTINUE;
        }
    }

    switch (event->key) {
        case KEY_CHARACTER:
            if ((u8)event->character >= 32U && (u8)event->character <= 126U)
                insert_character(editor, event->character);
            break;

        case KEY_BACKSPACE:
            erase_before(editor);
            break;

        case KEY_DELETE:
            erase_at(editor);
            break;

        case KEY_LEFT:
            move_left(editor, event->shift);
            break;

        case KEY_RIGHT:
            move_right(editor, event->shift);
            break;

        case KEY_HOME:
            move_home(editor, event->shift);
            break;

        case KEY_END:
            move_end(editor, event->shift);
            break;

        case KEY_UP:
            history_up(editor);
            break;

        case KEY_DOWN:
            history_down(editor);
            break;

        case KEY_ENTER:
            clear_selection(editor);
            redraw(editor);
            move_end(editor, false);
            editor->buffer[editor->length] = 0;
            history_store(editor);
            editor->history_offset = -1;
            editor->draft_saved = false;
            return SHELL_EDITOR_SUBMIT;

        default:
            break;
    }

    return SHELL_EDITOR_CONTINUE;
}

const char *shell_editor_line(ShellEditor *editor) {
    if (!editor) return "";

    editor->buffer[editor->length] = 0;
    return editor->buffer;
}
