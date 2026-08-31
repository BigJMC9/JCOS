#ifndef JA_OS_PMM_H
#define JA_OS_PMM_H

#include "types.h"

#define FRAME_SIZE 4096ULL

typedef u64 frame_t;

#define FRAME_INVALID ((frame_t)~0ULL)

typedef struct {
    u64 total_pages;
    u64 free_pages;

    u64 bitmap_physical;
    u64 bitmap_pages;

    u32 range_count;
    u32 discarded_ranges;
} PmmStats;

/*
 * Initialize the physical frame allocator from
 * the UEFI memory map.
 */
bool pmm_init(
    const BootInfo *boot
);

/* Move PMM metadata access away from the temporary low identity mapping. */
bool pmm_enable_phys_map_access(void);

bool pmm_phys_map_access_enabled(void);

/* frame_t is a physical frame number, not an address. */
frame_t frame_alloc(void);

bool frame_free(
    frame_t frame
);

u64 frame_to_phys(
    frame_t frame
);

frame_t phys_to_frame(
    u64 physical
);

/*
 * Compatibility API.
 *
 * Existing drivers currently expect physical
 * addresses rather than frame numbers.
 */
u64 pmm_alloc_page(void);

u64 pmm_alloc_pages(
    u64 count
);

PmmStats pmm_stats(void);

#endif