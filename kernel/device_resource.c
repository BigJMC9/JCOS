#include "device_resource.h"

#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"

#define DEVICE_RESOURCE_STORAGE_CAPACITY 8U

static void *g_resources[DEVICE_RESOURCE_STORAGE_CAPACITY];
static u64 g_next_id = 1ULL;

static u64 resource_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void resource_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) interrupts_enable();
}

bool device_resource_valid(const DeviceResource *resource) {
    u64 flags = resource_irq_save();
    bool valid = resource && resource->initialized && resource->id &&
        resource->kind != DEVICE_RESOURCE_NONE &&
        object_storage_find(g_resources, DEVICE_RESOURCE_STORAGE_CAPACITY, resource) <
            DEVICE_RESOURCE_STORAGE_CAPACITY;
    resource_irq_restore(flags);
    return valid;
}

bool device_resource_create(DeviceResource *resource, DeviceResourceKind kind) {
    if (!resource || kind == DEVICE_RESOURCE_NONE) return false;
    u64 flags = resource_irq_save();
    u32 slot = object_storage_empty(g_resources, DEVICE_RESOURCE_STORAGE_CAPACITY);
    bool created = false;
    if (slot < DEVICE_RESOURCE_STORAGE_CAPACITY &&
        object_storage_find(g_resources, DEVICE_RESOURCE_STORAGE_CAPACITY, resource) ==
            DEVICE_RESOURCE_STORAGE_CAPACITY && g_next_id) {
        k_memset(resource, 0, sizeof(*resource));
        resource->id = g_next_id++;
        resource->kind = kind;
        resource->initialized = true;
        g_resources[slot] = resource;
        created = true;
    }
    resource_irq_restore(flags);
    return created;
}

bool device_resource_destroy(DeviceResource *resource) {
    if (!resource) return false;
    u64 flags = resource_irq_save();
    u32 slot = object_storage_find(g_resources, DEVICE_RESOURCE_STORAGE_CAPACITY, resource);
    bool destroyed = slot < DEVICE_RESOURCE_STORAGE_CAPACITY && resource->initialized &&
        resource->id && !resource->capability_refs;
    if (destroyed) {
        g_resources[slot] = 0;
        k_memset(resource, 0, sizeof(*resource));
    }
    resource_irq_restore(flags);
    return destroyed;
}
