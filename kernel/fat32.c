#include "fat32.h"
#include "lib.h"

#define FAT32_MIN_CLUSTERS 65525U

#define FAT32_CLUSTER_FREE 0x00000000U
#define FAT32_CLUSTER_BAD  0x0FFFFFF7U
#define FAT32_CLUSTER_EOC  0x0FFFFFF8U

#define FAT32_MAX_SECTOR_SIZE 4096U

static Fat32Info g_info;

/*
 * Shared scratch buffer.
 *
 * Single-threaded and FAT32 access is synchronous
 * right now, so one sector buffer is enough.
 */
static u8 g_sector[FAT32_MAX_SECTOR_SIZE];

static u16 read_le16(const u8 *data) {
    return
        (u16)data[0] |
        ((u16)data[1] << 8);
}

static u32 read_le32(const u8 *data) {
    return
        (u32)data[0] |
        ((u32)data[1] << 8) |
        ((u32)data[2] << 16) |
        ((u32)data[3] << 24);
}

static bool power_of_two(u32 value) {
    return
        value != 0 &&
        (value & (value - 1U)) == 0;
}

static bool sector_size_valid(u32 size) {
    return
        size == 512U ||
        size == 1024U ||
        size == 2048U ||
        size == 4096U;
}

static bool cluster_to_lba(u32 cluster, u64 *lba) {
    if (!g_info.valid || !lba) return false;
    if (cluster < 2U) return false;

    u32 relative_cluster = cluster - 2U;

    if (relative_cluster >= g_info.cluster_count) return false;
    if (relative_cluster > (~0ULL / g_info.sectors_per_cluster)) return false;

    u64 offset = (u64)relative_cluster * g_info.sectors_per_cluster;

    if (g_info.first_data_sector > ~0ULL - offset) return false;

    u64 result = g_info.first_data_sector + offset;

    if (result >= g_info.device->block_count) return false;

    *lba = result;

    return true;
}

static bool fat32_next_cluster(u32 cluster, u32 *next) {
    if (!g_info.valid || !g_info.device || !next) return false;
    if (cluster < 2U) return false;

    /* FAT32 entries are four bytes each. */
    u64 fat_offset = (u64)cluster * 4ULL;
    u64 fat_sector_offset = fat_offset / g_info.bytes_per_sector;
    u32 entry_offset = (u32)(fat_offset % g_info.bytes_per_sector);

    /* A valid FAT sector size is divisible by four, FAT32 entry cannot straddle sectors. */
    if (entry_offset + 4U > g_info.bytes_per_sector) return false;
    if (g_info.first_fat_sector > ~0ULL - fat_sector_offset) return false;

    u64 lba = g_info.first_fat_sector + fat_sector_offset;

    if (lba >= g_info.device->block_count) return false;
    if (!block_read(g_info.device, lba, 1, g_sector)) return false;

    /* FAT32 only uses the low 28 bits. */
    *next = read_le32(g_sector + entry_offset) & 0x0FFFFFFFU;

    return true;
}

static bool fat32_is_eoc(u32 cluster) {
    return cluster >= FAT32_CLUSTER_EOC;
}

static void fat32_short_name(const u8 raw[11], char output[FAT32_NAME_MAX + 1]) {
    u32 position = 0;

    /* Base name: bytes 0..7. */
    for (u32 i = 0; i < 8U; ++i) {
        u8 c = raw[i];

        if (c == ' ') break;

        /* 0x05 represents an actual first-byte 0xE5. Extended FAT names not impl yet, but handle the special value correctly. */
        if (i == 0 && c == 0x05U) c = 0xE5U;
        if (position < FAT32_NAME_MAX) output[position++] = (char)c;
    }

    bool extension = false;

    for (u32 i = 8U; i < 11U; ++i) {
        if (raw[i] != ' ') {
            extension = true;
            break;
        }
    }

    if (extension && position < FAT32_NAME_MAX) {

        output[position++] = '.';

        for (u32 i = 8U; i < 11U; ++i) {
            u8 c = raw[i];

            if (c == ' ') break;
            if (position < FAT32_NAME_MAX) output[position++] = (char)c;
        }
    }

    output[position] = 0;
}

