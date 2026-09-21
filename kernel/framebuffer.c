#include "framebuffer.h"

static const BootInfo *g_boot;

static u32 channel(u8 value, u32 mask) {
    if (!mask) return 0;
    u32 shift = 0;
    while (shift < 31 && ((mask >> shift) & 1U) == 0U) ++shift;
    u32 max = mask >> shift;
    return ((((u32)value * max) / 255U) << shift) & mask;
}

bool framebuffer_init(const BootInfo *boot) {
    if (!boot || !boot->framebuffer_base || !boot->framebuffer_width ||
        !boot->framebuffer_height || !boot->framebuffer_pixels_per_scanline ||
        boot->framebuffer_pixels_per_scanline < boot->framebuffer_width)
        return false;
    if (boot->framebuffer_pixel_format > 2) return false; /* PixelBltOnly/unknown. */
    if (boot->framebuffer_pixel_format == 2 &&
        !(boot->framebuffer_red_mask | boot->framebuffer_green_mask | boot->framebuffer_blue_mask))
        return false;
    u64 last_pixel = (u64)(boot->framebuffer_height - 1) * boot->framebuffer_pixels_per_scanline +
                     boot->framebuffer_width;
    if (last_pixel > boot->framebuffer_size / sizeof(u32)) return false;
    g_boot = boot;
    return true;
}

u32 framebuffer_rgb(u8 r, u8 g, u8 b) {
    if (!g_boot) return 0;
    if (g_boot->framebuffer_pixel_format == 0)
        return (u32)r | ((u32)g << 8) | ((u32)b << 16);
    if (g_boot->framebuffer_pixel_format == 1)
        return (u32)b | ((u32)g << 8) | ((u32)r << 16);
    return channel(r, g_boot->framebuffer_red_mask) |
           channel(g, g_boot->framebuffer_green_mask) |
           channel(b, g_boot->framebuffer_blue_mask);
}

void framebuffer_pixel(u32 x, u32 y, u32 color) {
    if (!g_boot || x >= g_boot->framebuffer_width || y >= g_boot->framebuffer_height) return;
    volatile u32 *fb = (volatile u32 *)(u64)g_boot->framebuffer_base;
    fb[(u64)y * g_boot->framebuffer_pixels_per_scanline + x] = color;
}

void framebuffer_fill(u32 color) {
    if (!g_boot) return;
    u32 *fb = (u32 *)(u64)g_boot->framebuffer_base;
    u32 pitch = g_boot->framebuffer_pixels_per_scanline;
    for (u32 y = 0; y < g_boot->framebuffer_height; ++y) {
        u32 *row = fb + (u64)y * pitch;
        u32 x = 0;
        if (((u64)row & 7ULL) == 0) {
            u64 *wide = (u64 *)(void *)row;
            u64 pair = (u64)color | ((u64)color << 32);
            for (; x + 1U < g_boot->framebuffer_width; x += 2U) wide[x >> 1] = pair;
        }
        for (; x < g_boot->framebuffer_width; ++x) row[x] = color;
    }
}

void framebuffer_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color) {
    if (!g_boot) return;
    u32 x_end = x + width;
    u32 y_end = y + height;
    if (x_end < x || x_end > g_boot->framebuffer_width) x_end = g_boot->framebuffer_width;
    if (y_end < y || y_end > g_boot->framebuffer_height) y_end = g_boot->framebuffer_height;
    u32 *fb = (u32 *)(u64)g_boot->framebuffer_base;
    u32 pitch = g_boot->framebuffer_pixels_per_scanline;
    for (u32 row = y; row < y_end; ++row) {
        u32 *line = fb + (u64)row * pitch + x;
        u32 col = x;
        if (((u64)line & 7ULL) == 0) {
            u64 *wide = (u64 *)(void *)line;
            u64 pair = (u64)color | ((u64)color << 32);
            for (; col + 1U < x_end; col += 2U) wide[(col - x) >> 1] = pair;
        }
        for (; col < x_end; ++col) line[col - x] = color;
    }
}

void framebuffer_scroll_up(u32 rows, u32 fill_color) {
    if (!g_boot || rows == 0) return;
    if (rows >= g_boot->framebuffer_height) {
        framebuffer_fill(fill_color);
        return;
    }
    volatile u32 *fb = (volatile u32 *)(u64)g_boot->framebuffer_base;
    u32 pitch = g_boot->framebuffer_pixels_per_scanline;
    u32 height = g_boot->framebuffer_height;
    u32 width = g_boot->framebuffer_width;
    for (u32 y = 0; y < height - rows; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fb[(u64)(y + rows) * pitch + x];
    for (u32 y = height - rows; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fill_color;
}

void framebuffer_blit_rgb332_row(u32 x, u32 y, u32 width,
    const u8 *source, u32 source_width) {
    if (!g_boot || !source || !source_width || !width ||
        x >= g_boot->framebuffer_width || y >= g_boot->framebuffer_height) return;
    if (width > g_boot->framebuffer_width - x) width = g_boot->framebuffer_width - x;
    volatile u32 *fb = (volatile u32 *)(u64)g_boot->framebuffer_base;
    volatile u32 *row = fb + (u64)y * g_boot->framebuffer_pixels_per_scanline + x;
    for (u32 column = 0; column < width; ++column) {
        u32 source_x = (u32)(((u64)column * source_width) / width);
        u8 pixel = source[source_x];
        u8 r = (u8)(((pixel >> 5) & 7U) * 255U / 7U);
        u8 g = (u8)(((pixel >> 2) & 7U) * 255U / 7U);
        u8 b = (u8)((pixel & 3U) * 255U / 3U);
        row[column] = framebuffer_rgb(r, g, b);
    }
}

u32 framebuffer_width(void) { return g_boot ? g_boot->framebuffer_width : 0; }
u32 framebuffer_height(void) { return g_boot ? g_boot->framebuffer_height : 0; }
