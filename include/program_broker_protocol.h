#ifndef JCOS_PROGRAM_BROKER_PROTOCOL_H
#define JCOS_PROGRAM_BROKER_PROTOCOL_H

#include "user_abi.h"

#define JCOS_PROGRAM_BROKER_PROTOCOL_VERSION 1ULL
#define JCOS_PROGRAM_BROKER_OP_FOREGROUND_EXTENT 1ULL
#define JCOS_PROGRAM_BROKER_OP_FOREGROUND_MEDIA 2ULL

#define JCOS_PROGRAM_BROKER_HEADER(op, version) \
    ((((JcosU64)(version) & 0xFFFFULL) << 48) | ((JcosU64)(op) & 0xFFFFULL))
#define JCOS_PROGRAM_BROKER_HEADER_OP(value) ((JcosU64)(value) & 0xFFFFULL)
#define JCOS_PROGRAM_BROKER_HEADER_VERSION(value) (((JcosU64)(value) >> 48) & 0xFFFFULL)

#define JCOS_PROGRAM_BROKER_PACK_EXTENT(offset, size) \
    ((((JcosU64)(size) & 0xFFFFFFFFULL) << 32) | ((JcosU64)(offset) & 0xFFFFFFFFULL))
#define JCOS_PROGRAM_BROKER_EXTENT_OFFSET(value) ((JcosU64)(value) & 0xFFFFFFFFULL)
#define JCOS_PROGRAM_BROKER_EXTENT_SIZE(value) (((JcosU64)(value) >> 32) & 0xFFFFFFFFULL)

/* One-way policy request from the Ring3 shell service to the privileged launch
 * mechanism. No pathname crosses this boundary.
 *
 * word0 = header(op, version)
 * word1 = console-service incarnation
 * word2 = immutable boot-archive data offset
 * word3 = exact executable byte length
 *
 * FOREGROUND_MEDIA instead packs the executable extent in word2 and the
 * read-only media extent in word3. Each offset and size is a 32-bit field;
 * the boot archive is rejected if either extent cannot be represented.
 */

#endif