bool fat32_read_root(Fat32DirectoryEntry *entries, u32 capacity, u32 *entry_count) {
    if (!g_info.valid || !entries || !entry_count || capacity == 0) return false;

    *entry_count = 0;

    u32 cluster = g_info.root_cluster;

    /* This also acts as a cycle/corruption guard. A valid chain cannot contain more clusters than the entire filesystem. */
    for (u32 visited = 0; visited <= g_info.cluster_count; ++visited) {

        u64 cluster_lba = 0;

        if (!cluster_to_lba(cluster, &cluster_lba)) return false;
        for (u32 sector = 0; sector < g_info.sectors_per_cluster; ++sector) {
            if (cluster_lba > ~0ULL - sector) return false;

            u64 lba = cluster_lba + sector;

            if (!block_read(g_info.device, lba, 1, g_sector)) return false;

            /* One FAT directory entry is 32 bytes. */
            u32 entries_per_sector = g_info.bytes_per_sector / 32U;

            for (u32 i = 0; i < entries_per_sector; ++i) {

                const u8 *raw = g_sector + i * 32U;

                /* 0x00: no more entries in this directory. */
                if (raw[0] == 0x00U) return true;

                /* 0xE5: deleted entry. */
                if (raw[0] == 0xE5U) continue;

                u8 attributes = raw[11];

                /* Skip long filename records for now. */
                if ((attributes & 0x0FU) == FAT32_ATTR_LFN) continue;

                /* Volume labels != normal files. */
                if (attributes & FAT32_ATTR_VOLUME_ID) continue;
                if (*entry_count >= capacity) return false;

                Fat32DirectoryEntry *entry = &entries[*entry_count];

                k_memset(entry, 0, sizeof(*entry));

                fat32_short_name(raw, entry->name);

                entry->attributes = attributes;

                u32 high = read_le16(raw + 20);

                u32 low = read_le16(raw + 26);

                entry->first_cluster = (high << 16) | low;
                entry->size = read_le32(raw + 28);

                ++(*entry_count);
            }
        }

        u32 next = 0;

        if (!fat32_next_cluster(cluster, &next)) return false;
        if (fat32_is_eoc(next)) return true;
        if (next == FAT32_CLUSTER_FREE || next == FAT32_CLUSTER_BAD || next < 2U) return false;

        cluster = next;
    }

    /* Visited more clusters than physically exist, chain must contain a cycle/corruption. */
    return false;
}

bool fat32_find_root(const char *name, Fat32DirectoryEntry *entry) {
    if (!g_info.valid || !name || !*name || !entry) return false;

    /*
     * Enough for the current ESP root.
     *
     * Will replace with a generic
     * directory iterator.
     */
    static Fat32DirectoryEntry entries[64];

    u32 count = 0;

    if (!fat32_read_root(entries, ARRAY_COUNT(entries), &count)) return false;
    for (u32 i = 0; i < count; ++i) {
        if (k_strieq(entries[i].name, name)) {

            *entry = entries[i];

            return true;
        }
    }

    return false;
}

bool fat32_read_file(const Fat32DirectoryEntry *entry, u64 offset, void *buffer, u64 size, u64 *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!g_info.valid || !g_info.device || !entry) return false;

    /* API for regular files. */
    if (entry->attributes & FAT32_ATTR_DIRECTORY) return false;

    /* Reading zero bytes is valid. */
    if (size == 0) return true;
    if (!buffer) return false;

    /* Reading beyond EOF returns zero bytes, like a normal filesystem read. */
    if (offset >= entry->size) return true;

    /* Empty files are allowed to have cluster zero. */
    if (entry->size == 0) return true;
    if (entry->first_cluster < 2U) return false;

    u64 cluster_bytes = (u64)g_info.bytes_per_sector * g_info.sectors_per_cluster;

    if (cluster_bytes == 0) return false;

    /* Clamp the request to EOF. */
    u64 available = (u64)entry->size - offset;
    u64 remaining = size < available ? size : available;
    u64 clusters_to_skip = offset / cluster_bytes;
    u64 offset_in_cluster = offset % cluster_bytes;

    u32 cluster = entry->first_cluster;
    u32 visited = 0;

    /* Follow the FAT chain until the cluster containing 'offset'. */
    while (clusters_to_skip) {
        if (++visited > g_info.cluster_count) return false;

        u32 next = 0;

        if (!fat32_next_cluster(cluster, &next)) return false;

        /* EOF before reaching the requested file offset means corrupt metadata. */
        if (fat32_is_eoc(next)) return false;
        if (next == FAT32_CLUSTER_FREE || next == FAT32_CLUSTER_BAD || next < 2U) return false;

        cluster = next;

        --clusters_to_skip;
    }

    u8 *output = (u8 *)buffer;
    u64 total_read = 0;

    /* Read clusters until the request is satisfied or EOF is reached. */
    while (remaining) {
        if (++visited > g_info.cluster_count) return false;

        u64 cluster_lba = 0;

        if (!cluster_to_lba(cluster, &cluster_lba)) return false;

        /* A cluster may contain several sectors. */
        for (u32 sector = 0; sector < g_info.sectors_per_cluster && remaining; ++sector) {

            u64 sector_start = (u64)sector * g_info.bytes_per_sector;

            u64 sector_end = sector_start + g_info.bytes_per_sector;

            /* The requested offset may start in a later sector of this cluster. */
            if (offset_in_cluster >= sector_end) continue;
            if (cluster_lba > ~0ULL - sector) return false;

            u64 lba = cluster_lba + sector;

            if (!block_read(g_info.device, lba, 1, g_sector)) return false;

            u32 start = 0;

            if (offset_in_cluster > sector_start) start = (u32)(offset_in_cluster - sector_start);

            u64 sector_available = g_info.bytes_per_sector - start;
            u64 amount = remaining < sector_available ? remaining : sector_available;

            k_memcpy(output + total_read, g_sector + start, (usize)amount);

            total_read += amount;
            remaining -= amount;

            /* After first piece has been consumed, subsequent sectors + clusters begin at offset zero. */
            offset_in_cluster = 0;
        }

        if (!remaining) break;

        /* Need another cluster. Follow the FAT; never assume cluster + 1. */
        u32 next = 0;

        if (!fat32_next_cluster(cluster, &next)) return false;

        /* Directory entry says more bytes exist, but FAT chain ended early. */
        if (fat32_is_eoc(next)) return false;
        if (next == FAT32_CLUSTER_FREE || next == FAT32_CLUSTER_BAD || next < 2U) return false;

        cluster = next;
    }

    if (bytes_read) *bytes_read = total_read;

    return true;
}

