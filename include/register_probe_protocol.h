#ifndef JCOS_REGISTER_PROBE_PROTOCOL_H
#define JCOS_REGISTER_PROBE_PROTOCOL_H
#include "device_register_abi.h"
#define REGISTER_PROBE_VERSION 1ULL
#define REGISTER_PROBE_HEADER(op) ((REGISTER_PROBE_VERSION << 32) | (op))
#define REGISTER_PROBE_CHECK    REGISTER_PROBE_HEADER(1ULL)
#define REGISTER_PROBE_READ     REGISTER_PROBE_HEADER(2ULL)
#define REGISTER_PROBE_FAULT    REGISTER_PROBE_HEADER(3ULL)
#define REGISTER_PROBE_SHUTDOWN REGISTER_PROBE_HEADER(4ULL)
#define REGISTER_PROBE_SPIN REGISTER_PROBE_HEADER(5ULL)
#define REGISTER_PROBE_CAPTURE REGISTER_PROBE_HEADER(6ULL)
#define REGISTER_PROBE_CAPTURE_DECODED (1ULL << 16)
#define REGISTER_PROBE_OBSERVATION REGISTER_PROBE_HEADER(7ULL)
/* Four-word replies preserve the existing IPC ABI.
 * CAPTURE: header,incarnation,result|decoded-bit|restrictions<<32,GPU address.
 * OBSERVATION request: op,expected resource ID,metadata selector,reserved0.
 * Reply: header,incarnation,selector+1,value (zero tag on refusal).
 * Metadata 0=span,1=width<<32|height,2=pitch,3=depth|format<<8|array<<16. */
#define REGISTER_PROBE_REPLY    0x52454750524F4245ULL
#define REGISTER_PROBE_STOPPED  0x52454753544F5050ULL
#define REGISTER_PROBE_EXPECTED 0xFFULL
#endif
