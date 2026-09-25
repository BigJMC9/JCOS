#ifndef JA_OS_FRAMEBUFFER_H
#define JA_OS_FRAMEBUFFER_H

#include "types.h"

typedef struct {
    /* Validated GOP byte range supplied by firmware. */
    u64 physical_base;
    u64 size;

    /* Minimal page-rounded span needed to map the byte range. */
    u64 mapping_physical_base;
    u64 mapping_size;
    u64 page_offset;

    /* Direct Ring3 mapping is permitted only when the complete page-rounded
     * span is firmware-described MMIO rather than allocatable RAM. */
    u32 memory_type;
    bool direct_map_safe;

    u32 width;
    u32 height;
    u32 pixels_per_scanline;
    u32 pixel_format;
    u32 red_mask;
    u32 green_mask;
    u32 blue_mask;
    u32 reserved_mask;
} FramebufferInfo;

bool framebuffer_init(const BootInfo *boot);
bool framebuffer_info(FramebufferInfo *out);
u32 framebuffer_rgb(u8 r, u8 g, u8 b);
void framebuffer_pixel(u32 x, u32 y, u32 color);
void framebuffer_fill(u32 color);
void framebuffer_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color);
void framebuffer_scroll_up(u32 rows, u32 fill_color);
void framebuffer_blit_rgb332_row(u32 x, u32 y, u32 width,
    const u8 *source, u32 source_width);
u32 framebuffer_width(void);
u32 framebuffer_height(void);

#endif