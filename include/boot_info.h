#ifndef JC_OS_BOOT_INFO_H
#define JC_OS_BOOT_INFO_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define BOOT_INFO_MAGIC 0x48454C4C4F4F5334ULL /* "HELLOOS4" */

typedef struct {
    /* Offset 0 is intentionally the stack top: kernel/entry.asm uses it. */
    u64 kernel_stack_top;
    u64 magic;

    u64 framebuffer_base;
    u64 framebuffer_size;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u32 framebuffer_pixels_per_scanline;
    u32 framebuffer_pixel_format;
    u32 framebuffer_red_mask;
    u32 framebuffer_green_mask;
    u32 framebuffer_blue_mask;
    u32 framebuffer_reserved_mask;

    u64 memory_map;
    u64 memory_map_size;
    u64 memory_map_descriptor_size;
    u32 memory_map_descriptor_version;
    u32 reserved0;

    u64 kernel_base;
    u64 kernel_size;
    u64 kernel_entry;
} BootInfo;

#endif
