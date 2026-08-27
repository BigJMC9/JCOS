#ifndef JA_OS_FRAMEBUFFER_H
#define JA_OS_FRAMEBUFFER_H

#include "types.h"

bool framebuffer_init(const BootInfo *boot);
u32 framebuffer_rgb(u8 r, u8 g, u8 b);
void framebuffer_pixel(u32 x, u32 y, u32 color);
void framebuffer_fill(u32 color);
void framebuffer_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color);
void framebuffer_scroll_up(u32 rows, u32 fill_color);
u32 framebuffer_width(void);
u32 framebuffer_height(void);

#endif
