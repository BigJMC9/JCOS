#ifndef JA_OS_SHELL_EDITOR_H
#define JA_OS_SHELL_EDITOR_H

#include "key_event.h"
#include "types.h"

#define SHELL_EDITOR_CAPACITY 128U
#define SHELL_HISTORY_CAPACITY 64U

typedef enum {
    SHELL_EDITOR_CONTINUE = 0,
    SHELL_EDITOR_SUBMIT
} ShellEditorResult;

typedef struct {
    char buffer[SHELL_EDITOR_CAPACITY];
    u32 length;
    u32 cursor;
    u32 rendered_length;
    u32 rendered_cursor;

    char history[SHELL_HISTORY_CAPACITY][SHELL_EDITOR_CAPACITY];
    u32 history_count;
    u32 history_next;
    s32 history_offset;

    char draft[SHELL_EDITOR_CAPACITY];
    u32 draft_length;
    bool draft_saved;
} ShellEditor;

void shell_editor_init(ShellEditor *editor);
void shell_editor_reset_line(ShellEditor *editor);
ShellEditorResult shell_editor_handle(ShellEditor *editor, const KeyEvent *event);
const char *shell_editor_line(ShellEditor *editor);

#endif
