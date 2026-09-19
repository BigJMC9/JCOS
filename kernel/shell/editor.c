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

static void redraw(ShellEditor *editor) {
    if (!editor) return;

    while (editor->rendered_cursor) {
        if (!terminal_cursor_left()) break;
        --editor->rendered_cursor;
    }

    terminal_write(editor->buffer);

    u32 display_length = editor->rendered_length;
    if (editor->length > display_length) display_length = editor->length;

    for (u32 i = editor->length; i < display_length; ++i) terminal_putchar(' ');

    editor->rendered_cursor = display_length;
    while (editor->rendered_cursor > editor->cursor) {
        if (!terminal_cursor_left()) break;
        --editor->rendered_cursor;
    }

    editor->rendered_length = editor->length;
    editor->rendered_cursor = editor->cursor;
}

static void history_store(ShellEditor *editor) {
    if (!editor || !editor->length) return;

    if (editor->history_count) {
        u32 newest = (editor->history_next + SHELL_HISTORY_CAPACITY - 1U) % SHELL_HISTORY_CAPACITY;
        if (k_streq(editor->history[newest], editor->buffer)) return;
    }

    copy_line(editor->history[editor->history_next], editor->buffer, SHELL_EDITOR_CAPACITY);
    editor->history_next = (editor->history_next + 1U) % SHELL_HISTORY_CAPACITY;

    if (editor->history_count < SHELL_HISTORY_CAPACITY) ++editor->history_count;
}

static u32 history_index(const ShellEditor *editor, u32 offset) {
    return (editor->history_next + SHELL_HISTORY_CAPACITY - 1U - offset) % SHELL_HISTORY_CAPACITY;
}

static void history_load(ShellEditor *editor, u32 offset) {
    if (!editor || offset >= editor->history_count) return;

    copy_line(editor->buffer, editor->history[history_index(editor, offset)], SHELL_EDITOR_CAPACITY);
    editor->length = (u32)k_strlen(editor->buffer);
    editor->cursor = editor->length;
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
    redraw(editor);
}

static void insert_character(ShellEditor *editor, char c) {
    if (!editor || editor->length + 1U >= SHELL_EDITOR_CAPACITY) return;

    for (u32 i = editor->length; i > editor->cursor; --i) editor->buffer[i] = editor->buffer[i - 1U];

    editor->buffer[editor->cursor++] = c;
    ++editor->length;
    editor->buffer[editor->length] = 0;
    editor->history_offset = -1;
    editor->draft_saved = false;
    redraw(editor);
}

static void erase_before(ShellEditor *editor) {
    if (!editor || !editor->cursor) return;

    u32 target = editor->cursor - 1U;
    for (u32 i = target; i < editor->length; ++i) editor->buffer[i] = editor->buffer[i + 1U];

    --editor->length;
    editor->cursor = target;
    editor->history_offset = -1;
    editor->draft_saved = false;
    redraw(editor);
}

static void erase_at(ShellEditor *editor) {
    if (!editor || editor->cursor >= editor->length) return;

    for (u32 i = editor->cursor; i < editor->length; ++i) editor->buffer[i] = editor->buffer[i + 1U];

    --editor->length;
    editor->history_offset = -1;
    editor->draft_saved = false;
    redraw(editor);
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
}

ShellEditorResult shell_editor_handle(ShellEditor *editor, const KeyEvent *event) {
    if (!editor || !event || !event->pressed) return SHELL_EDITOR_CONTINUE;

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
            if (editor->cursor && terminal_cursor_left()) {
                --editor->cursor;
                --editor->rendered_cursor;
            }
            break;

        case KEY_RIGHT:
            if (editor->cursor < editor->length && terminal_cursor_right()) {
                ++editor->cursor;
                ++editor->rendered_cursor;
            }
            break;

        case KEY_HOME:
            while (editor->cursor && terminal_cursor_left()) {
                --editor->cursor;
                --editor->rendered_cursor;
            }
            break;

        case KEY_END:
            while (editor->cursor < editor->length && terminal_cursor_right()) {
                ++editor->cursor;
                ++editor->rendered_cursor;
            }
            break;

        case KEY_UP:
            history_up(editor);
            break;

        case KEY_DOWN:
            history_down(editor);
            break;

        case KEY_ENTER:
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
