#ifndef JCOS_SERVICE_BROKER_PROTOCOL_H
#define JCOS_SERVICE_BROKER_PROTOCOL_H

#include "user_abi.h"

#define JCOS_SERVICE_BROKER_PROTOCOL_VERSION 1ULL

#define JCOS_SERVICE_BROKER_OP_START_EXTENT   1ULL
#define JCOS_SERVICE_BROKER_OP_STOP           2ULL
#define JCOS_SERVICE_BROKER_OP_RESTART_EXTENT 3ULL
#define JCOS_SERVICE_BROKER_OP_STATUS         4ULL
#define JCOS_SERVICE_BROKER_OP_DIAG_FAULT     0x7FFFULL

#define JCOS_SERVICE_BROKER_STATE_STOPPED 0ULL
#define JCOS_SERVICE_BROKER_STATE_RUNNING 1ULL
#define JCOS_SERVICE_BROKER_STATE_FAILED  2ULL

#define JCOS_SERVICE_BROKER_RESULT_OK              1ULL
#define JCOS_SERVICE_BROKER_RESULT_ALREADY_RUNNING 2ULL
#define JCOS_SERVICE_BROKER_RESULT_NOT_RUNNING     3ULL
#define JCOS_SERVICE_BROKER_RESULT_FAILED          255ULL

#define JCOS_SERVICE_BROKER_HEADER(op, version) \
    ((((JcosU64)(version) & 0xFFFFULL) << 48) | ((JcosU64)(op) & 0xFFFFULL))
#define JCOS_SERVICE_BROKER_HEADER_OP(value) ((JcosU64)(value) & 0xFFFFULL)
#define JCOS_SERVICE_BROKER_HEADER_VERSION(value) (((JcosU64)(value) >> 48) & 0xFFFFULL)

/* One-way policy request from Ring3 to the privileged generic background
 * service mechanism. The kernel never receives a service name or pathname.
 *
 * START/RESTART:
 *   word0 = header(op, version)
 *   word1 = policy-service incarnation
 *   word2 = immutable boot-archive data offset
 *   word3 = exact executable byte length
 *
 * STOP/STATUS/DIAG_FAULT:
 *   word0 = header(op, version)
 *   word1 = policy-service incarnation
 *   word2..3 = 0
 */

#endif
