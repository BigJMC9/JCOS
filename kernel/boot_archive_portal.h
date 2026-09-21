#ifndef JA_OS_BOOT_ARCHIVE_PORTAL_H
#define JA_OS_BOOT_ARCHIVE_PORTAL_H

#include "capability.h"
#include "endpoint.h"
#include "program.h"
#include "thread.h"
#include "types.h"

/*
 * Privileged raw read-only mechanism for the immutable boot archive.
 *
 * The portal does not understand tar, paths, directories, executables or
 * application names. It accepts bounded offset reads from a configured byte
 * range. Namespace and file policy belong to Ring3.
 */
typedef struct {
    Endpoint request_endpoint;
    Endpoint reply_endpoint;
    Thread thread;

    CapabilityHandle kernel_request_receive_handle;
    CapabilityHandle kernel_request_transfer_handle;
    CapabilityHandle kernel_reply_send_handle;
    CapabilityHandle kernel_reply_transfer_handle;

    const u8 *archive;
    u64 archive_size;
    u64 info_count;
    u64 read_count;
    u64 last_read_offset;

    bool request_endpoint_created;
    bool reply_endpoint_created;
    bool thread_created;
    bool kernel_request_receive_cap;
    bool kernel_request_transfer_cap;
    bool kernel_reply_send_cap;
    bool kernel_reply_transfer_cap;
    bool active;
} BootArchivePortal;

bool boot_archive_portal_start(BootArchivePortal *portal, const void *archive, u64 archive_size);
bool boot_archive_portal_stop(BootArchivePortal *portal);
bool boot_archive_portal_grant_specs(const BootArchivePortal *portal,
    ProgramGrantSpec *request_send, ProgramGrantSpec *reply_receive);

bool boot_archive_portal_active(const BootArchivePortal *portal);
bool boot_archive_portal_idle(const BootArchivePortal *portal);
bool boot_archive_portal_present(const BootArchivePortal *portal);
u64 boot_archive_portal_archive_size(const BootArchivePortal *portal);
u64 boot_archive_portal_info_count(const BootArchivePortal *portal);
u64 boot_archive_portal_read_count(const BootArchivePortal *portal);
u64 boot_archive_portal_last_read_offset(const BootArchivePortal *portal);

/* Kernel-internal immutable extent validation for the generic program launcher.
 * This exposes raw bytes only; it has no tar/path/executable-name semantics. */
bool boot_archive_portal_extent(const BootArchivePortal *portal, u64 offset, u64 size,
    const u8 **out_data);

#endif
