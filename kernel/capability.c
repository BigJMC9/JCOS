#include "capability.h"
#include "lib.h"

static bool capability_type_valid(CapabilityType type) {
    return (
        type == CAPABILITY_TYPE_THREAD ||
        type == CAPABILITY_TYPE_ADDRESS_SPACE ||
        type == CAPABILITY_TYPE_ENDPOINT
    );
}

static CapabilityHandle capability_make_handle(u32 slot, u32 generation) {
    if (slot >= CAPABILITY_TABLE_CAPACITY || !generation) return CAPABILITY_INVALID_HANDLE;
    return ((u64)generation << 32) | (u64)slot;
}

static bool capability_decode_handle(CapabilityHandle handle, u32 *out_slot, u32 *out_generation) {
    if (handle == CAPABILITY_INVALID_HANDLE || !out_slot || !out_generation) return false;

    u32 slot = (u32)(handle & 0xFFFFFFFFULL);
    u32 generation = (u32)(handle >> 32);
    if (slot >= CAPABILITY_TABLE_CAPACITY || !generation) return false;

    *out_slot = slot;
    *out_generation = generation;

    return true;
}

static u32 capability_next_generation(u32 generation) {
    ++generation;

    /* Generation zero is reserved so that zero/partially-zero handles can never become valid accidentally. */
    if (!generation) generation = 1U;
    return generation;
}

bool capability_table_init(CapabilityTable *table) {
    if (!table) return false;
    k_memset(table, 0, sizeof(*table));

    /* Generation zero is never issued. */
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) table->slots[i].generation = 1U;
    table->count = 0;
    table->initialized = true;
    return true;
}

u32 capability_table_count(const CapabilityTable *table) {
    if (!table || !table->initialized) return 0;
    return table->count;
}

bool capability_insert(CapabilityTable *table, void *object, CapabilityType type, CapabilityRights rights, CapabilityHandle *out_handle) {
    if (!out_handle) return false;
    *out_handle = CAPABILITY_INVALID_HANDLE;

    if (!table || !table->initialized || !object || !capability_type_valid(type) || !rights || table->count >= CAPABILITY_TABLE_CAPACITY) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {

        CapabilitySlot *slot = &table->slots[i];
        if (slot->occupied) continue;

        /* Defensive repair in case corrupted or future initialization code ever leaves a free slot at generation zero. */
        if (!slot->generation) slot->generation = 1U;
        CapabilityHandle handle = capability_make_handle(i, slot->generation);

        if (handle == CAPABILITY_INVALID_HANDLE) return false;
        slot->object = object;
        slot->type = type;
        slot->rights = rights;
        slot->occupied = true;
        ++table->count;
        *out_handle = handle;
        return true;
    }

    return false;
}

bool capability_lookup_rights(const CapabilityTable *table, CapabilityHandle handle, CapabilityType expected_type, CapabilityRights required_rights, void **out_object) {
    if (!out_object) return false;
    *out_object = 0;

    if (!table || !table->initialized || !capability_type_valid(expected_type)) return false;

    u32 slot_index = 0;
    u32 generation = 0;

    if (!capability_decode_handle(handle, &slot_index, &generation)) return false;
    const CapabilitySlot *slot = &table->slots[slot_index];

    if (!slot->occupied || !slot->object || slot->generation != generation || slot->type != expected_type) return false;
    if ((slot->rights & required_rights) != required_rights) return false;
    *out_object = slot->object;
    return true;
}

bool capability_lookup(const CapabilityTable *table, CapabilityHandle handle, CapabilityType expected_type, void **out_object) {
    return capability_lookup_rights(table, handle, expected_type, 0, out_object);
}

bool capability_revoke(CapabilityTable *table, CapabilityHandle handle) {
    if (!table || !table->initialized || !table->count) return false;

    u32 slot_index = 0;
    u32 generation = 0;

    if (!capability_decode_handle(handle, &slot_index, &generation)) return false;
    CapabilitySlot *slot = &table->slots[slot_index];

    /* Generation mismatch is exactly how stale handles are rejected. */
    if (!slot->occupied || !slot->object || slot->generation != generation) return false;
    u32 next_generation = capability_next_generation(slot->generation);

    slot->object = 0;
    slot->rights = 0;
    slot->type = CAPABILITY_TYPE_NONE;
    slot->occupied = false;
    slot->generation = next_generation;

    --table->count;

    return true;
}