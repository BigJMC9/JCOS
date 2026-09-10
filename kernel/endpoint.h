#ifndef JA_OS_ENDPOINT_H
#define JA_OS_ENDPOINT_H

#include "types.h"

#define IPC_MESSAGE_MAX_WORDS 4U

typedef struct {
    u64 words[IPC_MESSAGE_MAX_WORDS];
    u32 word_count;
} IpcMessage;

struct Thread;

typedef struct Endpoint {
    u64 id;

    IpcMessage message;
    bool message_ready;

    /*
     * One receiver may wait for a message.
     */
    struct Thread *waiting_receiver;

    /*
     * One sender may wait for mailbox space.
     *
     * Its message is copied here before the
     * sender blocks.
     */
    struct Thread *waiting_sender;

    IpcMessage waiting_sender_message;
    bool waiting_sender_message_ready;

    bool initialized;
} Endpoint;

bool endpoint_system_init(void);
bool endpoint_create(Endpoint *endpoint);
bool endpoint_destroy(Endpoint *endpoint);

bool endpoint_try_send(Endpoint *endpoint, const IpcMessage *message);
bool endpoint_try_receive(Endpoint *endpoint, IpcMessage *out_message);
bool endpoint_message_ready(const Endpoint *endpoint);
bool endpoint_receiver_waiting(const Endpoint *endpoint);
bool endpoint_sender_waiting(const Endpoint *endpoint);

#endif