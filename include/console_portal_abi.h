#ifndef JCOS_CONSOLE_PORTAL_ABI_H
#define JCOS_CONSOLE_PORTAL_ABI_H

/* Privileged terminal mechanism ABI. This endpoint is granted only to the
 * console policy service; ordinary applications use the versioned public
 * console protocols instead. Policy (history, editing, selection, clipboard,
 * command parsing) remains in Ring3. */
#define JCOS_CONSOLE_PORTAL_WRITE_BYTE          1ULL
#define JCOS_CONSOLE_PORTAL_CURSOR_LEFT         2ULL
#define JCOS_CONSOLE_PORTAL_SCROLL_PAGE_UP      3ULL
#define JCOS_CONSOLE_PORTAL_SCROLL_PAGE_DOWN    4ULL
#define JCOS_CONSOLE_PORTAL_SCROLL_TO_BOTTOM    5ULL
#define JCOS_CONSOLE_PORTAL_WRITE_ACCENT_BYTE   6ULL
#define JCOS_CONSOLE_PORTAL_SCROLL_LINE_UP      7ULL
#define JCOS_CONSOLE_PORTAL_SCROLL_LINE_DOWN    8ULL

#define JCOS_CONSOLE_PORTAL_MAX_BYTE            0xFFULL

#endif
