#ifndef JCOS_CONSOLE_CLIENT_PROTOCOL_H
#define JCOS_CONSOLE_CLIENT_PROTOCOL_H

/* Public foreground-application console output protocol. This endpoint is
 * separate from the console service's privileged management endpoint. */
#define JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION 1ULL

#define JCOS_CONSOLE_CLIENT_OP_WRITE 1ULL
#define JCOS_CONSOLE_CLIENT_OP_END   2ULL

#define JCOS_CONSOLE_CLIENT_MAX_WRITE_BYTES 16U

/* word 0: bits 0..15 operation, 16..31 version, 32..39 payload byte count. */
#define JCOS_CONSOLE_CLIENT_HEADER(op, version, count) \
    (((op) & 0xFFFFULL) | (((version) & 0xFFFFULL) << 16) | (((count) & 0xFFULL) << 32))
#define JCOS_CONSOLE_CLIENT_HEADER_OP(value) ((value) & 0xFFFFULL)
#define JCOS_CONSOLE_CLIENT_HEADER_VERSION(value) (((value) >> 16) & 0xFFFFULL)
#define JCOS_CONSOLE_CLIENT_HEADER_COUNT(value) (((value) >> 32) & 0xFFULL)

#endif
