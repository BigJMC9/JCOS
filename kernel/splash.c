#include "splash.h"

#include "boot_logo.h"
#include "font8x8.h"
#include "framebuffer.h"
#include "lib.h"

#define BAR_WIDTH       300U
#define BAR_HEIGHT      8U
#define STATUS_HEIGHT   12U
#define STATUS_GAP      14U
#define STATUS_CHAR_W   8U
#define STATUS_CHAR_H   8U

static u32 g_bar_x;
static u32 g_bar_y;
static u32 g_status_y;
static u32 g_background;
static u32 g_bar_background;
static u32 g_bar_foreground;
static u32 g_status_foreground;

static void draw_logo(void) {
    u32 screen_width = framebuffer_width();
    u32 screen_height = framebuffer_height();

    u32 x =
        screen_width > BOOT_LOGO_WIDTH
            ? (screen_width - BOOT_LOGO_WIDTH) / 2U
            : 0U;

    u32 centered_y =
        screen_height > BOOT_LOGO_HEIGHT
            ? (screen_height - BOOT_LOGO_HEIGHT) / 2U
            : 0U;

    u32 y = centered_y > 50U ? centered_y - 50U : 0U;

    for (u32 row = 0; row < BOOT_LOGO_HEIGHT; ++row) {
        if (y + row >= screen_height) break;

        for (u32 col = 0; col < BOOT_LOGO_WIDTH; ++col) {
            if (x + col >= screen_width) break;

            u32 color =
                boot_logo[
                    row * BOOT_LOGO_WIDTH + col
                ];

            framebuffer_fill_rect(
                x + col,
                y + row,
                1U,
                1U,
                color
            );
        }
    }
}

static void draw_status_glyph(u32 x, u32 y, char c) {
    u8 character = (u8)c;
    if (character >= 128U) character = '?';

    for (u32 row = 0; row < FONT8X8_HEIGHT; ++row) {
        u8 bits = font8x8[character][row];

        for (u32 col = 0; col < FONT8X8_WIDTH; ++col) {
            if (!(bits & (1U << ((FONT8X8_WIDTH - 1U) - col)))) continue;

            framebuffer_fill_rect(
                x + col,
                y + row,
                1U,
                1U,
                g_status_foreground
            );
        }
    }
}

static void draw_progress_width(u32 filled) {
    if (filled > BAR_WIDTH) filled = BAR_WIDTH;

    framebuffer_fill_rect(
        g_bar_x,
        g_bar_y,
        BAR_WIDTH,
        BAR_HEIGHT,
        g_bar_background
    );

    if (filled) {
        framebuffer_fill_rect(
            g_bar_x,
            g_bar_y,
            filled,
            BAR_HEIGHT,
            g_bar_foreground
        );
    }
}

void splash_show(void) {
    u32 width = framebuffer_width();
    u32 height = framebuffer_height();

    g_background = framebuffer_rgb(9, 14, 22);
    g_bar_background = framebuffer_rgb(40, 45, 55);
    g_bar_foreground = framebuffer_rgb(100, 220, 150);
    g_status_foreground = framebuffer_rgb(225, 232, 240);

    framebuffer_fill(g_background);
    draw_logo();

    g_bar_x = width > BAR_WIDTH ? (width - BAR_WIDTH) / 2U : 0U;
    g_bar_y = height / 2U + 110U;

    if (g_bar_y + BAR_HEIGHT >= height) {
        g_bar_y = height > BAR_HEIGHT + STATUS_GAP + STATUS_HEIGHT
            ? height - BAR_HEIGHT - STATUS_GAP - STATUS_HEIGHT
            : 0U;
    }

    g_status_y = g_bar_y + BAR_HEIGHT + STATUS_GAP;

    draw_progress_width(0U);
    splash_status("Starting kernel");
}

void splash_progress(u32 percent) {
    if (percent > 100U) percent = 100U;

    u32 filled =
        (u32)(((u64)BAR_WIDTH * (u64)percent) / 100ULL);

    draw_progress_width(filled);
}

void splash_status(const char *text) {
    u32 width = framebuffer_width();

    if (g_status_y < framebuffer_height()) {
        u32 clear_height = STATUS_HEIGHT;

        if (g_status_y + clear_height > framebuffer_height())
            clear_height = framebuffer_height() - g_status_y;

        framebuffer_fill_rect(
            0U,
            g_status_y,
            width,
            clear_height,
            g_background
        );
    }

    if (!text || !*text || g_status_y + STATUS_CHAR_H > framebuffer_height())
        return;

    u32 max_chars = width / STATUS_CHAR_W;
    if (!max_chars) return;

    u32 length = (u32)k_strlen(text);
    if (length > max_chars) length = max_chars;

    u32 text_width = length * STATUS_CHAR_W;
    u32 x = width > text_width ? (width - text_width) / 2U : 0U;

    for (u32 i = 0; i < length; ++i)
        draw_status_glyph(x + i * STATUS_CHAR_W, g_status_y, text[i]);
}

void splash_update(u32 completed, u32 total, const char *status) {
    if (!total) total = 1U;
    if (completed > total) completed = total;

    u32 filled =
        (u32)(((u64)BAR_WIDTH * (u64)completed) / (u64)total);

    draw_progress_width(filled);
    splash_status(status);
}

void splash_finish(void) {
    splash_update(1U, 1U, "Boot complete");
}
