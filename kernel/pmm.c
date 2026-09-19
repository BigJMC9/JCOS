#include "pmm.h"
#include "lib.h"
#include "physmap.h"
#include "interrupts.h"
#include "pmm_test.h"

#define EFI_CONVENTIONAL_MEMORY 7U

#define PMM_MAX_RANGES 128U

#define MIN_USABLE_ADDRESS 0x100000ULL

typedef struct {
    /*
     * Physical frame numbers.
     *
     * end is exclusive.
     */
    frame_t first; frame_t end;
} PmmRange;

static PmmRange g_ranges[PMM_MAX_RANGES];
static PmmStats g_stats;

/*
 * Bitmap:
 *
 *     0 = free
 *     1 = allocated / reserved
 *
 * The pointer is assigned at runtime so we keep
 * the relocation-free PIE rules intact.
 */
static u8 *g_bitmap;
static frame_t g_base_frame;
static u64 g_bitmap_bits;
static frame_t g_bitmap_first_frame;
static u64 g_bitmap_page_count;
static u64 g_next_hint;
static bool g_initialized;
static bool g_use_phys_map;

/* UP-only serialization; PMM is not callable from NMI/emergency context. */
static u64 pmm_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void pmm_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}

/* Exact-range, one-shot test fault. Never armed by a userspace ABI. */
static bool g_free_fault_armed;
static frame_t g_free_fault_first;
static u64 g_free_fault_count;

/* Alignment helpers. */
static bool align_up_page(u64 value, u64 *result) {
    if (!result) return false;
    if (value > ~0ULL - (FRAME_SIZE - 1ULL)) return false;

    *result = (value + FRAME_SIZE - 1ULL) & ~(FRAME_SIZE - 1ULL);

    return true;
}

static u64 align_down_page(u64 value) {
    return
        value &
        ~(FRAME_SIZE - 1ULL);
}

/* Return true if this physical frame belongs to one of the conventional-memory regions that PMM owns. */
static bool frame_managed(frame_t frame) {
    for (u32 i = 0; i < g_stats.range_count; ++i) {
        if (frame >= g_ranges[i].first && frame < g_ranges[i].end) return true;
    }

    return false;
}

/* PMM metadata itself occupies physical frames. Those frames can never be returned through frame_free(). */
static bool frame_is_bitmap(frame_t frame) {
    if (g_bitmap_first_frame == FRAME_INVALID) return false;
    if (frame < g_bitmap_first_frame) return false;

    return
        frame - g_bitmap_first_frame <
        g_bitmap_page_count;
}

static bool bitmap_used(u64 index) {
    u64 byte = index >> 3;
    u32 bit = (u32)(index & 7ULL);

    return (g_bitmap[byte] & (u8)(1U << bit)) != 0;
}

static void bitmap_set(u64 index) {
    u64 byte = index >> 3;
    u32 bit = (u32)(index & 7ULL);

    g_bitmap[byte] |= (u8)(1U << bit);
}

static void bitmap_clear(u64 index) {
    u64 byte = index >> 3;
    u32 bit = (u32)(index & 7ULL);
    u8 mask = (u8)(1U << bit);

    g_bitmap[byte] &= (u8)~mask;
}

static u64 frame_bitmap_index(frame_t frame) {
    return frame - g_base_frame;
}

u64 frame_to_phys(frame_t frame) {
    if (frame == FRAME_INVALID) return 0;
    if (frame > ~0ULL / FRAME_SIZE) return 0;

    return frame * FRAME_SIZE;
}

frame_t phys_to_frame(u64 physical) {
    if (physical & (FRAME_SIZE - 1ULL)) return FRAME_INVALID;

    return physical / FRAME_SIZE;
}

