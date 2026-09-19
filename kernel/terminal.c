#include "terminal.h"

#include "font8x8.h"
#include "framebuffer.h"
#include "lib.h"
#include "serial.h"

#define TERMINAL_SCROLLBACK_LINES 512U
#define TERMINAL_MAX_COLUMNS      256U

static u32 g_background;
static u32 g_foreground;
static u32 g_default;
static u32 g_accent;
static u32 g_error;

static u32 g_scale;
static u32 g_cell_width;
static u32 g_cell_height;
static u32 g_margin;
static u32 g_columns;
static u32 g_rows;

static char g_chars[TERMINAL_SCROLLBACK_LINES][TERMINAL_MAX_COLUMNS];
static u32 g_colors[TERMINAL_SCROLLBACK_LINES][TERMINAL_MAX_COLUMNS];

static u64 g_first_line;
static u64 g_last_line;
static u64 g_cursor_line;
static u64 g_view_top;
static u32 g_cursor_column;

static bool g_follow_output;
static bool g_ready;
static bool g_cursor_enabled;
static bool g_cursor_visible;

static u32 line_slot(u64 line) {
    return (u32)(line % TERMINAL_SCROLLBACK_LINES);
}

static bool line_retained(u64 line) {
    return line >= g_first_line && line <= g_last_line;
}

static bool line_visible(u64 line) {
    return g_ready && line >= g_view_top && line < g_view_top + (u64)g_rows;
}

static u32 visible_row(u64 line) {
    return (u32)(line - g_view_top);
}

static u32 cell_x(u32 column) {
    return g_margin + column * g_cell_width;
}

static u32 cell_y(u32 row) {
    return g_margin + row * g_cell_height;
}

static void draw_glyph(u32 x, u32 y, char c, u32 color) {
    u8 character = (u8)c;
    if (character >= 128U) character = '?';

    for (u32 row = 0; row < FONT8X8_HEIGHT; ++row) {
        u8 bits = font8x8[character][row];

        for (u32 col = 0; col < FONT8X8_WIDTH; ++col) {
            if (!(bits & (1U << ((FONT8X8_WIDTH - 1U) - col)))) continue;

            framebuffer_fill_rect(
                x + col * g_scale,
                y + row * g_scale,
                g_scale,
                g_scale,
                color
            );
        }
    }
}

static void clear_line(u64 line) {
    u32 slot = line_slot(line);
    k_memset(g_chars[slot], 0, sizeof(g_chars[slot]));
}

static void render_cell(u64 line, u32 column) {
    if (!line_visible(line) || column >= g_columns) return;

    u32 row = visible_row(line);
    u32 x = cell_x(column);
    u32 y = cell_y(row);

    framebuffer_fill_rect(x, y, g_cell_width, g_cell_height, g_background);

    if (!line_retained(line)) return;

    u32 slot = line_slot(line);
    char c = g_chars[slot][column];

    if ((u8)c >= 32U && (u8)c <= 126U)
        draw_glyph(x, y, c, g_colors[slot][column]);
}

static void render_viewport(void) {
    if (!g_ready) return;

    framebuffer_fill(g_background);

    for (u32 row = 0; row < g_rows; ++row) {
        u64 line = g_view_top + (u64)row;
        if (!line_retained(line)) continue;

        u32 slot = line_slot(line);

        for (u32 column = 0; column < g_columns; ++column) {
            char c = g_chars[slot][column];
            if ((u8)c < 32U || (u8)c > 126U) continue;

            draw_glyph(
                cell_x(column),
                cell_y(row),
                c,
                g_colors[slot][column]
            );
        }
    }
}

static u64 live_view_top(void) {
    u64 top = 0;

    if (g_last_line + 1ULL > (u64)g_rows)
        top = g_last_line + 1ULL - (u64)g_rows;

    if (top < g_first_line) top = g_first_line;
    return top;
}

static void hide_cursor_overlay(void) {
    if (!g_cursor_visible) return;

    if (g_ready && line_visible(g_cursor_line) && g_cursor_column < g_columns)
        render_cell(g_cursor_line, g_cursor_column);

    g_cursor_visible = false;
}

static void show_cursor_overlay(void) {
    if (!g_cursor_enabled || g_cursor_visible) return;

    if (g_ready && line_visible(g_cursor_line) && g_cursor_column < g_columns) {
        u32 row = visible_row(g_cursor_line);
        u32 width = g_scale * 2U;
        if (!width) width = 1U;

        framebuffer_fill_rect(
            cell_x(g_cursor_column),
            cell_y(row) + g_scale,
            width,
            FONT8X8_HEIGHT * g_scale,
            g_default
        );
    }

    g_cursor_visible = true;
}

