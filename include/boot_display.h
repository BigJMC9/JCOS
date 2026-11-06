#ifndef JA_OS_BOOT_DISPLAY_H
#define JA_OS_BOOT_DISPLAY_H

#include "boot_info.h"

/* Shared byte decoders for bounded, copied firmware inventory. These helpers
 * do not create authority or touch hardware. The firmware protocol allocation
 * itself remains part of the trusted boot environment. */
static inline u64 boot_display_read_le(const u8 *bytes, u32 width) {
    u64 value = 0;
    for (u32 i = 0; i < width; ++i) value |= (u64)bytes[i] << (i * 8U);
    return value;
}

static inline void boot_display_zero(void *target, u32 size) {
    volatile u8 *bytes = (volatile u8 *)target;
    for (u32 i = 0; i < size; ++i) bytes[i] = 0;
}

static inline void boot_display_copy(void *target, const void *source, u32 size) {
    volatile u8 *out = (volatile u8 *)target;
    const u8 *in = (const u8 *)source;
    for (u32 i = 0; i < size; ++i) out[i] = in[i];
}

static inline int boot_display_bar_fail(BootDisplayBarAudit *audit, u32 result) {
    if (audit) audit->result = result;
    return 0;
}

/* One to eight QWord descriptors followed by End Tag. Decode only identity
 * translation and a contiguous extent; both address spaces remain explicit.
 * Granularity follows UEFI PCI I/O, not generic ACPI mask semantics. */
static inline int boot_display_decode_bar_audited(const u8 *bytes, u32 size,
    u32 low, u32 high, u32 index, BootDisplayBar *out, BootDisplayBarAudit *audit) {
    if (audit) boot_display_zero(audit, sizeof(*audit));
    if (!out) return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
    boot_display_zero(out, sizeof(*out));
    if (!bytes || size < 2U || index >= BOOT_DISPLAY_BAR_COUNT || !low || low == 0xFFFFFFFFU)
        return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
    u32 flags = BOOT_DISPLAY_BAR_VALID;
    u64 expected = 0;
    int memory = !(low & 1U);
    int wide = memory && ((low >> 1U) & 3U) == 2U;
    if (!memory) {
        expected = low & ~3U;
    } else {
        flags |= BOOT_DISPLAY_BAR_MEMORY;
        u32 type = (low >> 1U) & 3U;
        if (type == 0U) {
            expected = low & ~15U;
        } else if (wide && index + 1U < BOOT_DISPLAY_BAR_COUNT) {
            expected = ((u64)high << 32U) | (u64)(low & ~15U);
            flags |= BOOT_DISPLAY_BAR_64BIT;
        } else {
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_TYPE_MISMATCH);
        }
        if (low & 8U) flags |= BOOT_DISPLAY_BAR_PREFETCH;
    }
    u32 offset = 0;
    u32 count = 0;
    u64 total = 0;
    for (;;) {
        if (size - offset < 2U)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
        if (bytes[offset] == 0x79U) {
            if (!count && offset + 2U == size)
                return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_EMPTY_LIST);
            if (offset + 2U != size)
                return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
            if (bytes[offset + 1U]) {
                u8 sum = 0;
                for (u32 i = 0; i < size; ++i) sum = (u8)(sum + bytes[i]);
                if (sum) return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_CHECKSUM);
            }
            break;
        }
        if (count == BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_LIST_LIMIT);
        if (size - offset < 3U)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
        const u8 *descriptor = bytes + offset;
        u16 length = (u16)boot_display_read_le(descriptor + 1U, 2U);
        if (audit) {
            audit->descriptor_tag = descriptor[0];
            audit->descriptor_length = length;
        }
        if (descriptor[0] != 0x8AU || length != 43U)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_DESCRIPTOR);
        if (size - offset < 46U)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_MALFORMED);
        u64 base = boot_display_read_le(descriptor + 14U, 8U);
        u64 span = boot_display_read_le(descriptor + 38U, 8U);
        u64 translation = boot_display_read_le(descriptor + 30U, 8U);
        u64 granularity = boot_display_read_le(descriptor + 6U, 8U);
        ++count;
        if (audit) {
            audit->descriptor_count = (u16)count;
            audit->reported_base = base;
            audit->reported_size = span;
            audit->translation = translation;
            audit->granularity = granularity;
            audit->reported_maximum = boot_display_read_le(descriptor + 22U, 8U);
            audit->resource_type = descriptor[3U];
            audit->general_flags = descriptor[4U];
            audit->specific_flags = descriptor[5U];
        }
        if (translation) return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_TRANSLATED);
        if ((memory && (descriptor[3U] != 0U || granularity != (wide ? 64U : 32U))) ||
            (!memory && descriptor[3U] != 1U))
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_TYPE_MISMATCH);
        if (!span || span - 1ULL > ~0ULL - base || total > ~0ULL - span)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_EXTENT);
        if (!expected || total > ~0ULL - expected)
            return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_EXTENT);
        if (base != expected + total)
            return boot_display_bar_fail(audit, count == 1U ?
                BOOT_DISPLAY_BAR_BASE_MISMATCH : BOOT_DISPLAY_BAR_NONCONTIGUOUS);
        total += span;
        offset += 46U;
    }
    /* AddrRangeMax is reported for diagnosis, not used as an endpoint: EDK2
     * versions also use it for alignment. Base + length is checked directly. */
    if (!total || (total & (total - 1ULL)) || (expected & (total - 1ULL)) ||
        total - 1ULL > ~0ULL - expected ||
        (!(flags & BOOT_DISPLAY_BAR_64BIT) && expected + total - 1ULL > 0xFFFFFFFFULL))
        return boot_display_bar_fail(audit, BOOT_DISPLAY_BAR_EXTENT);
    out->base = expected;
    out->size = total;
    out->flags = flags;
    if (audit) audit->result = BOOT_DISPLAY_BAR_ACCEPTED;
    return 1;
}

