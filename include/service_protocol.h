#ifndef JCOS_SERVICE_PROTOCOL_H
#define JCOS_SERVICE_PROTOCOL_H

/*
 * Common bounded Ring3 service protocol conventions for R7+ services.
 *
 * Request words:
 *   word 0 = service operation
 *   word 1 = protocol version
 *   word 2..3 = operation payload
 *
 * Reply words:
 *   word 0 = service-specific result or common error below
 *   word 1 = service incarnation
 *   word 2 = protocol version supported by the replying service
 *   word 3 = optional result/error detail
 *
 * Capability authority and service incarnation are separate. A replacement
 * service requires an explicit reconnect; a protocol reply never retargets an
 * old capability handle. Requests are never replayed automatically.
 */
#define JCOS_SERVICE_PROTOCOL_VERSION_1 1ULL

#define JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION 0x8FFFFFFFFFFFFF01ULL
#define JCOS_SERVICE_REPLY_UNKNOWN_OPERATION    0x8FFFFFFFFFFFFF02ULL
#define JCOS_SERVICE_REPLY_INVALID_REQUEST      0x8FFFFFFFFFFFFF03ULL

#endif
