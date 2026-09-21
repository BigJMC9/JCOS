#include "capability.h"
#include "capability_test.h"
#include "address_space.h"
#include "endpoint.h"
#include "thread.h"
#include "device_resource.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"

static void *g_tables[CAPABILITY_TABLE_STORAGE_CAPACITY];
/* One boot-global issuer per slot index: each (generation,index) pair is
 * unique across table/object reincarnations. Exhausted indices are skipped. */
static u64 g_next_generation[CAPABILITY_TABLE_CAPACITY];
static bool g_issuers_initialized;

static u64 cap_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void cap_irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }

static bool table_live(const CapabilityTable *table) {
    return object_storage_find(g_tables, CAPABILITY_TABLE_STORAGE_CAPACITY, table) < CAPABILITY_TABLE_STORAGE_CAPACITY;
}

bool capability_table_storage_in_use(const CapabilityTable *table) {
    u64 flags = cap_irq_save();
    bool result = table_live(table);
    cap_irq_restore(flags);
    return result;
}

u32 capability_table_object_count(void) {
    u64 flags = cap_irq_save();
    u32 result = object_storage_count(g_tables, CAPABILITY_TABLE_STORAGE_CAPACITY);
    cap_irq_restore(flags);
    return result;
}

static bool type_valid(CapabilityType type) {
    return type == CAPABILITY_TYPE_THREAD || type == CAPABILITY_TYPE_ADDRESS_SPACE ||
        type == CAPABILITY_TYPE_ENDPOINT || type == CAPABILITY_TYPE_DEVICE_RESOURCE;
}

/* Registry membership is checked BEFORE dereferencing a supplied object address.
 * References protect storage lifetime, not execution, delivery, or service health. */
static u64 *object_refs(void *object, CapabilityType type, u64 *identity, bool inserting) {
    if (!object || !identity) return 0;
    if (type == CAPABILITY_TYPE_THREAD) {
        Thread *thread = object;
        if (!thread_storage_in_use(thread) || !thread->id || !thread->process ||
            thread->state == THREAD_STATE_INVALID) return 0;
        if (inserting && thread->state == THREAD_STATE_DEAD) return 0;
        *identity = thread->id;
        return &thread->capability_refs;
    }
    if (type == CAPABILITY_TYPE_ADDRESS_SPACE) {
        AddressSpace *space = object;
        if (!address_space_storage_in_use(space)) return 0;
        if (inserting && (!address_space_cr3(space) || space->page_map.destroy_pending)) return 0;
        *identity = space->id;
        return &space->capability_refs;
    }
    if (type == CAPABILITY_TYPE_ENDPOINT) {
        Endpoint *endpoint = object;
        if (!endpoint_storage_in_use(endpoint) || !endpoint->initialized || !endpoint->id) return 0;
        if (inserting && endpoint->closed) return 0;
        *identity = endpoint->id;
        return &endpoint->capability_refs;
    }
    if (type == CAPABILITY_TYPE_DEVICE_RESOURCE) {
        DeviceResource *resource = object;
        if (!device_resource_valid(resource)) return 0;
        *identity = resource->id;
        return &resource->capability_refs;
    }
    return 0;
}

static u64 *slot_refs(const CapabilitySlot *slot) {
    if (!slot->occupied || !slot->generation || !slot->rights || (slot->rights & ~CAPABILITY_RIGHT_ALL)) return 0;
    u64 identity = 0;
    u64 *refs = object_refs(slot->object, slot->type, &identity, false);
    return refs && *refs && identity == slot->object_id ? refs : 0;
}

static bool table_valid(const CapabilityTable *table) {
    if (!table_live(table) || !table->initialized || table->count > CAPABILITY_TABLE_CAPACITY) return false;
    u32 count = 0;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *slot = &table->slots[i];
        if (slot->occupied) {
            if (!slot_refs(slot)) return false;
            ++count;
        } else if (slot->object || slot->object_id || slot->rights || slot->type != CAPABILITY_TYPE_NONE) return false;
    }
    return count == table->count;
}

static bool take_generation(u64 *next, u32 *out) {
    if (!next || !out || !*next || *next > CAPABILITY_GENERATION_MAX) return false;
    *out = (u32)*next;
    ++*next; /* u64 sentinel MAX+1 cannot wrap the 32-bit handle field. */
    return true;
}

/* Boundary check on a private counter: never reset or rewind the live issuer. */
bool capability_test_generation_boundary(void) {
    u64 next = CAPABILITY_GENERATION_MAX;
    u32 last = 0, untouched = 0x12345678U;
    return take_generation(&next, &last) && last == (u32)CAPABILITY_GENERATION_MAX &&
        next == CAPABILITY_GENERATION_MAX + 1ULL && !take_generation(&next, &untouched) &&
        untouched == 0x12345678U && next == CAPABILITY_GENERATION_MAX + 1ULL;
}

bool capability_table_init(CapabilityTable *table) {
    if (!table) return false;
    u64 flags = cap_irq_save();
    bool result = false;
    u32 slot = object_storage_empty(g_tables, CAPABILITY_TABLE_STORAGE_CAPACITY);
    if (!table_live(table) && slot < CAPABILITY_TABLE_STORAGE_CAPACITY) {
        if (!g_issuers_initialized) {
            for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) g_next_generation[i] = 1ULL;
            g_issuers_initialized = true;
        }
        k_memset(table, 0, sizeof(*table));
        table->initialized = true;
        g_tables[slot] = table;
        result = true;
    }
    cap_irq_restore(flags);
    return result;
}

