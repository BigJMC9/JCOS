#include "terminal.h"
#include "framebuffer.h"
#include "serial.h"

/* Home-grown 5x7 font. Lowercase is displayed as uppercase. */

static const u8 letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

static const u8 digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};

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

static u8 punctuation(char c, u32 row) {
    static const u8 exclamation[7] = {4,4,4,4,4,0,4};
    static const u8 colon[7]       = {0,4,4,0,4,4,0};
    static const u8 semicolon[7]   = {0,4,4,0,4,4,8};
    static const u8 dash[7]        = {0,0,0,31,0,0,0};
    static const u8 underscore[7]  = {0,0,0,0,0,0,31};
    static const u8 dot[7]         = {0,0,0,0,0,12,12};
    static const u8 comma[7]       = {0,0,0,0,0,4,8};
    static const u8 slash[7]       = {1,2,2,4,8,8,16};
    static const u8 backslash[7]   = {16,8,8,4,2,2,1};
    static const u8 greater[7]     = {16,8,4,2,4,8,16};
    static const u8 less[7]        = {1,2,4,8,4,2,1};
    static const u8 equal[7]       = {0,31,0,31,0,0,0};
    static const u8 plus[7]        = {0,4,4,31,4,4,0};
    static const u8 question[7]    = {14,17,1,2,4,0,4};
    static const u8 lparen[7]      = {2,4,8,8,8,4,2};
    static const u8 rparen[7]      = {8,4,2,2,2,4,8};
    static const u8 lbracket[7]    = {14,8,8,8,8,8,14};
    static const u8 rbracket[7]    = {14,2,2,2,2,2,14};
    static const u8 quote[7]       = {10,10,0,0,0,0,0};
    static const u8 apostrophe[7]  = {4,4,0,0,0,0,0};
    static const u8 hash[7]        = {10,31,10,10,31,10,0};
    static const u8 percent[7]     = {25,25,2,4,8,19,19};
    static const u8 star[7]        = {0,21,14,31,14,21,0};
    static const u8 at[7]          = {14,17,23,21,23,16,14};
    static const u8 pipe[7]        = {4,4,4,4,4,4,4};
    static const u8 caret[7]       = {4,10,17,0,0,0,0};
    const u8 *p = 0;
    switch (c) {
        case '!': p = exclamation; break; case ':': p = colon; break;
        case ';': p = semicolon; break; case '-': p = dash; break;
        case '_': p = underscore; break; case '.': p = dot; break;
        case ',': p = comma; break; case '/': p = slash; break;
        case '\\': p = backslash; break; case '>': p = greater; break;
        case '<': p = less; break; case '=': p = equal; break;
        case '+': p = plus; break; case '?': p = question; break;
        case '(': p = lparen; break; case ')': p = rparen; break;
        case '[': p = lbracket; break; case ']': p = rbracket; break;
        case '"': p = quote; break; case '\'': p = apostrophe; break;
        case '#': p = hash; break; case '%': p = percent; break;
        case '*': p = star; break; case '@': p = at; break;
        case '|': p = pipe; break; case '^': p = caret; break;
        default: break;
    }
    return p ? p[row] : 0;
}

static u8 glyph_row(char c, u32 row) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[(u32)(c - 'A')][row];
    if (c >= '0' && c <= '9') return digits[(u32)(c - '0')][row];
    return punctuation(c, row);
}

static void draw_glyph(u32 x, u32 y, char c, u32 color) {
    for (u32 row = 0; row < 7; ++row) {
        u8 bits = glyph_row(c, row);
        for (u32 col = 0; col < 5; ++col) {
            if (!(bits & (1U << (4 - col)))) continue;
            framebuffer_fill_rect(x + col * g_scale, y + row * g_scale,
                                  g_scale, g_scale, color);
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
    g_scale = (framebuffer_width() >= 800 && framebuffer_height() >= 600) ? 2U : 1U;
    g_cell_width = 6 * g_scale;
    g_cell_height = 9 * g_scale;
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
            framebuffer_fill_rect(g_x, g_y, g_cell_width, g_cell_height, g_background);
        }
        return;
    }

    serial_putc(c);
    if (!g_ready) return;
    if (c == '\r') { g_x = g_margin; return; }
    if (c == '\n') { newline_framebuffer(); return; }
    if (c == '\t') {
        for (u32 i = 0; i < 4; ++i) terminal_putchar(' ');
        return;
    }
    if ((u8)c < 32 || (u8)c > 126) return;

    if (g_x + g_cell_width + g_margin > framebuffer_width()) newline_framebuffer();
    framebuffer_fill_rect(g_x, g_y, g_cell_width, g_cell_height, g_background);
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
