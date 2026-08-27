#include "tar.h"
#include "vfs.h"

#define TAR_BLOCK_SIZE 512U


typedef struct __attribute__((packed)) {
    char name[100];

    char mode[8];
    char uid[8];
    char gid[8];

    char size[12];
    char mtime[12];
    char checksum[8];

    char typeflag;

    char linkname[100];

    char magic[6];
    char version[2];

    char uname[32];
    char gname[32];

    char devmajor[8];
    char devminor[8];

    char prefix[155];

    char padding[12];
} TarHeader;


_Static_assert(
    sizeof(TarHeader) == TAR_BLOCK_SIZE,
    "TarHeader must be exactly 512 bytes"
);


static bool block_is_zero(
    const u8 *block
) {
    for (u32 i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (block[i] != 0)
            return false;
    }

    return true;
}


/*
 * TAR stores most integer fields as ASCII octal.
 */
static bool parse_octal(
    const char *field,
    u32 length,
    u64 *result
) {
    if (!field || !result)
        return false;

    u64 value = 0;
    bool found_digit = false;

    u32 i = 0;

    /*
     * Skip leading spaces and NUL bytes.
     */
    while (i < length &&
           (field[i] == ' ' || field[i] == 0)) {
        ++i;
    }

    while (i < length) {
        char c = field[i];

        if (c == 0 || c == ' ')
            break;

        if (c < '0' || c > '7')
            return false;

        found_digit = true;

        value =
            (value << 3) +
            (u64)(c - '0');

        ++i;
    }

    /*
     * Empty numeric fields are treated as zero.
     */
    if (!found_digit)
        value = 0;

    *result = value;

    return true;
}


static u32 field_length(
    const char *field,
    u32 maximum
) {
    u32 length = 0;

    while (length < maximum &&
           field[length] != 0) {
        ++length;
    }

    return length;
}


static bool tar_magic_valid(
    const TarHeader *header
) {
    /*
     * USTAR normally contains:
     *
     * "ustar\0"
     *
     * We only need to test the meaningful five characters.
     */
    return
        header->magic[0] == 'u' &&
        header->magic[1] == 's' &&
        header->magic[2] == 't' &&
        header->magic[3] == 'a' &&
        header->magic[4] == 'r';
}


static bool checksum_valid(
    const TarHeader *header
) {
    u64 stored = 0;

    if (!parse_octal(
            header->checksum,
            sizeof(header->checksum),
            &stored)) {
        return false;
    }

    const u8 *bytes =
        (const u8 *)(const void *)header;

    u64 calculated = 0;

    for (u32 i = 0; i < TAR_BLOCK_SIZE; ++i) {
        /*
         * Bytes 148..155 are the checksum field.
         * TAR calculates the checksum as if those
         * eight bytes contained spaces.
         */
        if (i >= 148 && i < 156)
            calculated += (u8)' ';
        else
            calculated += bytes[i];
    }

    return stored == calculated;
}


static bool append_field(
    char *destination,
    u32 *length,
    u32 capacity,
    const char *field,
    u32 field_capacity
) {
    u32 amount =
        field_length(
            field,
            field_capacity
        );

    if (*length + amount >= capacity)
        return false;

    for (u32 i = 0; i < amount; ++i)
        destination[(*length)++] = field[i];

    destination[*length] = 0;

    return true;
}


/*
 * Convert a USTAR name/prefix pair into a VFS path.
 *
 * Examples:
 *
 * "./hello.txt"     -> "/hello.txt"
 * "./etc/motd"      -> "/etc/motd"
 * "./etc/"          -> "/etc"
 */
static bool build_path(
    const TarHeader *header,
    char path[VFS_PATH_MAX]
) {
    char temporary[VFS_PATH_MAX];

    u32 length = 0;

    temporary[0] = 0;

    u32 prefix_length =
        field_length(
            header->prefix,
            sizeof(header->prefix)
        );

    if (prefix_length) {
        if (!append_field(
                temporary,
                &length,
                sizeof(temporary),
                header->prefix,
                sizeof(header->prefix))) {
            return false;
        }

        if (length + 1 >= sizeof(temporary))
            return false;

        temporary[length++] = '/';
        temporary[length] = 0;
    }

    if (!append_field(
            temporary,
            &length,
            sizeof(temporary),
            header->name,
            sizeof(header->name))) {
        return false;
    }

    const char *source = temporary;

    /*
     * GNU tar commonly generates names beginning "./"
     * when we use:
     *
     * tar --format=ustar -cf ROOTFS.TAR -C rootfs .
     */
    while (source[0] == '.' &&
           source[1] == '/') {
        source += 2;
    }

    while (*source == '/')
        ++source;

    u32 source_length = 0;

    while (source[source_length])
        ++source_length;

    /*
     * Remove trailing slashes.
     */
    while (source_length &&
           source[source_length - 1] == '/') {
        --source_length;
    }

    /*
     * "." or "./" represents the archive root.
     */
    if (source_length == 0 ||
        (source_length == 1 &&
         source[0] == '.')) {

        path[0] = '/';
        path[1] = 0;

        return true;
    }

    if (source_length + 2 > VFS_PATH_MAX)
        return false;

    path[0] = '/';

    for (u32 i = 0; i < source_length; ++i)
        path[i + 1] = source[i];

    path[source_length + 1] = 0;

    return true;
}


bool tar_mount(
    const void *archive,
    u64 archive_size
) {
    if (!archive ||
        archive_size < TAR_BLOCK_SIZE ||
        !vfs_root()) {
        return false;
    }

    const u8 *bytes =
        (const u8 *)archive;

    u64 offset = 0;

    while (offset + TAR_BLOCK_SIZE <= archive_size) {
        const u8 *block =
            bytes + offset;

        /*
         * TAR normally ends with two zero blocks.
         * Seeing the first one is sufficient for us.
         */
        if (block_is_zero(block))
            return true;

        const TarHeader *header =
            (const TarHeader *)(const void *)block;

        if (!tar_magic_valid(header))
            return false;

        if (!checksum_valid(header))
            return false;

        u64 file_size = 0;

        if (!parse_octal(
                header->size,
                sizeof(header->size),
                &file_size)) {
            return false;
        }

        u64 data_offset =
            offset + TAR_BLOCK_SIZE;

        /*
         * Check for integer overflow.
         */
        if (data_offset < offset)
            return false;

        if (file_size >
            archive_size - data_offset) {
            return false;
        }

        char path[VFS_PATH_MAX];

        if (!build_path(header, path))
            return false;

        bool success = true;

        switch (header->typeflag) {
            /*
             * Regular file.
             */
            case 0:
            case '0':
                success =
                    vfs_add_file(
                        path,
                        bytes + data_offset,
                        file_size
                    );
                break;

            /*
             * Directory.
             */
            case '5':
                success =
                    vfs_add_directory(path);
                break;

            /*
             * We don't need symbolic links, hard links,
             * devices, etc. for our initial rootfs.
             *
             * Ignore unsupported entry types.
             */
            default:
                break;
        }

        if (!success)
            return false;

        /*
         * Every TAR entry is padded to a 512-byte boundary.
         */
        u64 padded_size =
            (file_size + TAR_BLOCK_SIZE - 1) &
            ~(u64)(TAR_BLOCK_SIZE - 1);

        if (padded_size < file_size)
            return false;

        u64 next =
            data_offset + padded_size;

        if (next < data_offset ||
            next > archive_size) {
            return false;
        }

        offset = next;
    }

    return true;
}