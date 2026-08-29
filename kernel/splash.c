#include "splash.h"
#include "boot_logo.h"
#include "framebuffer.h"

#define BAR_WIDTH  300U
#define BAR_HEIGHT 8U

static u32 g_bar_x;
static u32 g_bar_y;

static void draw_logo(void) {
    u32 screen_width = framebuffer_width();
    u32 screen_height = framebuffer_height();

    u32 x =
        (screen_width - BOOT_LOGO_WIDTH) / 2U;

    u32 y =
        (screen_height - BOOT_LOGO_HEIGHT) / 2U - 50U;

    for (u32 row = 0; row < BOOT_LOGO_HEIGHT; ++row) {
        for (u32 col = 0; col < BOOT_LOGO_WIDTH; ++col) {
            u32 color =
                boot_logo[
                    row * BOOT_LOGO_WIDTH + col
                ];

            framebuffer_fill_rect(
                x + col,
                y + row,
                1,
                1,
                color
            );
        }
    }
}

void splash_show(void) {
    u32 width = framebuffer_width();
    u32 height = framebuffer_height();

    u32 background =
        framebuffer_rgb(9, 14, 22);

    framebuffer_fill(background);

    draw_logo();

    g_bar_x = (width - BAR_WIDTH) / 2U;
    g_bar_y = height / 2U + 110U;

    framebuffer_fill_rect(
        g_bar_x,
        g_bar_y,
        BAR_WIDTH,
        BAR_HEIGHT,
        framebuffer_rgb(40, 45, 55)
    );
}

void splash_progress(u32 percent) {
    if (percent > 100U)
        percent = 100U;

    u32 filled =
        (BAR_WIDTH * percent) / 100U;

    framebuffer_fill_rect(
        g_bar_x,
        g_bar_y,
        BAR_WIDTH,
        BAR_HEIGHT,
        framebuffer_rgb(40, 45, 55)
    );

    framebuffer_fill_rect(
        g_bar_x,
        g_bar_y,
        filled,
        BAR_HEIGHT,
        framebuffer_rgb(100, 220, 150)
    );
}