bool fat32_probe(BlockDevice *device) {
    k_memset(&g_info, 0, sizeof(g_info));

    if (!device) return false;
    if (!sector_size_valid(device->block_size)) return false;
    if (device->block_size > sizeof(g_sector)) return false;
    if (!block_read(device, 0, 1, g_sector)) return false;

    /* FAT/PC boot-sector signature. */
    if (g_sector[510] != 0x55U || g_sector[511] != 0xAAU) return false;

    u32 bytes_per_sector = read_le16(g_sector + 11);
    u32 sectors_per_cluster = g_sector[13];
    u32 reserved_sectors = read_le16(g_sector + 14);
    u32 fat_count = g_sector[16];
    u32 root_entry_count = read_le16(g_sector + 17);
    u32 total16 = read_le16(g_sector + 19);
    u32 fat16 = read_le16(g_sector + 22);
    u32 total32 = read_le32(g_sector + 32);
    u32 fat32 = read_le32(g_sector + 36);
    u32 root_cluster = read_le32(g_sector + 44);
    u32 fsinfo_sector = read_le16(g_sector + 48);
    u32 backup_boot_sector = read_le16(g_sector + 50);

    if (!sector_size_valid(bytes_per_sector)) return false;

    /* For now one filesystem sector == one BlockDevice logical block. */
    if (bytes_per_sector != device->block_size) return false;
    if (!power_of_two(sectors_per_cluster)) return false;

    /* FAT allows powers of two up through 128 sectors per cluster. */
    if (sectors_per_cluster > 128U) return false;
    if (reserved_sectors == 0) return false;
    if (fat_count == 0 || fat_count > 2U) return false;

    /* FAT32 has no fixed FAT12/16 root directory. */
    if (root_entry_count != 0) return false;

    /* FAT32 uses BPB_FATSz32. */
    if (fat16 != 0 || fat32 == 0) return false;

    u32 total_sectors = total16 ? total16 : total32;

    if (total_sectors == 0) return false;
    if ((u64)total_sectors > device->block_count) return false;

    u64 fats_size = (u64)fat_count * fat32;

    if (fats_size > ~0ULL - reserved_sectors) return false;

    u64 first_data_sector = (u64)reserved_sectors + fats_size;

    if (first_data_sector >= total_sectors) return false;

    u64 data_sectors = (u64)total_sectors - first_data_sector;
    u64 cluster_count64 = data_sectors / sectors_per_cluster;

    /* Standard FAT type classification. */
    if (cluster_count64 < FAT32_MIN_CLUSTERS) return false;
    if (cluster_count64 > 0x0FFFFFF5ULL) return false;
    if (root_cluster < 2U) return false;
    if ((u64)(root_cluster - 2U) >= cluster_count64) return false;

    /* Image identifies FAT32. This field isn't the sole authority for detecting FAT, but it is a useful sanity check for JCOS's image. */
    if (g_sector[82] != 'F' || g_sector[83] != 'A' || g_sector[84] != 'T' || g_sector[85] != '3' || g_sector[86] != '2') return false;

    g_info.device = device;
    g_info.bytes_per_sector = bytes_per_sector;
    g_info.sectors_per_cluster = sectors_per_cluster;
    g_info.reserved_sectors = reserved_sectors;
    g_info.fat_count = fat_count;
    g_info.sectors_per_fat = fat32;
    g_info.total_sectors = total_sectors;
    g_info.root_cluster = root_cluster;
    g_info.fsinfo_sector = fsinfo_sector;
    g_info.backup_boot_sector = backup_boot_sector;
    g_info.first_fat_sector = reserved_sectors;
    g_info.first_data_sector = first_data_sector;
    g_info.cluster_count = (u32)cluster_count64;
    g_info.valid = true;

    return true;
}

const Fat32Info *fat32_get(void) {
    return &g_info;
}