bool pmm_init(const BootInfo *boot) {
    g_initialized = false;
    g_use_phys_map = false;
    g_free_fault_armed = false;

    g_bitmap = 0;
    g_base_frame = FRAME_INVALID;
    g_bitmap_first_frame = FRAME_INVALID;
    g_bitmap_page_count = 0;
    g_bitmap_bits = 0;
    g_next_hint = 0;

    k_memset(&g_stats, 0, sizeof(g_stats));
    k_memset(g_ranges, 0, sizeof(g_ranges));

    if (!boot || !boot->memory_map || !boot->memory_map_size || boot->memory_map_descriptor_size < sizeof(BootMemoryDescriptor)) return false;

    /*
     * First pass:
     *
     * Collect every conventional-memory range
     * that PMM will own.
     */
    for (u64 offset = 0; offset + sizeof(BootMemoryDescriptor) <= boot->memory_map_size; offset += boot->memory_map_descriptor_size) {

        const BootMemoryDescriptor *descriptor = (const BootMemoryDescriptor *)(u64)(boot->memory_map + offset);

        if (descriptor->type != EFI_CONVENTIONAL_MEMORY) continue;
        if (!descriptor->number_of_pages) continue;
        if (descriptor->number_of_pages > ~0ULL / FRAME_SIZE) continue;

        u64 bytes = descriptor->number_of_pages * FRAME_SIZE;

        if (descriptor->physical_start > ~0ULL - bytes) continue;

        u64 raw_end = descriptor->physical_start + bytes;
        u64 start = 0;

        if (!align_up_page(descriptor->physical_start, &start)) continue;

        u64 end = align_down_page(raw_end);

        /* Keep the first MiB out of the allocator. */
        if (start < MIN_USABLE_ADDRESS) start = MIN_USABLE_ADDRESS;
        if (end <= start) continue;

        frame_t first = phys_to_frame(start);
        frame_t last = phys_to_frame(end);

        if (first == FRAME_INVALID || last == FRAME_INVALID || last <= first) continue;

        /* Merge adjacent ranges where possible. */
        if (g_stats.range_count && g_ranges[g_stats.range_count - 1U].end == first) {
            g_ranges[g_stats.range_count - 1U].end = last;
            continue;
        }

        if (g_stats.range_count >= PMM_MAX_RANGES) {
            ++g_stats.discarded_ranges;
            continue;
        }

        PmmRange *range = &g_ranges[g_stats.range_count++];

        range->first = first;
        range->end = last;
    }

    if (!g_stats.range_count) return false;

    /* Determine the physical frame span covered by the bitmap. */
    frame_t lowest = g_ranges[0].first;
    frame_t highest = g_ranges[0].end;

    for (u32 i = 0; i < g_stats.range_count; ++i) {

        PmmRange *range = &g_ranges[i];

        if (range->first < lowest) lowest = range->first;
        if (range->end > highest) highest = range->end;

        g_stats.total_pages += range->end - range->first;
    }

    if (highest <= lowest) return false;

    g_base_frame = lowest;
    g_bitmap_bits = highest - lowest;

    if (g_bitmap_bits > ~0ULL - 7ULL) return false;

    u64 bitmap_bytes = (g_bitmap_bits + 7ULL) / 8ULL;

    if (!bitmap_bytes) return false;
    if (bitmap_bytes > ~0ULL - (FRAME_SIZE - 1ULL)) return false;

    g_bitmap_page_count = (bitmap_bytes + FRAME_SIZE - 1ULL) / FRAME_SIZE;

    if (!g_bitmap_page_count) return false;

    /*
     * Find one conventional-memory range large
     * enough to hold the bitmap itself.
     *
     * Since the allocator does not exist yet,
     * this is the bootstrap allocation.
     */
    for (u32 i = 0; i < g_stats.range_count; ++i) {
        u64 pages = g_ranges[i].end - g_ranges[i].first;

        if (pages >= g_bitmap_page_count) {
            g_bitmap_first_frame = g_ranges[i].first;
            break;
        }
    }

    if (g_bitmap_first_frame == FRAME_INVALID) return false;

    u64 bitmap_physical = frame_to_phys(g_bitmap_first_frame);

    if (!bitmap_physical) return false;
    if (g_bitmap_page_count > ~0ULL / FRAME_SIZE) return false;

    u64 bitmap_storage_bytes = g_bitmap_page_count * FRAME_SIZE;

    /*
     * Current boot mappings allow physical RAM
     * to be directly accessed.
     *
     * Once VMM owns CR3, this bitmap will be
     * accessed through the physical direct map.
     */
    g_bitmap = (u8 *)(u64) bitmap_physical;

    /*
     * Start pessimistically:
     *
     * everything is reserved.
     */
    k_memset(g_bitmap, 0xFFU, (usize)bitmap_storage_bytes);

    /*
     * Now mark only managed conventional-memory
     * frames as free.
     *
     * Holes and non-conventional memory therefore
     * remain permanently reserved.
     */
    for (u32 i = 0; i < g_stats.range_count; ++i) {
        for (frame_t frame = g_ranges[i].first; frame < g_ranges[i].end; ++frame) {
            u64 index = frame_bitmap_index(frame);
            bitmap_clear(index);
        }
    }

    /* Reserve PMM own bitmap storage again. */
    for (u64 i = 0; i < g_bitmap_page_count; ++i) {
        frame_t frame = g_bitmap_first_frame + i;
        bitmap_set(frame_bitmap_index(frame));
    }

    if (g_bitmap_page_count > g_stats.total_pages) return false;

    g_stats.free_pages = g_stats.total_pages - g_bitmap_page_count;
    g_stats.bitmap_physical = bitmap_physical;
    g_stats.bitmap_pages = g_bitmap_page_count;
    g_next_hint = 0;
    g_initialized = true;

    return true;
}

bool pmm_enable_phys_map_access(void) {
    if (!g_initialized ||
        !g_bitmap ||
        !g_stats.bitmap_physical ||
        !g_stats.bitmap_pages) {

        return false;
    }

    void *direct =
        phys_to_virt(
            g_stats.bitmap_physical
        );

    if (!direct)
        return false;

    /*
     * From this point onward frame_alloc() and
     * frame_free() access the allocator bitmap
     * through the high-half physical direct map.
     */
    g_bitmap =
        (u8 *)direct;

    g_use_phys_map =
        true;

    return true;
}


bool pmm_phys_map_access_enabled(void) {
    return
        g_use_phys_map;
}