bool capability_table_destroy(CapabilityTable *table) {
    u64 flags = cap_irq_save();
    bool result = table_valid(table) && table->count == 0U;
    if (result) {
        u32 slot = object_storage_find(g_tables, CAPABILITY_TABLE_STORAGE_CAPACITY, table);
        g_tables[slot] = 0;
        k_memset(table, 0, sizeof(*table));
    }
    cap_irq_restore(flags);
    return result;
}

u32 capability_table_count(const CapabilityTable *table) {
    u64 flags = cap_irq_save();
    u32 count = table_live(table) && table->initialized ? table->count : 0U;
    cap_irq_restore(flags);
    return count;
}

bool capability_table_empty(const CapabilityTable *table) {
    u64 flags = cap_irq_save();
    bool result = table_valid(table) && table->count == 0U;
    cap_irq_restore(flags);
    return result;
}

bool capability_insert(CapabilityTable *table, void *object, CapabilityType type, CapabilityRights rights,
    CapabilityHandle *out_handle) {
    if (!out_handle) return false;
    *out_handle = CAPABILITY_INVALID_HANDLE;
    u64 flags = cap_irq_save();
    bool result = false;
    if (!table_valid(table) || !type_valid(type) || !rights || (rights & ~CAPABILITY_RIGHT_ALL) ||
        table->count == CAPABILITY_TABLE_CAPACITY) goto done;
    u64 identity = 0;
    u64 *refs = object_refs(object, type, &identity, true);
    if (!refs || *refs == ~0ULL) goto done;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        CapabilitySlot *slot = &table->slots[i];
        if (slot->occupied) continue;
        u32 generation = 0;
        if (!take_generation(&g_next_generation[i], &generation)) continue;
        slot->object = object;
        slot->object_id = identity;
        slot->type = type;
        slot->rights = rights;
        slot->generation = generation;
        slot->occupied = true;
        ++*refs;
        ++table->count;
        *out_handle = ((u64)generation << 32) | i;
        result = true;
        break;
    }
done:
    cap_irq_restore(flags);
    return result;
}

static CapabilitySlot *resolve_slot(const CapabilityTable *table, CapabilityHandle handle) {
    if (!table_live(table) || !table->initialized || !handle) return 0;
    u32 index = (u32)handle, generation = (u32)(handle >> 32);
    if (index >= CAPABILITY_TABLE_CAPACITY || !generation) return 0;
    const CapabilitySlot *slot = &table->slots[index];
    if (slot->generation != generation || !slot_refs(slot)) return 0;
    return (CapabilitySlot *)slot;
}

bool capability_lookup_rights(const CapabilityTable *table, CapabilityHandle handle,
    CapabilityType expected_type, CapabilityRights required_rights, void **out_object) {
    if (!out_object) return false;
    *out_object = 0;
    u64 flags = cap_irq_save();
    CapabilitySlot *slot = resolve_slot(table, handle);
    bool result = type_valid(expected_type) && !(required_rights & ~CAPABILITY_RIGHT_ALL) && slot &&
        slot->type == expected_type && (slot->rights & required_rights) == required_rights;
    if (result) *out_object = slot->object;
    cap_irq_restore(flags);
    return result;
}

bool capability_lookup(const CapabilityTable *table, CapabilityHandle handle,
    CapabilityType expected_type, void **out_object) {
    return capability_lookup_rights(table, handle, expected_type, 0, out_object);
}

static void delete_slot(CapabilityTable *table, CapabilitySlot *slot) {
    /* All callers prevalidate and hold IRQ exclusion through the mutation. */
    u64 *refs = slot_refs(slot);
    --*refs;
    slot->object = 0;
    slot->object_id = 0;
    slot->rights = 0;
    slot->type = CAPABILITY_TYPE_NONE;
    slot->occupied = false;
    --table->count;
}

bool capability_revoke(CapabilityTable *table, CapabilityHandle handle) {
    u64 flags = cap_irq_save();
    CapabilitySlot *slot = resolve_slot(table, handle);
    bool result = table_valid(table) && slot;
    if (result) delete_slot(table, slot);
    cap_irq_restore(flags);
    return result;
}

static bool delete_selected(CapabilityTable *table, const void *object, CapabilityType type, bool all) {
    if (!table_valid(table)) return false;
    /* Check multiplicity before deleting anything; repeated grants each own a pin. */
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        CapabilitySlot *slot = &table->slots[i];
        if (!slot->occupied || (!all && (slot->object != object || slot->type != type))) continue;
        u32 matching = 0;
        for (u32 j = 0; j < CAPABILITY_TABLE_CAPACITY; ++j) {
            CapabilitySlot *other = &table->slots[j];
            if (other->occupied && other->object == slot->object && other->type == slot->type) ++matching;
        }
        u64 *refs = slot_refs(slot);
        if (!refs || *refs < matching) return false;
    }
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        CapabilitySlot *slot = &table->slots[i];
        if (slot->occupied && (all || (slot->object == object && slot->type == type))) delete_slot(table, slot);
    }
    return true;
}

bool capability_revoke_all(CapabilityTable *table) {
    u64 flags = cap_irq_save();
    bool result = delete_selected(table, 0, CAPABILITY_TYPE_NONE, true);
    cap_irq_restore(flags);
    return result;
}

bool capability_revoke_object(CapabilityTable *table, const void *object, CapabilityType type) {
    if (!object || !type_valid(type)) return false;
    u64 flags = cap_irq_save();
    bool result = delete_selected(table, object, type, false);
    cap_irq_restore(flags);
    return result;
}
