#include "../include/boot_info.h"

extern void cpu_halt_forever(void);

/* Home-grown 5x7 font. Lowercase is intentionally rendered as uppercase. */
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

static BootInfo *g_boot;

static u32 channel(u8 value, u32 mask) {
    if (!mask) return 0;
    u32 shift = 0;
    while (((mask >> shift) & 1U) == 0U && shift < 31) ++shift;
    u32 max = mask >> shift;
    return (((u32)value * max) / 255U << shift) & mask;
}

static u32 rgb(u8 r, u8 g, u8 b) {
    if (g_boot->framebuffer_pixel_format == 0) return (u32)r | ((u32)g << 8) | ((u32)b << 16);
    if (g_boot->framebuffer_pixel_format == 1) return (u32)b | ((u32)g << 8) | ((u32)r << 16);
    return channel(r, g_boot->framebuffer_red_mask) |
           channel(g, g_boot->framebuffer_green_mask) |
           channel(b, g_boot->framebuffer_blue_mask);
}

static void pixel(u32 x, u32 y, u32 color) {
    if (x >= g_boot->framebuffer_width || y >= g_boot->framebuffer_height) return;
    volatile u32 *fb = (volatile u32 *)(u64)g_boot->framebuffer_base;
    fb[(u64)y * g_boot->framebuffer_pixels_per_scanline + x] = color;
}

static void clear(u32 color) {
    volatile u32 *fb = (volatile u32 *)(u64)g_boot->framebuffer_base;
    for (u32 y = 0; y < g_boot->framebuffer_height; ++y) {
        for (u32 x = 0; x < g_boot->framebuffer_width; ++x) {
            fb[(u64)y * g_boot->framebuffer_pixels_per_scanline + x] = color;
        }
    }
}

static u8 glyph_row(char c, u32 row) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[(u32)(c - 'A')][row];
    if (c >= '0' && c <= '9') return digits[(u32)(c - '0')][row];
    if (c == '!') return (u8[]){4,4,4,4,4,0,4}[row];
    if (c == ':') return (u8[]){0,4,4,0,4,4,0}[row];
    if (c == '-') return (u8[]){0,0,0,31,0,0,0}[row];
    if (c == '_') return (u8[]){0,0,0,0,0,0,31}[row];
    if (c == '.') return (u8[]){0,0,0,0,0,12,12}[row];
    if (c == '/') return (u8[]){1,2,2,4,8,8,16}[row];
    return 0;
}

static void glyph(u32 x, u32 y, char c, u32 color, u32 scale) {
    for (u32 row = 0; row < 7; ++row) {
        u8 bits = glyph_row(c, row);
        for (u32 col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) {
                for (u32 dy = 0; dy < scale; ++dy)
                    for (u32 dx = 0; dx < scale; ++dx)
                        pixel(x + col * scale + dx, y + row * scale + dy, color);
            }
        }
    }
}

static void string(u32 x, u32 y, const char *s, u32 color, u32 scale) {
    u32 origin = x;
    for (; *s; ++s) {
        if (*s == '\n') { x = origin; y += 9 * scale; continue; }
        glyph(x, y, *s, color, scale);
        x += 6 * scale;
    }
}

static void u64_dec(char *out, u64 value) {
    char tmp[21];
    u32 n = 0;
    if (value == 0) { out[0] = '0'; out[1] = 0; return; }
    while (value && n < 20) { tmp[n++] = (char)('0' + value % 10); value /= 10; }
    for (u32 i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static void u64_hex(char *out, u64 value) {
    const char *d = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'X';
    for (u32 i = 0; i < 16; ++i) out[2 + i] = d[(value >> ((15 - i) * 4)) & 0xF];
    out[18] = 0;
}

void kernel_main(BootInfo *boot) {
    g_boot = boot;
    if (!boot || boot->magic != BOOT_INFO_MAGIC) cpu_halt_forever();

    u32 background = rgb(9, 14, 22);
    u32 title = rgb(240, 245, 250);
    u32 accent = rgb(100, 220, 150);
    u32 muted = rgb(170, 185, 200);
    clear(background);

    const u32 x = 32;
    u32 y = 32;
    string(x, y, "HELLO FROM A STANDALONE X86_64 KERNEL!", title, 3); y += 38;
    string(x, y, "UEFI BOOT SERVICES HAVE BEEN EXITED.", accent, 2); y += 32;
    string(x, y, "NO BIOS. NO UEFI CONSOLE. NO LIBC. NO OS UNDERNEATH.", muted, 2); y += 40;

    char n[24];
    string(x, y, "FRAMEBUFFER WIDTH: ", muted, 2);
    u64_dec(n, boot->framebuffer_width); string(x + 19 * 12, y, n, title, 2); y += 22;
    string(x, y, "FRAMEBUFFER HEIGHT: ", muted, 2);
    u64_dec(n, boot->framebuffer_height); string(x + 20 * 12, y, n, title, 2); y += 22;
    string(x, y, "MEMORY MAP ENTRIES: ", muted, 2);
    u64 count = boot->memory_map_descriptor_size ? boot->memory_map_size / boot->memory_map_descriptor_size : 0;
    u64_dec(n, count); string(x + 20 * 12, y, n, title, 2); y += 22;
    string(x, y, "KERNEL PHYSICAL BASE: ", muted, 2);
    u64_hex(n, boot->kernel_base); string(x + 22 * 12, y, n, title, 2); y += 34;

    string(x, y, "CPU HALTED. NEXT: IDT, INTERRUPTS, MEMORY ALLOCATOR, KEYBOARD.", accent, 1);
    cpu_halt_forever();
}
