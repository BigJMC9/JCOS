#include "block.h"

static BlockDevice g_devices[BLOCK_MAX_DEVICES];
static u32 g_device_count;

static void zero_device(BlockDevice *device) {
    u8 *bytes = (u8 *)(void *)device;

    for (u32 i = 0; i < sizeof(BlockDevice); ++i) bytes[i] = 0;
}

static bool string_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;

        ++a;
        ++b;
    }

    return *a == *b;
}

static bool copy_name(char destination[BLOCK_NAME_MAX + 1], const char *source) {
    if (!source || !*source) return false;

    u32 length = 0;

    while (source[length]) {
        if (length >= BLOCK_NAME_MAX) return false;

        destination[length] = source[length];
        ++length;
    }

    destination[length] = 0;
    return true;
}

void block_init(void) {
    g_device_count = 0;

    for (u32 i = 0; i < BLOCK_MAX_DEVICES; ++i) zero_device(&g_devices[i]);
}

BlockDevice *block_register(const char *name, 
    BlockDeviceType type, 
    u32 block_size, 
    u64 block_count, 
    bool read_only, 
    BlockReadFn read, 
    BlockWriteFn write, 
    void *driver_data) {

    if (g_device_count >= BLOCK_MAX_DEVICES) return 0;
    if (!name || !*name || !block_size || !block_count || !read) return 0;

    /*
     * Require power-of-two logical block sizes.
     *
     * Common values are 512 and 4096.
     */
    if (block_size & (block_size - 1U)) return 0;
    if (!read_only && !write) return 0;

    /* Device names must be unique. */
    if (block_find(name)) return 0;

    BlockDevice *device = &g_devices[g_device_count];
    zero_device(device);

    if (!copy_name(device->name, name)) return 0;

    device->id = g_device_count;
    device->type = type;
    device->block_size = block_size;
    device->block_count = block_count;
    device->read_only = read_only;

    /*
     * IMPORTANT:
     *
     * These function pointers are assigned at runtime.
     * That's what is necessary for relocation-free
     * PIE kernel.
     */
    device->read = read;
    device->write = write;
    device->driver_data = driver_data;
    ++g_device_count;

    return device;
}

u32 block_device_count(void) {
    return g_device_count;
}

BlockDevice *block_device(u32 index) {
    if (index >= g_device_count) return 0;

    return &g_devices[index];
}

BlockDevice *block_find(const char *name) {
    if (!name) return 0;
    for (u32 i = 0; i < g_device_count; ++i) {
        if (string_equal(g_devices[i].name, name)) return &g_devices[i];
    }

    return 0;
}

static bool valid_range(const BlockDevice *device, u64 lba, u32 count) {
    if (!device || count == 0) return false;
    if (lba >= device->block_count) return false;

    /*
     * Written this way rather than:
     *
     *     lba + count > block_count
     *
     * because that could overflow.
     */
    if ((u64)count > device->block_count - lba) return false;

    return true;
}

bool block_read(BlockDevice *device, u64 lba, u32 count, void *buffer) {
    if (!device || !buffer || !device->read) return false;
    if (!valid_range(device, lba, count)) return false;

    return device->read(device, lba, count, buffer);
}

bool block_write(BlockDevice *device, u64 lba, u32 count, const void *buffer) {
    if (!device || !buffer || device->read_only || !device->write) return false;
    if (!valid_range(device, lba, count)) return false;

    return device->write(device, lba, count, buffer);
}

u64 block_capacity_bytes(const BlockDevice *device) {
    if (!device) return 0;

    /* Detect multiplication overflow. */
    if (device->block_count > (~0ULL / device->block_size)) return ~0ULL;

    return
        device->block_count *
        (u64)device->block_size;
}