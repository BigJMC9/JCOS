#ifndef JCOS_ORDINARY_SERVICE_PROTOCOL_H
#define JCOS_ORDINARY_SERVICE_PROTOCOL_H

#include "user_abi.h"

#define JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION 1ULL

#define JCOS_ORDINARY_SERVICE_OP_PING       1ULL
#define JCOS_ORDINARY_SERVICE_OP_SHUTDOWN   2ULL
#define JCOS_ORDINARY_SERVICE_OP_IDENTIFY   3ULL
#define JCOS_ORDINARY_SERVICE_OP_DIAG_FAULT 0xFF41ULL

#define JCOS_ORDINARY_SERVICE_REPLY_PONG    0x8000000000000401ULL
#define JCOS_ORDINARY_SERVICE_REPLY_STOPPED 0x8000000000000402ULL
#define JCOS_ORDINARY_SERVICE_REPLY_IDENTITY 0x8000000000000403ULL

#define JCOS_ORDINARY_SERVICE_IMAGE_PRIMARY     1ULL
#define JCOS_ORDINARY_SERVICE_IMAGE_REPLACEMENT 2ULL

#define JCOS_ORDINARY_SERVICE_HEADER(op, version) \
    ((((JcosU64)(version) & 0xFFFFULL) << 48) | ((JcosU64)(op) & 0xFFFFULL))
#define JCOS_ORDINARY_SERVICE_HEADER_OP(value) ((JcosU64)(value) & 0xFFFFULL)
#define JCOS_ORDINARY_SERVICE_HEADER_VERSION(value) (((JcosU64)(value) >> 48) & 0xFFFFULL)

/* Common lifecycle profile for an ordinary restartable service.
 * Request word 0 is the versioned header. Replies carry the service
 * incarnation in word 1 so old authority cannot silently target a replacement. */

#endif