static inline int boot_display_decode_bar(const u8 *bytes, u32 size,
    u32 low, u32 high, u32 index, BootDisplayBar *out) {
    return boot_display_decode_bar_audited(bytes, size, low, high, index, out, 0);
}

/* Classify a physical config-header slot, never treating the high half of a
 * 64-bit BAR as another BAR even if its bits resemble a valid low half. */
static inline u32 boot_display_bar_slot(const u32 *config, u32 index) {
    if (!config || index >= BOOT_DISPLAY_BAR_COUNT) return BOOT_DISPLAY_BAR_MALFORMED;
    for (u32 i = 0; i <= index; ++i) {
        u32 low = config[4U + i];
        u32 type = (low >> 1U) & 3U;
        u32 state = BOOT_DISPLAY_BAR_NOT_QUERIED; /* assigned; eligible for matching */
        if (!low) state = BOOT_DISPLAY_BAR_UNASSIGNED;
        else if (low == 0xFFFFFFFFU || (!(low & 1U) &&
            (type != 0U && type != 2U)))
            state = BOOT_DISPLAY_BAR_TYPE_MISMATCH;
        else if (!(low & 1U) && type == 2U) {
            if (i + 1U == BOOT_DISPLAY_BAR_COUNT) return BOOT_DISPLAY_BAR_TYPE_MISMATCH;
            if (index == i + 1U) return BOOT_DISPLAY_BAR_CONTINUATION;
            if (index != i) { ++i; continue; }
        }
        if (i == index) return state;
    }
    return BOOT_DISPLAY_BAR_MALFORMED;
}

/* Exact content association for inventory only. Firmware numbering is retained,
 * never applied to MMIO/IO operations. Refuse nonstandard descriptor types and
 * do not guess a dense numbering scheme. A multi-bit match is ambiguous. */
static inline void boot_display_match_query(const u8 *bytes, u32 size, const u32 *config,
    u32 query_index, BootDisplayBarQuery *query, BootDisplayBar *extent) {
    if (!query || !extent) return;
    boot_display_zero(query, sizeof(*query));
    boot_display_zero(extent, sizeof(*extent));
    query->audit.result = BOOT_DISPLAY_BAR_NO_MATCH;
    if (!bytes || !config || query_index >= BOOT_DISPLAY_BAR_COUNT ||
        size > BOOT_DISPLAY_BAR_RESOURCE_BYTES) {
        query->audit.result = BOOT_DISPLAY_BAR_MALFORMED;
        return;
    }
    query->prefix_size = size < BOOT_DISPLAY_QUERY_PREFIX_BYTES ? size : BOOT_DISPLAY_QUERY_PREFIX_BYTES;
    boot_display_copy(query->prefix, bytes, query->prefix_size);
    int have_audit = 0;
    for (u32 bar = 0; bar < BOOT_DISPLAY_BAR_COUNT; ++bar) {
        if (boot_display_bar_slot(config, bar) != BOOT_DISPLAY_BAR_NOT_QUERIED) continue;
        u32 low = config[4U + bar];
        u32 high = (!(low & 1U) && ((low >> 1U) & 3U) == 2U) ? config[5U + bar] : 0U;
        BootDisplayBar candidate;
        BootDisplayBarAudit audit;
        int ok = boot_display_decode_bar_audited(bytes, size, low, high, bar, &candidate, &audit);
        /* Prefer the physical slot's rejection when no exact match exists. */
        if (!query->matching_bars && (!have_audit || bar == query_index)) {
            boot_display_copy(&query->audit, &audit, sizeof(audit));
            have_audit = 1;
        }
        if (!ok) continue;
        if (!query->matching_bars) {
            boot_display_copy(extent, &candidate, sizeof(candidate));
            boot_display_copy(&query->audit, &audit, sizeof(audit));
        }
        query->matching_bars |= 1U << bar;
    }
    if (query->matching_bars & (query->matching_bars - 1U)) {
        query->audit.result = BOOT_DISPLAY_BAR_AMBIGUOUS;
        boot_display_zero(extent, sizeof(*extent));
    }
}

static inline int boot_display_edid_valid(const u8 *bytes, u32 size) {
    if (!bytes || size < BOOT_DISPLAY_EDID_BYTES || bytes[0] || bytes[7U] ||
        bytes[18U] != 1U || bytes[19U] > 4U) return 0;
    for (u32 i = 1U; i < 7U; ++i)
        if (bytes[i] != 0xFFU) return 0;
    u8 sum = 0;
    for (u32 i = 0; i < BOOT_DISPLAY_EDID_BYTES; ++i) sum = (u8)(sum + bytes[i]);
    return !sum;
}

#endif
