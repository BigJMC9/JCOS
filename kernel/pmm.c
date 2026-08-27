#include "pmm.h"

#define PAGE_SIZE 4096ULL
#define EFI_CONVENTIONAL_MEMORY 7U
#define MAX_RANGES 128U
#define MIN_USABLE_ADDRESS 0x100000ULL

typedef struct {
    u64 next;
    u64 end;
} PmmRange;

static PmmRange g_ranges[MAX_RANGES];
static PmmStats g_stats;

static u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static u64 align_down(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

bool pmm_init(const BootInfo *boot) {
    g_stats.total_pages = 0;
    g_stats.free_pages = 0;
    g_stats.range_count = 0;
    g_stats.discarded_ranges = 0;

    if (!boot || !boot->memory_map ||
        boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor)) return false;

    for (u64 offset = 0; offset + sizeof(BootMemoryDescriptor) <= boot->memory_map_size;
         offset += boot->memory_map_descriptor_size) {
        const BootMemoryDescriptor *d =
            (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);
        if (d->type != EFI_CONVENTIONAL_MEMORY || d->number_of_pages == 0) continue;
        if (d->number_of_pages > (~0ULL / PAGE_SIZE)) continue;

        u64 bytes = d->number_of_pages * PAGE_SIZE;
        u64 raw_end = d->physical_start + bytes;
        if (raw_end < d->physical_start) continue;
        u64 start = align_up(d->physical_start, PAGE_SIZE);
        u64 end = align_down(raw_end, PAGE_SIZE);
        if (start < MIN_USABLE_ADDRESS) start = MIN_USABLE_ADDRESS;
        if (end <= start) continue;

        /* Merge adjacent conventional-memory descriptors where possible. */
        if (g_stats.range_count && g_ranges[g_stats.range_count - 1].end == start) {
            g_ranges[g_stats.range_count - 1].end = end;
        } else if (g_stats.range_count < MAX_RANGES) {
            g_ranges[g_stats.range_count].next = start;
            g_ranges[g_stats.range_count].end = end;
            ++g_stats.range_count;
        } else {
            ++g_stats.discarded_ranges;
            continue;
        }
    }

    for (u32 i = 0; i < g_stats.range_count; ++i) {
        u64 pages = (g_ranges[i].end - g_ranges[i].next) / PAGE_SIZE;
        g_stats.total_pages += pages;
    }
    g_stats.free_pages = g_stats.total_pages;
    return g_stats.range_count != 0;
}

u64 pmm_alloc_pages(u64 count) {
    if (!count || count > (~0ULL / PAGE_SIZE)) return 0;
    u64 bytes = count * PAGE_SIZE;
    for (u32 i = 0; i < g_stats.range_count; ++i) {
        PmmRange *range = &g_ranges[i];
        if (range->end - range->next < bytes) continue;
        u64 address = range->next;
        range->next += bytes;
        g_stats.free_pages -= count;
        return address;
    }
    return 0;
}

u64 pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

PmmStats pmm_stats(void) {
    return g_stats;
}
