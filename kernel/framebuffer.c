#include "framebuffer.h"

#include "display.h"
#include "interrupts.h"
#include "lib.h"
#include "vmm.h"

#define EFI_RESERVED_MEMORY_TYPE 0U
#define EFI_MEMORY_MAPPED_IO 11U

#define FRAMEBUFFER_MEMORY_TYPE_MIXED       0xFFFFFFFEU
#define FRAMEBUFFER_MEMORY_TYPE_UNDESCRIBED 0xFFFFFFFFU

static FramebufferInfo g_framebuffer; /* Immutable boot descriptor. */
static FramebufferInfo g_output;
static FramebufferOutputKind g_output_kind;
static u64 g_output_identity, g_next_output_identity = 1ULL;
static bool g_output_valid;
static bool g_ready; /* Immutable boot metadata remains ready after retirement. */
static FramebufferScanoutState g_scanout;
static u32 g_kernel_writers;

static u64 framebuffer_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void framebuffer_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}

void framebuffer_scanout_status(FramebufferScanoutStatus *out) {
    if (!out) return;
    u64 flags = framebuffer_irq_save();
    out->state = g_scanout;
    out->kernel_writers = g_kernel_writers;
    framebuffer_irq_restore(flags);
}

bool framebuffer_firmware_valid(void) {
    u64 flags = framebuffer_irq_save();
    bool valid = g_scanout == FRAMEBUFFER_SCANOUT_FIRMWARE;
    framebuffer_irq_restore(flags);
    return valid;
}

bool framebuffer_write_begin(void) {
    u64 flags = framebuffer_irq_save();
    bool allowed = g_output_valid &&
        display_kernel_framebuffer_writable() && g_kernel_writers != ~0U;
    if (allowed) ++g_kernel_writers;
    framebuffer_irq_restore(flags);
    return allowed;
}

void framebuffer_write_end(void) {
    u64 flags = framebuffer_irq_save();
    if (g_kernel_writers) --g_kernel_writers;
    if (!g_kernel_writers && g_scanout == FRAMEBUFFER_SCANOUT_RETIRING)
        g_scanout = FRAMEBUFFER_SCANOUT_RETIRED;
    framebuffer_irq_restore(flags);
}

bool framebuffer_writers_idle(void) {
    u64 flags = framebuffer_irq_save();
    bool idle = !g_kernel_writers;
    framebuffer_irq_restore(flags);
    return idle;
}

bool framebuffer_firmware_retire(void) {
    u64 flags = framebuffer_irq_save();
    if (g_scanout == FRAMEBUFFER_SCANOUT_FIRMWARE) {
        g_scanout = FRAMEBUFFER_SCANOUT_RETIRING;
        g_output_valid = false;
        g_output_kind = FRAMEBUFFER_OUTPUT_NONE;
    }
    if (g_scanout == FRAMEBUFFER_SCANOUT_RETIRING && !g_kernel_writers)
        g_scanout = FRAMEBUFFER_SCANOUT_RETIRED;
    bool drained = g_scanout == FRAMEBUFFER_SCANOUT_RETIRED;
    framebuffer_irq_restore(flags);
    return drained;
}

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
    /* Re-reading saved GOP data cannot revalidate disrupted hardware. */
    if (g_ready) return false;
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
    g_scanout = FRAMEBUFFER_SCANOUT_FIRMWARE;
    g_output = g_framebuffer;
    g_output_kind = FRAMEBUFFER_OUTPUT_FIRMWARE;
    g_output_valid = true;
    return true;
}

bool framebuffer_info(FramebufferInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!g_ready) return false;
    *out = g_framebuffer;
    return true;
}

void framebuffer_output_status(FramebufferOutputStatus *out) {
    if (!out) return;
    u64 flags = framebuffer_irq_save();
    out->kind = g_output_kind;
    out->identity = g_output_identity;
    out->kernel_writers = g_kernel_writers;
    out->valid = g_output_valid;
    framebuffer_irq_restore(flags);
}
bool framebuffer_output_valid(void) {
    u64 flags = framebuffer_irq_save();
    bool valid = g_output_valid;
    framebuffer_irq_restore(flags);
    return valid;
}
bool framebuffer_output_matches(u64 identity) {
    u64 flags = framebuffer_irq_save();
    bool matches = g_output_valid && g_output_identity == identity;
    framebuffer_irq_restore(flags);
    return matches;
}
bool framebuffer_output_info(FramebufferInfo *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    u64 flags = framebuffer_irq_save();
    bool valid = g_output_valid;
    if (valid) *out = g_output;
    framebuffer_irq_restore(flags);
    return valid;
}
static bool retained_output_valid(const FramebufferInfo *info) {
    /* The provider owns the hardware proof. This generic boundary additionally
     * prevents it from replacing the retained aperture with arbitrary memory.
     * New allocations/formats need a separate retained-storage contract. */
    return info && g_ready && g_framebuffer.direct_map_safe &&
        info->physical_base == g_framebuffer.physical_base && info->size == g_framebuffer.size &&
        info->mapping_physical_base == g_framebuffer.mapping_physical_base &&
        info->mapping_size == g_framebuffer.mapping_size && info->page_offset == g_framebuffer.page_offset &&
        info->memory_type == g_framebuffer.memory_type && info->direct_map_safe &&
        info->pixel_format == g_framebuffer.pixel_format && info->pixel_format <= 1U &&
        info->red_mask == g_framebuffer.red_mask && info->green_mask == g_framebuffer.green_mask &&
        info->blue_mask == g_framebuffer.blue_mask && info->reserved_mask == g_framebuffer.reserved_mask &&
        info->width == g_framebuffer.width && info->height == g_framebuffer.height &&
        info->pixels_per_scanline == g_framebuffer.pixels_per_scanline &&
        info->width && info->height && info->pixels_per_scanline >= info->width &&
        (u64)info->pixels_per_scanline <= info->size / 4ULL / info->height;
}
bool framebuffer_native_install(const FramebufferInfo *info, bool rescue, u64 *out_identity) {
    if (!out_identity) return false;
    *out_identity = 0;
    u64 flags = framebuffer_irq_save();
    bool installed = false;
    if (!g_next_output_identity || g_scanout != FRAMEBUFFER_SCANOUT_RETIRED ||
        g_output_valid || g_kernel_writers || display_direct_state() != DISPLAY_LEASE_KERNEL ||
        !retained_output_valid(info)) goto done;
    g_output = *info;
    g_output_identity = g_next_output_identity++;
    g_output_kind = rescue ? FRAMEBUFFER_OUTPUT_RESCUE : FRAMEBUFFER_OUTPUT_NATIVE;
    g_output_valid = true;
    *out_identity = g_output_identity;
    installed = true;
done:
    framebuffer_irq_restore(flags);
    return installed;
}
bool framebuffer_output_revoke(u64 expected_identity) {
    if (!expected_identity) return false;
    u64 flags = framebuffer_irq_save();
    bool revoked = g_output_identity == expected_identity &&
        (g_output_kind == FRAMEBUFFER_OUTPUT_NATIVE || g_output_kind == FRAMEBUFFER_OUTPUT_RESCUE ||
         g_output_kind == FRAMEBUFFER_OUTPUT_QUARANTINED);
    if (revoked) {
        g_output_valid = false;
        g_output_kind = FRAMEBUFFER_OUTPUT_QUARANTINED;
    }
    framebuffer_irq_restore(flags);
    return revoked;
}

