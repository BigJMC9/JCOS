#include "partition.h"

#define PARTITION_MAX_DEVICES 128U

typedef struct {
    bool used;

    BlockDevice *parent;

    u64 first_lba;
    u64 block_count;

    BlockDevice *device;
} PartitionState;

static PartitionState g_partitions[PARTITION_MAX_DEVICES];
static u32 g_partition_count;

void partition_init(void) {
    g_partition_count = 0;

    for (u32 i = 0; i < PARTITION_MAX_DEVICES; ++i) {
        g_partitions[i].used = false;
        g_partitions[i].parent = 0;
        g_partitions[i].first_lba = 0;
        g_partitions[i].block_count = 0;
        g_partitions[i].device = 0;
    }
}

static bool partition_read(BlockDevice *device, u64 lba, u32 count, void *buffer) {
    if (!device || !device->driver_data || !buffer || count == 0) return false;

    PartitionState *state = (PartitionState *)device->driver_data;

    if (!state->parent) return false;
    if (lba >= state->block_count) return false;
    if ((u64)count > state->block_count - lba) return false;

    u64 parent_lba = state->first_lba + lba;

    if (parent_lba < state->first_lba) return false;

    return block_read(state->parent, parent_lba, count, buffer);
}

static bool partition_write(BlockDevice *device, u64 lba, u32 count, const void *buffer) {
    if (!device || !device->driver_data || !buffer || count == 0) return false;

    PartitionState *state = (PartitionState *)device->driver_data;

    if (!state->parent || state->parent->read_only) return false;
    if (lba >= state->block_count) return false;
    if ((u64)count > state->block_count - lba) return false;

    u64 parent_lba = state->first_lba + lba;

    if (parent_lba < state->first_lba) return false;

    return block_write(state->parent, parent_lba, count, buffer);
}

BlockDevice *partition_register(BlockDevice *parent, const char *name, u64 first_lba, u64 block_count) {
    if (!parent || !name || !*name || !block_count) return 0;
    if (g_partition_count >= PARTITION_MAX_DEVICES) return 0;
    if (first_lba >= parent->block_count) return 0;
    if (block_count > parent->block_count - first_lba) return 0;

    PartitionState *state = &g_partitions[g_partition_count];

    state->used = true;
    state->parent = parent;
    state->first_lba = first_lba;
    state->block_count = block_count;

    /* A partition inherits read-only state from its underlying disk. */
    BlockDevice *device = block_register(name, BLOCK_DEVICE_PARTITION, parent->block_size, block_count, parent->read_only, partition_read, partition_write, state);

    if (!device) {
        state->used = false;
        return 0;
    }

    state->device = device;

    ++g_partition_count;

    return device;
}