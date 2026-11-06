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

/* Active output is independent of immutable boot metadata and the CPU lease.
 * QUARANTINED retains its generation/descriptor for diagnosis, never admission. */
typedef enum {
    FRAMEBUFFER_OUTPUT_NONE = 0,
    FRAMEBUFFER_OUTPUT_FIRMWARE,
    FRAMEBUFFER_OUTPUT_NATIVE,
    FRAMEBUFFER_OUTPUT_RESCUE,
    FRAMEBUFFER_OUTPUT_QUARANTINED
} FramebufferOutputKind;
typedef struct {
    FramebufferOutputKind kind;
    u64 identity; /* zero only for firmware/unavailable; monotonic native IDs */
    u32 kernel_writers;
    bool valid;
} FramebufferOutputStatus;

void framebuffer_output_status(FramebufferOutputStatus *out);
bool framebuffer_output_valid(void);
bool framebuffer_output_info(FramebufferInfo *out);
bool framebuffer_output_matches(u64 expected_identity);
/* Privileged post-validation publication, not a GPU operation or userspace ABI.
 * Caller must already have independently established operational scanout.
 * This first storage profile is confined to the retained boot allocation:
 * same physical range/cache/format/geometry, no new VRAM authority.
 * Requires permanently retired firmware, drained writers and KERNEL lease.
 * No normal/diagnostic boot path calls this constructor. */
bool framebuffer_native_install(const FramebufferInfo *info, bool rescue, u64 *out_identity);
/* Immediately closes kernel admission; retains the descriptor/generation.
 * Does not revoke a live Ring3 mapping or prove hardware/DMA quiescence.
 * Stop/reap its owner before publishing replacement under the KERNEL lease. */
bool framebuffer_output_revoke(u64 expected_identity);

/* This is boot-scanout validity, independent of the CPU writer lease and of
 * device/DMA isolation. RETIRED does not mean that native output is active. */
typedef enum {
    FRAMEBUFFER_SCANOUT_UNAVAILABLE = 0,
    FRAMEBUFFER_SCANOUT_FIRMWARE,
    FRAMEBUFFER_SCANOUT_RETIRING,
    FRAMEBUFFER_SCANOUT_RETIRED
} FramebufferScanoutState;

typedef struct {
    FramebufferScanoutState state;
    u32 kernel_writers;
} FramebufferScanoutStatus;

bool framebuffer_init(const BootInfo *boot);
void framebuffer_scanout_status(FramebufferScanoutStatus *out);
bool framebuffer_firmware_valid(void);
/* Short, UP interrupt-serialized admission; never hold interrupts off while
 * drawing. An outer writer pins an entire multi-primitive presentation. */
bool framebuffer_write_begin(void);
void framebuffer_write_end(void);
bool framebuffer_writers_idle(void);
/* Display manager only, after borrowed Ring3 mappings have been torn down.
 * Closes admission permanently, including on a false (still draining) result.
 * True proves CPU writers drained, never DMA isolation or native readiness. */
bool framebuffer_firmware_retire(void);
/* Immutable boot descriptor, still available after retirement/replacement. */
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