static void keep_view_valid(void) {
    if (g_view_top < g_first_line) {
        g_view_top = g_first_line;
        if (!g_follow_output) render_viewport();
    }
}

static void retain_new_line(u64 line) {
    if (line <= g_last_line) return;

    g_last_line = line;

    while (g_last_line - g_first_line + 1ULL > TERMINAL_SCROLLBACK_LINES)
        ++g_first_line;

    clear_line(line);
    keep_view_valid();
}

static void advance_line(void) {
    bool cursor_was_visible = g_cursor_visible;
    hide_cursor_overlay();

    ++g_cursor_line;
    g_cursor_column = 0;
    retain_new_line(g_cursor_line);

    if (g_follow_output) {
        u64 new_top = live_view_top();

        if (new_top == g_view_top + 1ULL && g_rows > 1U) {
            framebuffer_scroll_up(g_cell_height, g_background);
            g_view_top = new_top;
        } else if (new_top != g_view_top) {
            g_view_top = new_top;
            render_viewport();
        }
    }

    if (cursor_was_visible) show_cursor_overlay();
}

bool terminal_init(void) {
    if (!framebuffer_width() || !framebuffer_height()) return false;

    g_scale = 1U;
    g_cell_width = (FONT8X8_WIDTH + 1U) * g_scale;
    g_cell_height = (FONT8X8_HEIGHT + 2U) * g_scale;
    g_margin = 4U * g_scale;

    g_background = framebuffer_rgb(9, 14, 22);
    g_default = framebuffer_rgb(225, 232, 240);
    g_accent = framebuffer_rgb(100, 220, 150);
    g_error = framebuffer_rgb(255, 120, 120);
    g_foreground = g_default;

    u32 usable_width = framebuffer_width() > 2U * g_margin
        ? framebuffer_width() - 2U * g_margin
        : 0U;

    u32 usable_height = framebuffer_height() > 2U * g_margin
        ? framebuffer_height() - 2U * g_margin
        : 0U;

    g_columns = g_cell_width ? usable_width / g_cell_width : 0U;
    g_rows = g_cell_height ? usable_height / g_cell_height : 0U;

    if (!g_columns || !g_rows) return false;
    if (g_columns > TERMINAL_MAX_COLUMNS) g_columns = TERMINAL_MAX_COLUMNS;

    g_ready = true;
    terminal_clear();
    return true;
}

void terminal_clear(void) {
    bool cursor_was_enabled = g_cursor_enabled;

    g_cursor_enabled = false;
    g_cursor_visible = false;

    k_memset(g_chars, 0, sizeof(g_chars));
    k_memset(g_colors, 0, sizeof(g_colors));

    g_first_line = 0;
    g_last_line = 0;
    g_cursor_line = 0;
    g_cursor_column = 0;
    g_view_top = 0;
    g_follow_output = true;

    if (g_ready) framebuffer_fill(g_background);
    serial_clear();

    g_cursor_enabled = cursor_was_enabled;
}

void terminal_set_color(u32 color) {
    g_foreground = color;
}

u32 terminal_default_color(void) {
    return g_default;
}

u32 terminal_accent_color(void) {
    return g_accent;
}

u32 terminal_error_color(void) {
    return g_error;
}

void terminal_putchar(char c) {
    bool cursor_was_visible = g_cursor_visible;
    hide_cursor_overlay();

    if (c == '\b') {
        serial_write("\b \b");

        if (g_cursor_column) {
            --g_cursor_column;
            u32 slot = line_slot(g_cursor_line);
            g_chars[slot][g_cursor_column] = 0;
            render_cell(g_cursor_line, g_cursor_column);
        }

        if (cursor_was_visible) show_cursor_overlay();
        return;
    }

    if (c == '\t') {
        if (cursor_was_visible) show_cursor_overlay();
        for (u32 i = 0; i < 4U; ++i) terminal_putchar(' ');
        return;
    }

    serial_putc(c);

    if (!g_ready) {
        if (cursor_was_visible) show_cursor_overlay();
        return;
    }

    if (c == '\r') {
        g_cursor_column = 0;
        if (cursor_was_visible) show_cursor_overlay();
        return;
    }

    if (c == '\n') {
        if (cursor_was_visible) show_cursor_overlay();
        advance_line();
        return;
    }

    if ((u8)c < 32U || (u8)c > 126U) {
        if (cursor_was_visible) show_cursor_overlay();
        return;
    }

    if (g_cursor_column >= g_columns) advance_line();

    u32 slot = line_slot(g_cursor_line);
    g_chars[slot][g_cursor_column] = c;
    g_colors[slot][g_cursor_column] = g_foreground;
    render_cell(g_cursor_line, g_cursor_column);

    ++g_cursor_column;

    if (g_cursor_column >= g_columns) advance_line();

    if (cursor_was_visible) show_cursor_overlay();
}

