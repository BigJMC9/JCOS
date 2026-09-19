#ifndef JA_OS_OBJECT_STORAGE_H
#define JA_OS_OBJECT_STORAGE_H

#include "types.h"

/* Initial UP profile: fixed live-object budgets, including bootstrap objects. */
#define PROCESS_STORAGE_CAPACITY 128U
#define THREAD_STORAGE_CAPACITY 512U
#define ADDRESS_SPACE_STORAGE_CAPACITY 256U

/* Compare storage addresses without reading an uninitialized caller object.
 * Entries do not allocate storage or retain resources after destruction.
 * Callers serialize every registry operation with local IRQ exclusion. */
static inline u32 object_storage_find(void *const *slots, u32 count, const void *object) {
    if (object) for (u32 i = 0; i < count; ++i) if (slots[i] == object) return i;
    return count;
}

static inline u32 object_storage_empty(void *const *slots, u32 count) {
    for (u32 i = 0; i < count; ++i) if (!slots[i]) return i;
    return count;
}

static inline u32 object_storage_count(void *const *slots, u32 count) {
    u32 used = 0;
    for (u32 i = 0; i < count; ++i) if (slots[i]) ++used;
    return used;
}

#endif
