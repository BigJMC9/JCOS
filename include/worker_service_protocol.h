#ifndef JCOS_WORKER_SERVICE_PROTOCOL_H
#define JCOS_WORKER_SERVICE_PROTOCOL_H

/* R6b.2 demonstration service. Every reply carries the service incarnation in
 * word 1. Requests are never replayed automatically across an incarnation. */
#define WORKER_SERVICE_MESSAGE_PING      1ULL
#define WORKER_SERVICE_MESSAGE_SHUTDOWN  2ULL
#define WORKER_SERVICE_MESSAGE_FAULT     3ULL
#define WORKER_SERVICE_MESSAGE_HANG      4ULL

#define WORKER_SERVICE_REPLY_PONG        0x8000000000000101ULL
#define WORKER_SERVICE_REPLY_STOPPED     0x8000000000000102ULL

#endif