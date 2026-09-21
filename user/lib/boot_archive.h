#ifndef JCOS_USER_BOOT_ARCHIVE_H
#define JCOS_USER_BOOT_ARCHIVE_H

#include "syscall.h"
#include "../../include/boot_archive_portal_abi.h"

#define JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE 512U
#define JCOS_BOOT_ARCHIVE_PATH_CAPACITY 256U

#define JCOS_BOOT_ARCHIVE_ENTRY_FILE 1U
#define JCOS_BOOT_ARCHIVE_ENTRY_DIRECTORY 2U

typedef struct {
    JcosCapabilityHandle request_cap;
    JcosCapabilityHandle reply_cap;
    JcosU64 incarnation;
    JcosU64 size;
    int ready;
} JcosBootArchive;

typedef struct {
    char path[JCOS_BOOT_ARCHIVE_PATH_CAPACITY];
    JcosU64 header_offset;
    JcosU64 data_offset;
    JcosU64 size;
    JcosU32 type;
} JcosBootArchiveEntry;

int jcos_boot_archive_init(JcosBootArchive *archive,
    JcosCapabilityHandle request_cap, JcosCapabilityHandle reply_cap,
    JcosU64 service_incarnation);
int jcos_boot_archive_probe(JcosBootArchive *archive);
int jcos_boot_archive_open(JcosBootArchive *archive,
    JcosCapabilityHandle request_cap, JcosCapabilityHandle reply_cap,
    JcosU64 service_incarnation);

int jcos_boot_archive_read(JcosBootArchive *archive, JcosU64 offset,
    void *buffer, JcosU32 count);

/* Set *cursor to zero before the first call. Returns 1 for an entry, 0 at end,
 * and -1 for malformed archive/protocol input. */
int jcos_boot_archive_next(JcosBootArchive *archive, JcosU64 *cursor,
    JcosBootArchiveEntry *entry);

int jcos_boot_archive_find(JcosBootArchive *archive, const char *path,
    JcosBootArchiveEntry *entry);

int jcos_boot_archive_read_file(JcosBootArchive *archive,
    const JcosBootArchiveEntry *entry, JcosU64 file_offset,
    void *buffer, JcosU32 count);

#endif