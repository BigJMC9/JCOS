#include "gpt.h"
#include "lib.h"

#define GPT_SIGNATURE 0x5452415020494645ULL
#define GPT_MIN_HEADER_SIZE 92U

typedef struct __attribute__((packed)) {
    u64 signature;

    u32 revision;
    u32 header_size;

    u32 header_crc32;
    u32 reserved;

    u64 current_lba;
    u64 backup_lba;

    u64 first_usable_lba;
    u64 last_usable_lba;

    u8 disk_guid[16];

    u64 partition_entry_lba;

    u32 partition_entry_count;
    u32 partition_entry_size;

    u32 partition_array_crc32;
} GptHeader;

_Static_assert(sizeof(GptHeader) == 92, "GPT header prefix must be 92 bytes");

typedef struct __attribute__((packed)) {
    u8 type_guid[16];
    u8 unique_guid[16];

    u64 first_lba;
    u64 last_lba;
    u64 attributes;

    u16 name[36];
} GptEntry;

_Static_assert(sizeof(GptEntry) == 128, "GPT partition entry must be 128 bytes");

static GptInfo g_info;

static u8 g_header_sector[4096];
static u8 g_entry_buffer[
    GPT_MAX_PARTITIONS * sizeof(GptEntry)
];

static u32 crc32(const void *data, u64 size) {
    const u8 *bytes = (const u8 *)data;

    u32 crc = 0xFFFFFFFFU;

    for (u64 i = 0; i < size; ++i) {
        crc ^= bytes[i];

        for (u32 bit = 0; bit < 8U; ++bit) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else crc >>= 1;
        }
    }

    return ~crc;
}

static bool guid_is_zero(const u8 guid[16]) {
    for (u32 i = 0; i < 16U; ++i) {
        if (guid[i] != 0) return false;
    }

    return true;
}

static void copy_guid(u8 destination[16], const u8 source[16]) {
    for (u32 i = 0; i < 16U; ++i) destination[i] = source[i];
}

static void copy_partition_name(char destination[GPT_NAME_MAX + 1], const u16 source[36]) {
    u32 output = 0;

    for (u32 i = 0; i < 36U && output < GPT_NAME_MAX; ++i) {

        u16 character = source[i];

        if (character == 0) break;

        /* ASCII subset for now. Proper UTF-16 handling can come later. */
        if (character >= 32U && character <= 126U) destination[output++] = (char)character;
        else destination[output++] = '?';
    }

    destination[output] = 0;
}

bool gpt_probe(BlockDevice *device) {
    k_memset(&g_info, 0, sizeof(g_info));

    if (!device || device->block_size < 512U || device->block_size > sizeof(g_header_sector)) return false;
    if (!block_read(device, 1, 1, g_header_sector)) return false;

    GptHeader *header = (GptHeader *)(void *) g_header_sector;

    if (header->signature != GPT_SIGNATURE) return false;
    if (header->header_size < GPT_MIN_HEADER_SIZE || header->header_size > device->block_size) return false;

    /*
     * Verify header CRC.
     *
     * The CRC field itself must be treated as zero
     * while computing the checksum.
     */
    u32 expected_header_crc = header->header_crc32;
    header->header_crc32 = 0;
    u32 actual_header_crc = crc32(header, header->header_size);
    header->header_crc32 = expected_header_crc;

    if (actual_header_crc != expected_header_crc) return false;
    if (header->partition_entry_size < sizeof(GptEntry)) return false;
    if (header->partition_entry_count == 0) return false;

    /* Protect all the size arithmetic. */
    u64 entry_bytes = (u64)header->partition_entry_count * header->partition_entry_size;

    if (header->partition_entry_size && entry_bytes / header->partition_entry_size != header->partition_entry_count) return false;
    if (entry_bytes > sizeof(g_entry_buffer)) return false;

    u64 blocks = (entry_bytes + device->block_size - 1U) / device->block_size;

    /* Block API takes u32 block count. */
    if (blocks == 0 || blocks > 0xFFFFFFFFULL) return false;

    k_memset(g_entry_buffer, 0, sizeof(g_entry_buffer));

    if (!block_read(device, header->partition_entry_lba, (u32)blocks, g_entry_buffer)) return false;

    u32 actual_array_crc = crc32(g_entry_buffer, entry_bytes);

    if (actual_array_crc != header->partition_array_crc32) return false;

    g_info.device = device;
    g_info.revision = header->revision;
    g_info.header_size = header->header_size;
    g_info.current_lba = header->current_lba;
    g_info.backup_lba = header->backup_lba;
    g_info.first_usable_lba = header->first_usable_lba;
    g_info.last_usable_lba = header->last_usable_lba;
    g_info.partition_entry_lba = header->partition_entry_lba;
    g_info.partition_entry_count = header->partition_entry_count;
    g_info.partition_entry_size = header->partition_entry_size;
    u32 output = 0;

    for (u32 i = 0; i < header->partition_entry_count && output < GPT_MAX_PARTITIONS; ++i) {

        const u8 *raw = g_entry_buffer + ((u64)i * header->partition_entry_size);
        const GptEntry *entry = (const GptEntry *)(const void *)raw;

        /* Zero type GUID = unused entry. */
        if (guid_is_zero(entry->type_guid)) continue;
        if (entry->first_lba > entry->last_lba) continue;

        GptPartition *partition = &g_info.partitions[output];
        partition->valid = true; partition->index = i + 1U;

        copy_guid(partition->type_guid, entry->type_guid);
        copy_guid(partition->unique_guid, entry->unique_guid);

        partition->first_lba = entry->first_lba;
        partition->last_lba = entry->last_lba;
        partition->attributes = entry->attributes;

        copy_partition_name(partition->name, entry->name);

        ++output;
    }

    g_info.partition_count = output;
    g_info.valid = true;

    return true;
}

const GptInfo *gpt_get(void) {
    return &g_info;
}