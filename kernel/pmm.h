#ifndef JA_OS_PMM_H
#define JA_OS_PMM_H

#include "types.h"

typedef struct {
    u64 total_pages;
    u64 free_pages;
    u32 range_count;
    u32 discarded_ranges;
} PmmStats;

bool pmm_init(const BootInfo *boot);
u64 pmm_alloc_page(void);
u64 pmm_alloc_pages(u64 count);
PmmStats pmm_stats(void);

#endif
