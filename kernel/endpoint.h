#ifndef JA_OS_ENDPOINT_H
#define JA_OS_ENDPOINT_H

#include "types.h"

#define IPC_MESSAGE_MAX_WORDS 4U
#define ENDPOINT_STORAGE_CAPACITY 256U

typedef struct {
    u64 words[IPC_MESSAGE_MAX_WORDS];
    u32 word_count;
} IpcMessage;

struct Thread;
struct Process;

typedef struct Endpoint {
    u64 id;

    /* Owning service/process incarnation. Zero means ownerless. */
    u64 owner_process_id;

    IpcMessage message;
    bool message_ready;

    /*
     * One receiver may wait for a message.
     */
    struct Thread *waiting_receiver;
    u64 waiting_receiver_id;

    /*
     * One sender may wait for mailbox space.
     *
     * Its message is copied here before the
     * sender blocks.
     */
    struct Thread *waiting_sender;
    u64 waiting_sender_id;

    IpcMessage waiting_sender_message;
    bool waiting_sender_message_ready;

    bool closed;
    bool initialized;

    /* Owned by occupied capability slots; changed only by capability.c. */
    u64 capability_refs;
} Endpoint;

bool endpoint_system_init(void);
bool endpoint_storage_in_use(const Endpoint *endpoint);
u32 endpoint_object_count(void);
bool endpoint_create(Endpoint *endpoint);
bool endpoint_create_owned(Endpoint *endpoint, struct Process *owner);
bool endpoint_destroy(Endpoint *endpoint);

bool endpoint_try_send(Endpoint *endpoint, const IpcMessage *message);
bool endpoint_try_receive(Endpoint *endpoint, IpcMessage *out_message);
bool endpoint_message_ready(const Endpoint *endpoint);
bool endpoint_receiver_waiting(const Endpoint *endpoint);
bool endpoint_sender_waiting(const Endpoint *endpoint);
bool endpoint_closed(const Endpoint *endpoint);
u64 endpoint_owner_process_id(const Endpoint *endpoint);

/* Registry iteration is stable under the current UP/local-IRQ exclusion model. */
Endpoint *endpoint_owned_first(u64 owner_process_id);
Endpoint *endpoint_owned_next(u64 owner_process_id, const Endpoint *endpoint);
bool endpoint_process_has_open_owned(u64 owner_process_id);

#endif