u32 framebuffer_rgb(u8 r, u8 g, u8 b) {
    if (!g_ready) return 0;
    if (g_output.pixel_format == 0)
        return (u32)r | ((u32)g << 8) | ((u32)b << 16);
    if (g_output.pixel_format == 1)
        return (u32)b | ((u32)g << 8) | ((u32)r << 16);
    return channel(r, g_output.red_mask) |
           channel(g, g_output.green_mask) |
           channel(b, g_output.blue_mask);
}

void framebuffer_pixel(u32 x, u32 y, u32 color) {
    if (!framebuffer_write_begin()) return;
    if (x >= g_output.width || y >= g_output.height) { framebuffer_write_end(); return; }
    volatile u32 *fb = (volatile u32 *)(u64)g_output.physical_base;
    fb[(u64)y * g_output.pixels_per_scanline + x] = color;
    framebuffer_write_end();
}

void framebuffer_fill(u32 color) {
    if (!framebuffer_write_begin()) return;
    u32 *fb = (u32 *)(u64)g_output.physical_base;
    u32 pitch = g_output.pixels_per_scanline;
    for (u32 y = 0; y < g_output.height; ++y) {
        u32 *row = fb + (u64)y * pitch;
        u32 x = 0;
        if (((u64)row & 7ULL) == 0) {
            u64 *wide = (u64 *)(void *)row;
            u64 pair = (u64)color | ((u64)color << 32);
            for (; x + 1U < g_output.width; x += 2U) wide[x >> 1] = pair;
        }
        for (; x < g_output.width; ++x) row[x] = color;
    }
    framebuffer_write_end();
}

void framebuffer_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color) {
    if (!width || !height || !framebuffer_write_begin()) return;
    if (x >= g_output.width || y >= g_output.height) { framebuffer_write_end(); return; }
    u32 x_end = x + width;
    u32 y_end = y + height;
    if (x_end < x || x_end > g_output.width) x_end = g_output.width;
    if (y_end < y || y_end > g_output.height) y_end = g_output.height;
    u32 *fb = (u32 *)(u64)g_output.physical_base;
    u32 pitch = g_output.pixels_per_scanline;
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
    framebuffer_write_end();
}

void framebuffer_scroll_up(u32 rows, u32 fill_color) {
    if (!rows || !framebuffer_write_begin()) return;
    if (rows > g_output.height) rows = g_output.height;
    volatile u32 *fb = (volatile u32 *)(u64)g_output.physical_base;
    u32 pitch = g_output.pixels_per_scanline;
    u32 height = g_output.height;
    u32 width = g_output.width;
    for (u32 y = 0; y < height - rows; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fb[(u64)(y + rows) * pitch + x];
    for (u32 y = height - rows; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            fb[(u64)y * pitch + x] = fill_color;
    framebuffer_write_end();
}

void framebuffer_blit_rgb332_row(u32 x, u32 y, u32 width,
    const u8 *source, u32 source_width) {
    if (!source || !source_width || !width || !framebuffer_write_begin()) return;
    if (x >= g_output.width || y >= g_output.height) { framebuffer_write_end(); return; }
    if (width > g_output.width - x) width = g_output.width - x;
    volatile u32 *fb = (volatile u32 *)(u64)g_output.physical_base;
    volatile u32 *row = fb + (u64)y * g_output.pixels_per_scanline + x;
    for (u32 column = 0; column < width; ++column) {
        u32 source_x = (u32)(((u64)column * source_width) / width);
        u8 pixel = source[source_x];
        u8 r = (u8)(((pixel >> 5) & 7U) * 255U / 7U);
        u8 g = (u8)(((pixel >> 2) & 7U) * 255U / 7U);
        u8 b = (u8)((pixel & 3U) * 255U / 3U);
        row[column] = framebuffer_rgb(r, g, b);
    }
    framebuffer_write_end();
}

u32 framebuffer_width(void) { return g_ready ? g_output.width : 0; }
u32 framebuffer_height(void) { return g_ready ? g_output.height : 0; }