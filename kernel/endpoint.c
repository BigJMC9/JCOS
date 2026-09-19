#include "endpoint.h"
#include "lib.h"
#include "interrupts.h"
#include "object_storage.h"
#include "process.h"

static u64 g_next_endpoint_id;
static bool g_initialized;
static void *g_endpoints[ENDPOINT_STORAGE_CAPACITY];

static u64 endpoint_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void endpoint_irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }
static bool endpoint_live(const Endpoint *endpoint) {
    return object_storage_find(g_endpoints, ENDPOINT_STORAGE_CAPACITY, endpoint) < ENDPOINT_STORAGE_CAPACITY;
}
bool endpoint_storage_in_use(const Endpoint *endpoint) {
    u64 flags = endpoint_irq_save();
    bool result = endpoint_live(endpoint);
    endpoint_irq_restore(flags);
    return result;
}
u32 endpoint_object_count(void) {
    u64 flags = endpoint_irq_save();
    u32 result = object_storage_count(g_endpoints, ENDPOINT_STORAGE_CAPACITY);
    endpoint_irq_restore(flags);
    return result;
}

bool endpoint_system_init(void) {
    if (g_initialized) return false;

    g_next_endpoint_id = 1ULL;
    g_initialized = true;
    return true;
}

static bool endpoint_create_locked(Endpoint *endpoint, u64 owner_process_id) {
    if (!g_initialized || !endpoint || !g_next_endpoint_id || endpoint_live(endpoint)) return false;
    u32 slot = object_storage_empty(g_endpoints, ENDPOINT_STORAGE_CAPACITY);
    if (slot == ENDPOINT_STORAGE_CAPACITY) return false;

    k_memset(endpoint, 0, sizeof(*endpoint));

    endpoint->id = g_next_endpoint_id++;
    endpoint->owner_process_id = owner_process_id;
    endpoint->initialized = true;

    g_endpoints[slot] = endpoint; /* ID exhaustion leaves zero: never wrap to one. */
    return true;
}

bool endpoint_create(Endpoint *endpoint) {
    u64 flags = endpoint_irq_save();
    bool result = endpoint_create_locked(endpoint, 0);
    endpoint_irq_restore(flags);
    return result;
}

bool endpoint_create_owned(Endpoint *endpoint, Process *owner) {
    u64 flags = endpoint_irq_save();
    CapabilityTable *caps = process_capabilities(owner);
    bool result = caps && owner->id && endpoint_create_locked(endpoint, owner->id);
    endpoint_irq_restore(flags);
    return result;
}

static bool endpoint_destroy_locked(Endpoint *endpoint) {
    if (!g_initialized || !endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id) return false;
    if (endpoint->capability_refs) return false;
    if (endpoint->message_ready) return false;
    if (endpoint->waiting_receiver || endpoint->waiting_receiver_id) return false;
    if (endpoint->waiting_sender || endpoint->waiting_sender_id) return false;
    if (endpoint->waiting_sender_message_ready) return false;

    u32 slot = object_storage_find(g_endpoints, ENDPOINT_STORAGE_CAPACITY, endpoint);
    g_endpoints[slot] = 0;
    k_memset(endpoint, 0, sizeof(*endpoint));

    return true;
}

bool endpoint_destroy(Endpoint *endpoint) {
    u64 flags = endpoint_irq_save();
    bool result = endpoint_destroy_locked(endpoint);
    endpoint_irq_restore(flags);
    return result;
}

bool endpoint_try_send(Endpoint *endpoint, const IpcMessage *message) {
    if (!g_initialized || !endpoint || !message) return false;
    if (!endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id || endpoint->closed) return false;
    if (!message->word_count || message->word_count > IPC_MESSAGE_MAX_WORDS) return false;
    if (endpoint->message_ready) return false;

    k_memset(&endpoint->message, 0, sizeof(endpoint->message));

    endpoint->message.word_count = message->word_count;

    for (u32 i = 0; i < message->word_count; ++i) endpoint->message.words[i] = message->words[i];

    endpoint->message_ready = true;
    return true;
}

bool endpoint_try_receive(Endpoint *endpoint, IpcMessage *out_message) {
    if (!out_message) return false;

    k_memset(out_message, 0, sizeof(*out_message));

    if (!g_initialized || !endpoint) return false;
    if (!endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id || endpoint->closed) return false;
    if (!endpoint->message_ready) return false;

    *out_message = endpoint->message;

    k_memset(&endpoint->message, 0, sizeof(endpoint->message));

    endpoint->message_ready = false;
    return true;
}

bool endpoint_message_ready(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->message_ready;
}

bool endpoint_receiver_waiting(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->waiting_receiver != 0;
}

bool endpoint_sender_waiting(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->waiting_sender != 0;
}

bool endpoint_closed(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint_live(endpoint) || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->closed;
}

u64 endpoint_owner_process_id(const Endpoint *endpoint) {
    u64 flags = endpoint_irq_save();
    u64 result = endpoint_live(endpoint) && endpoint->initialized && endpoint->id
        ? endpoint->owner_process_id : 0;
    endpoint_irq_restore(flags);
    return result;
}

static Endpoint *endpoint_owned_from(u64 owner_process_id, u32 start) {
    if (!owner_process_id) return 0;
    for (u32 i = start; i < ENDPOINT_STORAGE_CAPACITY; ++i) {
        Endpoint *endpoint = (Endpoint *)g_endpoints[i];
        if (endpoint && endpoint->initialized && endpoint->id &&
            endpoint->owner_process_id == owner_process_id) return endpoint;
    }
    return 0;
}

Endpoint *endpoint_owned_first(u64 owner_process_id) {
    u64 flags = endpoint_irq_save();
    Endpoint *result = endpoint_owned_from(owner_process_id, 0);
    endpoint_irq_restore(flags);
    return result;
}

Endpoint *endpoint_owned_next(u64 owner_process_id, const Endpoint *endpoint) {
    u64 flags = endpoint_irq_save();
    Endpoint *result = 0;
    u32 slot = object_storage_find(g_endpoints, ENDPOINT_STORAGE_CAPACITY, endpoint);
    if (slot < ENDPOINT_STORAGE_CAPACITY && endpoint->owner_process_id == owner_process_id &&
        slot + 1U < ENDPOINT_STORAGE_CAPACITY) {
        result = endpoint_owned_from(owner_process_id, slot + 1U);
    }
    endpoint_irq_restore(flags);
    return result;
}

bool endpoint_process_has_open_owned(u64 owner_process_id) {
    u64 flags = endpoint_irq_save();
    bool result = false;
    for (u32 i = 0; owner_process_id && i < ENDPOINT_STORAGE_CAPACITY; ++i) {
        Endpoint *endpoint = (Endpoint *)g_endpoints[i];
        if (endpoint && endpoint->initialized && endpoint->id && !endpoint->closed &&
            endpoint->owner_process_id == owner_process_id) {
            result = true;
            break;
        }
    }
    endpoint_irq_restore(flags);
    return result;
}
