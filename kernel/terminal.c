#include "terminal.h"
#include "framebuffer.h"
#include "serial.h"
#include "font8x8.h"

static u32 g_background;
static u32 g_foreground;
static u32 g_default;
static u32 g_accent;
static u32 g_error;
static u32 g_scale;
static u32 g_cell_width;
static u32 g_cell_height;
static u32 g_margin;
static u32 g_x;
static u32 g_y;
static bool g_ready;

static void draw_glyph(u32 x, u32 y, char c, u32 color) {
    u8 character = (u8)c;
    if (character >= 128)
        character = '?';

    for (u32 row = 0; row < FONT8X8_HEIGHT; ++row) {
        u8 bits = font8x8[character][row];
        for (u32 col = 0; col < FONT8X8_WIDTH; ++col) {
            if (bits & (1U << ((FONT8X8_WIDTH - 1U) - col))) {
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
}

static void ensure_room(void) {
    if (g_y + g_cell_height + g_margin <= framebuffer_height()) return;
    framebuffer_scroll_up(g_cell_height, g_background);
    if (g_y >= g_cell_height) g_y -= g_cell_height;
}

static void newline_framebuffer(void) {
    g_x = g_margin;
    g_y += g_cell_height;
    ensure_room();
}

bool terminal_init(void) {
    if (!framebuffer_width() || !framebuffer_height()) return false;
    g_scale = 1U;
    g_cell_width = (FONT8X8_WIDTH + 1U) * g_scale;
    g_cell_height = (FONT8X8_HEIGHT + 2U) * g_scale;
    g_margin = 4 * g_scale;
    g_background = framebuffer_rgb(9, 14, 22);
    g_default = framebuffer_rgb(225, 232, 240);
    g_accent = framebuffer_rgb(100, 220, 150);
    g_error = framebuffer_rgb(255, 120, 120);
    g_foreground = g_default;
    g_ready = true;
    terminal_clear();
    return true;
}

void terminal_clear(void) {
    if (g_ready) framebuffer_fill(g_background);
    g_x = g_margin;
    g_y = g_margin;
    serial_clear();
}

void terminal_set_color(u32 color) { g_foreground = color; }
u32 terminal_default_color(void) { return g_default; }
u32 terminal_accent_color(void) { return g_accent; }
u32 terminal_error_color(void) { return g_error; }

void terminal_putchar(char c) {
    if (c == '\b') {
        serial_write("\b \b");

        if (g_ready && g_x > g_margin) {
            g_x -= g_cell_width;
            framebuffer_fill_rect(
                g_x,
                g_y,
                g_cell_width,
                g_cell_height,
                g_background
            );
        }

        return;
    }

    if (c == '\t') {
        for (u32 i = 0; i < 4; ++i)
            terminal_putchar(' ');

        return;
    }

    serial_putc(c);

    if (!g_ready)
        return;

    if (c == '\r') {
        g_x = g_margin;
        return;
    }

    if (c == '\n') {
        newline_framebuffer();
        return;
    }

    if ((u8)c < 32 || (u8)c > 126)
        return;

    if (g_x + g_cell_width + g_margin > framebuffer_width())
        newline_framebuffer();

    framebuffer_fill_rect(
        g_x,
        g_y,
        g_cell_width,
        g_cell_height,
        g_background
    );

    draw_glyph(g_x, g_y, c, g_foreground);
    g_x += g_cell_width;
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
    char buf[21];
    u32 count = 0;
    if (value == 0) { terminal_putchar('0'); return; }
    while (value && count < 20) {
        buf[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (count) terminal_putchar(buf[--count]);
}

void terminal_write_hex(u64 value) {
    static const char digits_hex[] = "0123456789ABCDEF";
    terminal_write("0x");
    bool started = false;
    for (s32 shift = 60; shift >= 0; shift -= 4) {
        u8 digit = (u8)((value >> (u32)shift) & 0xF);
        if (digit || started || shift == 0) {
            terminal_putchar(digits_hex[digit]);
            started = true;
        }
    }
}