static frame_t frame_alloc_locked(void) {
    if (!g_initialized || !g_bitmap || !g_bitmap_bits || !g_stats.free_pages) return FRAME_INVALID;

    u64 index = g_next_hint;

    /* Search the bitmap once, wrapping at the end. */
    for (u64 scanned = 0; scanned < g_bitmap_bits; ++scanned) {
        if (!bitmap_used(index)) {
            bitmap_set(index);
            --g_stats.free_pages;
            frame_t frame = g_base_frame + index;
            ++index;

            if (index >= g_bitmap_bits) index = 0;
            g_next_hint = index;
            return frame;
        }

        ++index;
        if (index >= g_bitmap_bits) index = 0;
    }
    return FRAME_INVALID;
}

frame_t frame_alloc(void) {
    u64 flags = pmm_irq_save();
    frame_t frame = frame_alloc_locked();
    pmm_irq_restore(flags);
    return frame;
}

/* Validate the whole span before any bitmap, count, or hint mutation. */
static bool free_range_valid(frame_t first, u64 count) {
    if (!g_initialized || !g_bitmap || !count || first == FRAME_INVALID) return false;
    if (first < g_base_frame || first > ~0ULL / FRAME_SIZE) return false;
    if (count - 1ULL > (~0ULL / FRAME_SIZE) - first) return false;

    u64 index = frame_bitmap_index(first);
    if (index >= g_bitmap_bits || count > g_bitmap_bits - index) return false;
    if (g_stats.free_pages > g_stats.total_pages) return false;
    if (count > g_stats.total_pages - g_stats.free_pages) return false;

    for (u64 i = 0; i < count; ++i) {
        frame_t frame = first + i;
        if (!frame_managed(frame) || frame_is_bitmap(frame)) return false;
        if (!bitmap_used(index + i)) return false;
    }
    return true;
}

bool frame_free_range(frame_t first, u64 count) {
    u64 flags = pmm_irq_save();
    if (!free_range_valid(first, count)) {
        pmm_irq_restore(flags);
        return false;
    }

    if (g_free_fault_armed && first == g_free_fault_first && count == g_free_fault_count) {
        g_free_fault_armed = false;
        pmm_irq_restore(flags);
        return false;
    }

    /* Commit: no allocation, callback, yielding, or fallible operation. */
    u64 index = frame_bitmap_index(first);
    for (u64 i = 0; i < count; ++i) bitmap_clear(index + i);
    g_stats.free_pages += count;
    g_next_hint = index;
    pmm_irq_restore(flags);
    return true;
}

bool frame_free(frame_t frame) {
    return frame_free_range(frame, 1ULL);
}

bool pmm_test_fail_free_range_once(frame_t first, u64 count) {
    u64 flags = pmm_irq_save();
    bool valid = !g_free_fault_armed && free_range_valid(first, count);
    if (valid) {
        g_free_fault_first = first;
        g_free_fault_count = count;
        g_free_fault_armed = true;
    }
    pmm_irq_restore(flags);
    return valid;
}

bool pmm_test_free_failure_armed(void) {
    u64 flags = pmm_irq_save();
    bool armed = g_free_fault_armed;
    pmm_irq_restore(flags);
    return armed;
}

void pmm_test_clear_free_failure(void) {
    u64 flags = pmm_irq_save();
    g_free_fault_armed = false;
    pmm_irq_restore(flags);
}

bool pmm_test_frame_releasable(frame_t frame) {
    u64 flags = pmm_irq_save();
    bool valid = free_range_valid(frame, 1ULL);
    pmm_irq_restore(flags);
    return valid;
}

/* Compatibility API: allocate a contiguous physical run. */
static u64 pmm_alloc_pages_locked(u64 count) {
    if (!g_initialized || !count || count > g_stats.free_pages || count > g_bitmap_bits) return 0;

    u64 run_start = 0;
    u64 run_length = 0;

    for (u64 index = 0; index < g_bitmap_bits; ++index) {
        if (bitmap_used(index)) {
            run_length = 0;
            continue;
        }

        if (!run_length) run_start = index;

        ++run_length;

        if (run_length < count) continue;
        for (u64 i = 0; i < count; ++i) bitmap_set(run_start + i);

        g_stats.free_pages -= count;
        g_next_hint = run_start + count;
        if (g_next_hint >= g_bitmap_bits) g_next_hint = 0;

        frame_t first = g_base_frame + run_start;

        return frame_to_phys(first);
    }

    return 0;
}

u64 pmm_alloc_pages(u64 count) {
    u64 flags = pmm_irq_save();
    u64 physical = pmm_alloc_pages_locked(count);
    pmm_irq_restore(flags);
    return physical;
}

u64 pmm_alloc_page(void) {
    frame_t frame = frame_alloc();

    if (frame == FRAME_INVALID) return 0;
    return frame_to_phys(frame);
}

PmmStats pmm_stats(void) {
    u64 flags = pmm_irq_save();
    PmmStats stats = g_stats;
    pmm_irq_restore(flags);
    return stats;
}