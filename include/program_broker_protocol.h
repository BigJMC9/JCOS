#ifndef JCOS_PROGRAM_BROKER_PROTOCOL_H
#define JCOS_PROGRAM_BROKER_PROTOCOL_H

#include "user_abi.h"

#define JCOS_PROGRAM_BROKER_PROTOCOL_VERSION 1ULL
#define JCOS_PROGRAM_BROKER_OP_FOREGROUND_EXTENT 1ULL

#define JCOS_PROGRAM_BROKER_HEADER(op, version) \
    ((((JcosU64)(version) & 0xFFFFULL) << 48) | ((JcosU64)(op) & 0xFFFFULL))
#define JCOS_PROGRAM_BROKER_HEADER_OP(value) ((JcosU64)(value) & 0xFFFFULL)
#define JCOS_PROGRAM_BROKER_HEADER_VERSION(value) (((JcosU64)(value) >> 48) & 0xFFFFULL)

/* One-way policy request from the Ring3 shell service to the privileged launch
 * mechanism. No pathname crosses this boundary.
 *
 * word0 = header(op, version)
 * word1 = console-service incarnation
 * word2 = immutable boot-archive data offset
 * word3 = exact executable byte length
 */

#endif
