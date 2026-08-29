#ifndef JA_OS_PARTITION_H
#define JA_OS_PARTITION_H

#include "types.h"
#include "block.h"

BlockDevice *partition_register(
    BlockDevice *parent,
    const char *name,
    u64 first_lba,
    u64 block_count
);

void partition_init(void);

#endif