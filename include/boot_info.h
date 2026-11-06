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
#define BOOT_INFO_VERSION 10U
#define BOOT_DISPLAY_BAR_COUNT 6U
#define BOOT_DISPLAY_EDID_BYTES 128U
#define BOOT_DISPLAY_BAR_VALID    (1U << 0)
#define BOOT_DISPLAY_BAR_MEMORY   (1U << 1)
#define BOOT_DISPLAY_BAR_64BIT    (1U << 2)
#define BOOT_DISPLAY_BAR_PREFETCH (1U << 3)

typedef struct {
    u64 base;
    u64 size;
    u32 flags;
    u32 reserved;
} BootDisplayBar;

/* Firmware BAR query outcomes. Failure evidence never authorizes access. */
#define BOOT_DISPLAY_BAR_NOT_QUERIED       0U
#define BOOT_DISPLAY_BAR_UNASSIGNED        1U
#define BOOT_DISPLAY_BAR_CONTINUATION      2U
#define BOOT_DISPLAY_BAR_NO_PROTOCOL       3U
#define BOOT_DISPLAY_BAR_QUERY_FAILED      4U
#define BOOT_DISPLAY_BAR_EMPTY_RESPONSE    5U
#define BOOT_DISPLAY_BAR_DESCRIPTOR        6U
#define BOOT_DISPLAY_BAR_MALFORMED         7U
#define BOOT_DISPLAY_BAR_CHECKSUM          8U
#define BOOT_DISPLAY_BAR_TRANSLATED        9U
#define BOOT_DISPLAY_BAR_TYPE_MISMATCH    10U
#define BOOT_DISPLAY_BAR_BASE_MISMATCH    11U
#define BOOT_DISPLAY_BAR_EXTENT           12U
#define BOOT_DISPLAY_BAR_NONCONTIGUOUS    13U
#define BOOT_DISPLAY_BAR_ACCEPTED         14U
#define BOOT_DISPLAY_BAR_CONFIG_CHANGED   15U
#define BOOT_DISPLAY_BAR_LIST_LIMIT       16U
#define BOOT_DISPLAY_BAR_EMPTY_LIST       17U
#define BOOT_DISPLAY_BAR_NO_MATCH         18U
#define BOOT_DISPLAY_BAR_AMBIGUOUS        19U
#define BOOT_DISPLAY_QUERY_NONE           0xFFFFFFFFU
#define BOOT_DISPLAY_QUERY_PREFIX_BYTES   48U
#define BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT  8U
#define BOOT_DISPLAY_BAR_RESOURCE_BYTES   (46U * BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT + 2U)

typedef struct {
    u64 query_status;
    u64 reported_base;
    u64 reported_size;
    u64 translation;
    u64 granularity;
    u64 reported_maximum;
    u32 result;
    u16 descriptor_count;
    u16 descriptor_length;
    u8 descriptor_tag;
    u8 resource_type;
    u8 general_flags;
    u8 specific_flags;
    u32 reserved;
} BootDisplayBarAudit;

/* Array position is the exact firmware query index, distinct from PCI slots.
 * Match bits refer only to real lower BAR slots on the selected controller.
 * Raw prefix bytes are copied data, never a firmware pointer or access grant. */
typedef struct {
    BootDisplayBarAudit audit;
    u32 matching_bars;
    u32 prefix_size;
    u8 prefix[BOOT_DISPLAY_QUERY_PREFIX_BYTES];
} BootDisplayBarQuery;

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

    /* Optional PCI provenance for the selected GOP, copied before firmware
     * exit. Missing identity must not be inferred from framebuffer proximity. */
    u32 framebuffer_mode;
    u32 framebuffer_pci_valid;
    u32 framebuffer_pci_segment;
    u8 framebuffer_pci_bus;
    u8 framebuffer_pci_device;
    u8 framebuffer_pci_function;
    u8 reserved1;

    /* Firmware-reported inventory, never mapping or programming authority. */
    u32 framebuffer_pci_vendor_device;
    u32 framebuffer_pci_subsystem;
    u32 framebuffer_pci_class_revision;
    u32 framebuffer_metadata_reserved;
    BootDisplayBar framebuffer_bars[BOOT_DISPLAY_BAR_COUNT];
    u32 framebuffer_edid_size;
    u32 framebuffer_edid_reserved;
    u8 framebuffer_edid[BOOT_DISPLAY_EDID_BYTES];
    BootDisplayBarAudit framebuffer_bar_audit[BOOT_DISPLAY_BAR_COUNT];
    BootDisplayBarQuery framebuffer_bar_queries[BOOT_DISPLAY_BAR_COUNT];
    u32 framebuffer_bar_query_index[BOOT_DISPLAY_BAR_COUNT];
} BootInfo;

_Static_assert(__builtin_offsetof(BootInfo, kernel_stack_top) == 0, "entry stack offset");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_mode) == 168, "GOP metadata offset");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_bars) == 200, "GOP BAR metadata offset");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_edid) == 352, "GOP EDID metadata offset");
_Static_assert(sizeof(BootDisplayBar) == 24, "boot display BAR layout");
_Static_assert(sizeof(BootDisplayBarAudit) == 64, "boot display BAR audit layout");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_bar_audit) == 480, "GOP BAR audit offset");
_Static_assert(sizeof(BootDisplayBarQuery) == 120, "boot display query layout");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_bar_queries) == 864, "boot query offset");
_Static_assert(__builtin_offsetof(BootInfo, framebuffer_bar_query_index) == 1584, "boot query binding offset");
_Static_assert(sizeof(BootInfo) == 1608, "boot handoff v10 layout");

#endif
