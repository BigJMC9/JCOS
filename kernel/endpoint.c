#include "endpoint.h"
#include "lib.h"

static u64 g_next_endpoint_id;
static bool g_initialized;

bool endpoint_system_init(void) {
    if (g_initialized) return false;

    g_next_endpoint_id = 1ULL;
    g_initialized = true;
    return true;
}

bool endpoint_create(Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !g_next_endpoint_id) return false;

    k_memset(endpoint, 0, sizeof(*endpoint));

    endpoint->id = g_next_endpoint_id++;
    endpoint->initialized = true;

    if (!g_next_endpoint_id) g_next_endpoint_id = 1ULL;
    return true;
}

bool endpoint_destroy(Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !endpoint->initialized || !endpoint->id) return false;
    if (endpoint->message_ready) return false;
    if (endpoint->waiting_receiver) return false;
    if (endpoint->waiting_sender) return false;
    if (endpoint->waiting_sender_message_ready) return false;

    k_memset(endpoint, 0, sizeof(*endpoint));

    return true;
}

bool endpoint_try_send(Endpoint *endpoint, const IpcMessage *message) {
    if (!g_initialized || !endpoint || !message) return false;
    if (!endpoint->initialized || !endpoint->id || endpoint->closed) return false;
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
    if (!endpoint->initialized || !endpoint->id || endpoint->closed) return false;
    if (!endpoint->message_ready) return false;

    *out_message = endpoint->message;

    k_memset(&endpoint->message, 0, sizeof(endpoint->message));

    endpoint->message_ready = false;
    return true;
}

bool endpoint_message_ready(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->message_ready;
}

bool endpoint_receiver_waiting(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->waiting_receiver != 0;
}

bool endpoint_sender_waiting(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->waiting_sender != 0;
}

bool endpoint_closed(const Endpoint *endpoint) {
    if (!g_initialized || !endpoint || !endpoint->initialized || !endpoint->id) return false;
    return endpoint->closed;
}