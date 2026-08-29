#ifndef JA_OS_FAT32_H
#define JA_OS_FAT32_H

#include "types.h"
#include "block.h"

#define FAT32_NAME_MAX 12U

#define FAT32_ATTR_READ_ONLY 0x01U
#define FAT32_ATTR_HIDDEN    0x02U
#define FAT32_ATTR_SYSTEM    0x04U
#define FAT32_ATTR_VOLUME_ID 0x08U
#define FAT32_ATTR_DIRECTORY 0x10U
#define FAT32_ATTR_ARCHIVE   0x20U
#define FAT32_ATTR_LFN       0x0FU

typedef struct {
    char name[FAT32_NAME_MAX + 1];

    u8 attributes;

    u32 first_cluster;
    u32 size;
} Fat32DirectoryEntry;

typedef struct {
    bool valid;

    BlockDevice *device;

    u32 bytes_per_sector;
    u32 sectors_per_cluster;

    u32 reserved_sectors;
    u32 fat_count;
    u32 sectors_per_fat;

    u32 total_sectors;

    u32 root_cluster;

    u32 fsinfo_sector;
    u32 backup_boot_sector;

    /* Relative to the partition device. */
    u64 first_fat_sector;
    u64 first_data_sector;

    u32 cluster_count;
} Fat32Info;

bool fat32_probe(
    BlockDevice *device
);

const Fat32Info *fat32_get(void);

bool fat32_read_root(
    Fat32DirectoryEntry *entries,
    u32 capacity,
    u32 *entry_count
);

bool fat32_find_root(
    const char *name,
    Fat32DirectoryEntry *entry
);

bool fat32_read_file(
    const Fat32DirectoryEntry *entry,
    u64 offset,
    void *buffer,
    u64 size,
    u64 *bytes_read
);

#endif