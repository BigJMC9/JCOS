#include "framebuffer.h"

#include "lib.h"
#include "vmm.h"

#define EFI_RESERVED_MEMORY_TYPE 0U
#define EFI_MEMORY_MAPPED_IO 11U

#define FRAMEBUFFER_MEMORY_TYPE_MIXED       0xFFFFFFFEU
#define FRAMEBUFFER_MEMORY_TYPE_UNDESCRIBED 0xFFFFFFFFU

static FramebufferInfo g_framebuffer;
static bool g_ready;

static bool direct_span_is_mmio(const BootInfo *boot, u64 start, u64 size, u32 *out_type) {
    if (out_type) *out_type = 0U;
    if (!boot || !start || !size || !boot->memory_map || !boot->memory_map_size ||
        boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor) ||
        start > ~0ULL - size) return false;

    u64 end = start + size;
    u64 cursor = start;
    u32 observed_type = 0U;

    while (cursor < end) {
        const BootMemoryDescriptor *cover = 0;
        u64 cover_end = 0ULL;

        for (u64 offset = 0;
             offset + sizeof(BootMemoryDescriptor) <= boot->memory_map_size;
             offset += boot->memory_map_descriptor_size) {
            const BootMemoryDescriptor *descriptor =
                (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);
            if (!descriptor->number_of_pages ||
                descriptor->number_of_pages > ~0ULL / VM_PAGE_SIZE) continue;

            u64 bytes = descriptor->number_of_pages * VM_PAGE_SIZE;
            if (descriptor->physical_start > ~0ULL - bytes) continue;
            u64 descriptor_end = descriptor->physical_start + bytes;
            if (cursor < descriptor->physical_start || cursor >= descriptor_end) continue;

            cover = descriptor;
            cover_end = descriptor_end;
            break;
        }

        if (!cover || cover->type != EFI_MEMORY_MAPPED_IO) return false;
        if (!observed_type) observed_type = cover->type;
        if (cover_end > end) cover_end = end;
        if (cover_end <= cursor) return false;
        cursor = cover_end;
    }

    if (out_type) *out_type = observed_type;
    return cursor == end;
}

static bool direct_span_is_safe(const BootInfo *boot, u64 framebuffer_base, u64 framebuffer_size, u64 mapping_base, u64 mapping_size, u32 *out_type) {
    if (out_type) *out_type = 0U;

    /* The strongest case remains unchanged: firmware explicitly describes the
     * complete page-rounded range as MMIO. */
    if (direct_span_is_mmio(boot, mapping_base, mapping_size, out_type)) return true;

    /* UEFI memory maps describe system memory and are not required to contain
     * every PCI BAR. GOP's FrameBufferBase/FrameBufferSize are authoritative for
     * the framebuffer byte range itself. We may therefore accept an undescribed
     * PCI aperture only when that byte range is already page-exact, so granting
     * it to Ring3 cannot expose bytes before or after the GOP resource. */
    if (framebuffer_base != mapping_base || framebuffer_size != mapping_size ||
        !boot || !boot->memory_map || !boot->memory_map_size ||
        boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor) ||
        mapping_base > ~0ULL - mapping_size) return false;

    u64 mapping_end = mapping_base + mapping_size;
    bool observed = false;
    u32 observed_type = FRAMEBUFFER_MEMORY_TYPE_UNDESCRIBED;

    for (u64 offset = 0;
         offset + sizeof(BootMemoryDescriptor) <= boot->memory_map_size;
         offset += boot->memory_map_descriptor_size) {
        const BootMemoryDescriptor *descriptor =
            (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);
        if (!descriptor->number_of_pages ||
            descriptor->number_of_pages > ~0ULL / VM_PAGE_SIZE) continue;

        u64 bytes = descriptor->number_of_pages * VM_PAGE_SIZE;
        if (descriptor->physical_start > ~0ULL - bytes) continue;
        u64 descriptor_end = descriptor->physical_start + bytes;
        if (descriptor_end <= mapping_base || descriptor->physical_start >= mapping_end) continue;

        /* Any overlap with RAM-backed or firmware-owned memory remains a hard
         * rejection. Reserved is accepted only in the page-exact case above;
         * firmware commonly uses it for PCI display apertures. */
        if (descriptor->type != EFI_RESERVED_MEMORY_TYPE &&
            descriptor->type != EFI_MEMORY_MAPPED_IO) return false;

        if (!observed) {
            observed_type = descriptor->type;
            observed = true;
        } else if (observed_type != descriptor->type) {
            observed_type = FRAMEBUFFER_MEMORY_TYPE_MIXED;
        }
    }

    if (out_type) *out_type = observed ? observed_type : FRAMEBUFFER_MEMORY_TYPE_UNDESCRIBED;
    return true;
}

static u32 channel(u8 value, u32 mask) {
    if (!mask) return 0;
    u32 shift = 0;
    while (shift < 31 && ((mask >> shift) & 1U) == 0U) ++shift;
    u32 max = mask >> shift;
    return ((((u32)value * max) / 255U) << shift) & mask;
}

