#ifndef JA_OS_BLOCK_H
#define JA_OS_BLOCK_H

#include "types.h"

#define BLOCK_MAX_DEVICES 16U
#define BLOCK_NAME_MAX    31U

typedef struct BlockDevice BlockDevice;

typedef enum {
    BLOCK_DEVICE_UNKNOWN = 0,
    BLOCK_DEVICE_NVME,
    BLOCK_DEVICE_AHCI,
    BLOCK_DEVICE_VIRTIO,
    BLOCK_DEVICE_PARTITION
} BlockDeviceType;

typedef bool (*BlockReadFn)(
    BlockDevice *device,
    u64 lba,
    u32 count,
    void *buffer
);

typedef bool (*BlockWriteFn)(
    BlockDevice *device,
    u64 lba,
    u32 count,
    const void *buffer
);

struct BlockDevice {
    u32 id;

    char name[BLOCK_NAME_MAX + 1];

    BlockDeviceType type;

    u32 block_size;
    u64 block_count;

    bool read_only;

    BlockReadFn read;
    BlockWriteFn write;

    void *driver_data;
};

void block_init(void);

BlockDevice *block_register(
    const char *name,
    BlockDeviceType type,
    u32 block_size,
    u64 block_count,
    bool read_only,
    BlockReadFn read,
    BlockWriteFn write,
    void *driver_data
);

u32 block_device_count(void);

BlockDevice *block_device(
    u32 index
);

BlockDevice *block_find(
    const char *name
);

bool block_read(
    BlockDevice *device,
    u64 lba,
    u32 count,
    void *buffer
);

bool block_write(
    BlockDevice *device,
    u64 lba,
    u32 count,
    const void *buffer
);

u64 block_capacity_bytes(
    const BlockDevice *device
);

#endif