#ifndef JA_OS_BOOT_INFO_H
#define JA_OS_BOOT_INFO_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

#define BOOT_INFO_MAGIC 0x4A434F53424F4F54ULL  /* "JCOSBOOT" */
#define BOOT_INFO_VERSION 6U

typedef struct {
    u32 type;
    u32 padding;
    u64 physical_start;
    u64 virtual_start;
    u64 number_of_pages;
    u64 attribute;
} BootMemoryDescriptor;

typedef struct {
    /* Offset 0 is intentionally the stack top: kernel/entry.asm uses it. */
    u64 kernel_stack_top;
    u64 magic;
    u32 version;
    u32 size;

    u64 kernel_stack_base;
    u64 kernel_stack_size;

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
    u64 acpi_rsdp;

    u64 initrd_base;
    u64 initrd_size;
} BootInfo;

#endif
