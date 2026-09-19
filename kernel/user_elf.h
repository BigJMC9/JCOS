#ifndef JA_OS_USER_ELF_H
#define JA_OS_USER_ELF_H

#include "types.h"
#include "pmm.h"
#include "process.h"
#include "vfs.h"

#define USER_ELF_MAX_LOAD_PAGES 64U

typedef struct {
    u64 virtual_address;
    frame_t frame;
    bool mapped;
} UserElfPage;

typedef struct UserElfImage {
    u64 entry;
    UserElfPage pages[USER_ELF_MAX_LOAD_PAGES];
    u32 page_count;
    bool loaded;
    bool cleanup_pending;
    Process *owner;
    u64 owner_process_id;
    u64 owner_space_id;
} UserElfImage;

/*
 * Zero-initialize before first use. Keep Process and image storage alive and
 * stationary until needs_cleanup() is false. Never copy a live ledger.
 * One ELF per Process; load before publishing any thread execution context.
 *
 * false from load does NOT promise rollback completed: inspect needs_cleanup.
 * Unload is retryable, but may make partial progress; loaded/entry are revoked
 * when cleanup starts. Reap all owner threads before calling public unload.
 * These operations exclude local interrupts; they are not SMP/NMI-safe.
 */
bool user_elf_load(Process *process, const VfsNode *file, UserElfImage *image);
bool user_elf_unload(Process *process, UserElfImage *image);
bool user_elf_needs_cleanup(const UserElfImage *image);

#endif