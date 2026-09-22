#ifndef JA_OS_GPT_H
#define JA_OS_GPT_H

#include "types.h"
#include "block.h"

#define GPT_MAX_PARTITIONS 128U
#define GPT_NAME_MAX       36U

typedef struct {
    bool valid;

    u32 index;

    u8 type_guid[16];
    u8 unique_guid[16];

    u64 first_lba;
    u64 last_lba;
    u64 attributes;

    char name[GPT_NAME_MAX + 1];

    BlockDevice *block_device;
} GptPartition;

typedef struct {
    bool valid;

    BlockDevice *device;

    u32 revision;
    u32 header_size;

    u64 current_lba;
    u64 backup_lba;

    u64 first_usable_lba;
    u64 last_usable_lba;

    u64 partition_entry_lba;
    u32 partition_entry_count;
    u32 partition_entry_size;

    u32 partition_count;

    GptPartition partitions[GPT_MAX_PARTITIONS];
} GptInfo;

bool gpt_probe(
    BlockDevice *device
);

const GptInfo *gpt_get(void);

bool gpt_register_partitions(void);

/* Return the registered EFI System Partition for the active GPT, if any. */
BlockDevice *gpt_efi_system_partition(void);

#endif