void terminal_write(const char *s) {
    if (!s) return;
    while (*s) terminal_putchar(*s++);
}

void terminal_writeln(const char *s) {
    terminal_write(s);
    terminal_putchar('\n');
}

void terminal_write_u64(u64 value) {
    char buffer[21];
    u32 count = 0;

    if (!value) {
        terminal_putchar('0');
        return;
    }

    while (value && count < 20U) {
        buffer[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }

    while (count) terminal_putchar(buffer[--count]);
}

void terminal_write_hex(u64 value) {
    static const char digits_hex[] = "0123456789ABCDEF";

    terminal_write("0x");

    bool started = false;

    for (s32 shift = 60; shift >= 0; shift -= 4) {
        u8 digit = (u8)((value >> (u32)shift) & 0xFULL);

        if (digit || started || !shift) {
            terminal_putchar(digits_hex[digit]);
            started = true;
        }
    }
}

bool terminal_cursor_left(void) {
    bool cursor_was_visible = g_cursor_visible;
    hide_cursor_overlay();

    bool moved = false;

    if (g_cursor_column) {
        --g_cursor_column;
        moved = true;
    } else if (g_cursor_line > g_first_line) {
        --g_cursor_line;
        g_cursor_column = g_columns - 1U;
        moved = true;
    }

    if (moved) serial_write("\b");
    if (cursor_was_visible) show_cursor_overlay();
    return moved;
}

bool terminal_cursor_right(void) {
    bool cursor_was_visible = g_cursor_visible;
    hide_cursor_overlay();

    bool moved = false;

    if (g_cursor_column + 1U < g_columns) {
        ++g_cursor_column;
        moved = true;
    } else if (g_cursor_line < g_last_line) {
        ++g_cursor_line;
        g_cursor_column = 0;
        moved = true;
    }

    if (moved) serial_write("\x1B[C");
    if (cursor_was_visible) show_cursor_overlay();
    return moved;
}

void terminal_cursor_enable(bool enabled) {
    if (!enabled) {
        hide_cursor_overlay();
        g_cursor_enabled = false;
        return;
    }

    g_cursor_enabled = true;
}

void terminal_cursor_set_visible(bool visible) {
    if (!g_cursor_enabled) {
        g_cursor_visible = false;
        return;
    }

    if (visible) show_cursor_overlay();
    else hide_cursor_overlay();
}

void terminal_cursor_toggle(void) {
    if (!g_cursor_enabled) return;

    if (g_cursor_visible) hide_cursor_overlay();
    else show_cursor_overlay();
}

bool terminal_cursor_visible(void) {
    return g_cursor_enabled && g_cursor_visible;
}

void terminal_scrollback_page_up(void) {
    if (!g_ready || g_view_top <= g_first_line) return;

    hide_cursor_overlay();

    u64 step = g_rows > 1U ? (u64)(g_rows - 1U) : 1ULL;
    u64 distance = g_view_top - g_first_line;

    g_view_top = distance > step ? g_view_top - step : g_first_line;
    g_follow_output = false;
    render_viewport();
}

void terminal_scrollback_page_down(void) {
    if (!g_ready) return;

    u64 bottom = live_view_top();
    if (g_view_top >= bottom) {
        g_view_top = bottom;
        g_follow_output = true;
        render_viewport();
        return;
    }

    hide_cursor_overlay();

    u64 step = g_rows > 1U ? (u64)(g_rows - 1U) : 1ULL;
    u64 remaining = bottom - g_view_top;

    g_view_top = remaining > step ? g_view_top + step : bottom;
    g_follow_output = g_view_top == bottom;
    render_viewport();

    if (g_follow_output && g_cursor_enabled && g_cursor_visible)
        show_cursor_overlay();
}

void terminal_scrollback_to_bottom(void) {
    if (!g_ready) return;

    u64 bottom = live_view_top();

    if (g_follow_output && g_view_top == bottom) return;

    bool cursor_should_show = g_cursor_enabled && g_cursor_visible;
    hide_cursor_overlay();

    g_view_top = bottom;
    g_follow_output = true;
    render_viewport();

    if (cursor_should_show) show_cursor_overlay();
}

bool terminal_scrollback_active(void) {
    return g_ready && (!g_follow_output || g_view_top != live_view_top());
}