bool framebuffer_init(const BootInfo *boot) {
    g_ready = false;
    k_memset(&g_framebuffer, 0, sizeof(g_framebuffer));

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

    if (boot->framebuffer_base > ~0ULL - (boot->framebuffer_size - 1ULL)) return false;

    u64 page_offset = boot->framebuffer_base & (VM_PAGE_SIZE - 1ULL);
    u64 mapping_base = boot->framebuffer_base - page_offset;
    if (boot->framebuffer_size > ~0ULL - page_offset) return false;
    u64 covering_bytes = boot->framebuffer_size + page_offset;
    if (covering_bytes > ~0ULL - (VM_PAGE_SIZE - 1ULL)) return false;
    u64 mapping_size = (covering_bytes + VM_PAGE_SIZE - 1ULL) & ~(VM_PAGE_SIZE - 1ULL);
    if (!mapping_size || mapping_base > ~0ULL - (mapping_size - 1ULL)) return false;

    g_framebuffer.physical_base = boot->framebuffer_base;
    g_framebuffer.size = boot->framebuffer_size;
    g_framebuffer.mapping_physical_base = mapping_base;
    g_framebuffer.mapping_size = mapping_size;
    g_framebuffer.page_offset = page_offset;
    g_framebuffer.direct_map_safe = direct_span_is_safe(boot, boot->framebuffer_base,
        boot->framebuffer_size, mapping_base, mapping_size, &g_framebuffer.memory_type);
    g_framebuffer.width = boot->framebuffer_width;
    g_framebuffer.height = boot->framebuffer_height;
    g_framebuffer.pixels_per_scanline = boot->framebuffer_pixels_per_scanline;
    g_framebuffer.pixel_format = boot->framebuffer_pixel_format;
    g_framebuffer.red_mask = boot->framebuffer_red_mask;
    g_framebuffer.green_mask = boot->framebuffer_green_mask;
    g_framebuffer.blue_mask = boot->framebuffer_blue_mask;
    g_framebuffer.reserved_mask = boot->framebuffer_reserved_mask;
    g_ready = true;
    return true;
}

bool framebuffer_info(FramebufferInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_ready) return false;
    *out = g_framebuffer;
    return true;
}

u32 framebuffer_rgb(u8 r, u8 g, u8 b) {
    if (!g_ready) return 0;
    if (g_framebuffer.pixel_format == 0)
        return (u32)r | ((u32)g << 8) | ((u32)b << 16);
    if (g_framebuffer.pixel_format == 1)
        return (u32)b | ((u32)g << 8) | ((u32)r << 16);
    return channel(r, g_framebuffer.red_mask) |
           channel(g, g_framebuffer.green_mask) |
           channel(b, g_framebuffer.blue_mask);
}

void framebuffer_pixel(u32 x, u32 y, u32 color) {
    if (!g_ready || x >= g_framebuffer.width || y >= g_framebuffer.height) return;
    volatile u32 *fb = (volatile u32 *)(u64)g_framebuffer.physical_base;
    fb[(u64)y * g_framebuffer.pixels_per_scanline + x] = color;
}

void framebuffer_fill(u32 color) {
    if (!g_ready) return;
    u32 *fb = (u32 *)(u64)g_framebuffer.physical_base;
    u32 pitch = g_framebuffer.pixels_per_scanline;
    for (u32 y = 0; y < g_framebuffer.height; ++y) {
        u32 *row = fb + (u64)y * pitch;
        u32 x = 0;
        if (((u64)row & 7ULL) == 0) {
            u64 *wide = (u64 *)(void *)row;
            u64 pair = (u64)color | ((u64)color << 32);
            for (; x + 1U < g_framebuffer.width; x += 2U) wide[x >> 1] = pair;
        }
        for (; x < g_framebuffer.width; ++x) row[x] = color;
    }
}

void framebuffer_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color) {
    if (!g_ready) return;
    u32 x_end = x + width;
    u32 y_end = y + height;
    if (x_end < x || x_end > g_framebuffer.width) x_end = g_framebuffer.width;
    if (y_end < y || y_end > g_framebuffer.height) y_end = g_framebuffer.height;
    u32 *fb = (u32 *)(u64)g_framebuffer.physical_base;
    u32 pitch = g_framebuffer.pixels_per_scanline;
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
    if (!g_ready || rows == 0) return;
    if (rows >= g_framebuffer.height) {
        framebuffer_fill(fill_color);
        return;
    }
    volatile u32 *fb = (volatile u32 *)(u64)g_framebuffer.physical_base;
    u32 pitch = g_framebuffer.pixels_per_scanline;
    u32 height = g_framebuffer.height;
    u32 width = g_framebuffer.width;
    for (u32 y = 0; y < height - rows; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fb[(u64)(y + rows) * pitch + x];
    for (u32 y = height - rows; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fill_color;
}

void framebuffer_blit_rgb332_row(u32 x, u32 y, u32 width,
    const u8 *source, u32 source_width) {
    if (!g_ready || !source || !source_width || !width ||
        x >= g_framebuffer.width || y >= g_framebuffer.height) return;
    if (width > g_framebuffer.width - x) width = g_framebuffer.width - x;
    volatile u32 *fb = (volatile u32 *)(u64)g_framebuffer.physical_base;
    volatile u32 *row = fb + (u64)y * g_framebuffer.pixels_per_scanline + x;
    for (u32 column = 0; column < width; ++column) {
        u32 source_x = (u32)(((u64)column * source_width) / width);
        u8 pixel = source[source_x];
        u8 r = (u8)(((pixel >> 5) & 7U) * 255U / 7U);
        u8 g = (u8)(((pixel >> 2) & 7U) * 255U / 7U);
        u8 b = (u8)((pixel & 3U) * 255U / 3U);
        row[column] = framebuffer_rgb(r, g, b);
    }
}

u32 framebuffer_width(void) { return g_ready ? g_framebuffer.width : 0; }
u32 framebuffer_height(void) { return g_ready ? g_framebuffer.height : 0; }