#ifndef JCOS_BOOT_ARCHIVE_PORTAL_ABI_H
#define JCOS_BOOT_ARCHIVE_PORTAL_ABI_H

/*
 * Minimal privileged read-only boot archive mechanism.
 *
 * This protocol deliberately has no path or filename operation. Ring3 policy
 * parses the archive namespace and decides which path names mean what.
 *
 * Request word 0 packs operation/version/count. word 1 is the caller's current
 * managed-service incarnation so replies from a dead incarnation can be
 * detected and discarded. READ also carries the byte offset in word 2.
 *
 * Reply word 0 packs result/version/count, word 1 echoes the service
 * incarnation, word 2 is operation detail (archive size or read offset), and
 * word 3 contains up to eight little-endian payload bytes.
 */
#define JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION 1ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES 8U

#define JCOS_BOOT_ARCHIVE_PORTAL_OP_INFO 1ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_OP_READ 2ULL

#define JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INFO 0x8001ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_REPLY_DATA 0x8002ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INCOMPATIBLE_VERSION 0x8FF1ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST 0x8FF2ULL
#define JCOS_BOOT_ARCHIVE_PORTAL_REPLY_OUT_OF_RANGE 0x8FF3ULL

#define JCOS_BOOT_ARCHIVE_PORTAL_HEADER(code, version, count) \
    (((code) & 0xFFFFULL) | (((version) & 0xFFFFULL) << 16) | (((count) & 0xFFULL) << 32))
#define JCOS_BOOT_ARCHIVE_PORTAL_HEADER_CODE(value) ((value) & 0xFFFFULL)
#define JCOS_BOOT_ARCHIVE_PORTAL_HEADER_VERSION(value) (((value) >> 16) & 0xFFFFULL)
#define JCOS_BOOT_ARCHIVE_PORTAL_HEADER_COUNT(value) (((value) >> 32) & 0xFFULL)

#endif
