#include "boot_archive.h"

#define TAR_NAME_OFFSET 0U
#define TAR_NAME_SIZE 100U
#define TAR_SIZE_OFFSET 124U
#define TAR_SIZE_SIZE 12U
#define TAR_CHECKSUM_OFFSET 148U
#define TAR_CHECKSUM_SIZE 8U
#define TAR_TYPE_OFFSET 156U
#define TAR_MAGIC_OFFSET 257U
#define TAR_PREFIX_OFFSET 345U
#define TAR_PREFIX_SIZE 155U

static void bytes_clear(void *buffer, JcosU32 count) {
    unsigned char *bytes = (unsigned char *)buffer;
    if (!bytes) return;
    for (JcosU32 i = 0; i < count; ++i) bytes[i] = 0U;
}

static void message_clear(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int portal_receive_current(JcosBootArchive *archive, JcosIpcMessage *reply) {
    if (!archive || !reply) return 0;
    for (JcosU32 attempt = 0; attempt < 4U; ++attempt) {
        message_clear(reply);
        if (!jcos_ipc_receive_blocking(archive->reply_cap, reply)) return 0;
        if (reply->word_count == 4U && reply->words[1] == archive->incarnation) return 1;
        /* A reply from a dead service incarnation is never replayed into the
         * replacement. Discard it and wait for this incarnation's response. */
    }
    return 0;
}

static int portal_call(JcosBootArchive *archive, const JcosIpcMessage *request,
    JcosIpcMessage *reply) {
    return archive && request && reply &&
        jcos_ipc_send_blocking(archive->request_cap, request) &&
        portal_receive_current(archive, reply);
}

int jcos_boot_archive_init(JcosBootArchive *archive,
    JcosCapabilityHandle request_cap, JcosCapabilityHandle reply_cap,
    JcosU64 service_incarnation) {
    if (!archive || !request_cap || !reply_cap || !service_incarnation) return 0;
    bytes_clear(archive, (JcosU32)sizeof(*archive));
    archive->request_cap = request_cap;
    archive->reply_cap = reply_cap;
    archive->incarnation = service_incarnation;
    return 1;
}

int jcos_boot_archive_probe(JcosBootArchive *archive) {
    if (!archive || !archive->request_cap || !archive->reply_cap || !archive->incarnation) return 0;
    if (archive->ready && archive->size) return 1;

    JcosIpcMessage request;
    JcosIpcMessage reply;
    message_clear(&request);
    message_clear(&reply);
    request.word_count = 2U;
    request.words[0] = JCOS_BOOT_ARCHIVE_PORTAL_HEADER(
        JCOS_BOOT_ARCHIVE_PORTAL_OP_INFO,
        JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION, 0U);
    request.words[1] = archive->incarnation;

    if (!portal_call(archive, &request, &reply)) return 0;
    if (JCOS_BOOT_ARCHIVE_PORTAL_HEADER_CODE(reply.words[0]) != JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INFO ||
        JCOS_BOOT_ARCHIVE_PORTAL_HEADER_VERSION(reply.words[0]) != JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION ||
        JCOS_BOOT_ARCHIVE_PORTAL_HEADER_COUNT(reply.words[0]) != 0ULL ||
        !reply.words[2] || reply.words[3] != JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES) return 0;

    archive->size = reply.words[2];
    archive->ready = 1;
    return 1;
}

int jcos_boot_archive_open(JcosBootArchive *archive,
    JcosCapabilityHandle request_cap, JcosCapabilityHandle reply_cap,
    JcosU64 service_incarnation) {
    return jcos_boot_archive_init(archive, request_cap, reply_cap, service_incarnation) &&
        jcos_boot_archive_probe(archive);
}

int jcos_boot_archive_read(JcosBootArchive *archive, JcosU64 offset,
    void *buffer, JcosU32 count) {
    if (!archive || !archive->ready || !buffer || !count ||
        offset >= archive->size || (JcosU64)count > archive->size - offset) return 0;

    unsigned char *out = (unsigned char *)buffer;
    JcosU32 done = 0U;
    while (done < count) {
        JcosU32 chunk = count - done;
        if (chunk > JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES)
            chunk = JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES;

        JcosIpcMessage request;
        JcosIpcMessage reply;
        message_clear(&request);
        message_clear(&reply);
        request.word_count = 3U;
        request.words[0] = JCOS_BOOT_ARCHIVE_PORTAL_HEADER(
            JCOS_BOOT_ARCHIVE_PORTAL_OP_READ,
            JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION, chunk);
        request.words[1] = archive->incarnation;
        request.words[2] = offset + done;

        if (!portal_call(archive, &request, &reply)) return 0;
        if (JCOS_BOOT_ARCHIVE_PORTAL_HEADER_CODE(reply.words[0]) != JCOS_BOOT_ARCHIVE_PORTAL_REPLY_DATA ||
            JCOS_BOOT_ARCHIVE_PORTAL_HEADER_VERSION(reply.words[0]) != JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION ||
            JCOS_BOOT_ARCHIVE_PORTAL_HEADER_COUNT(reply.words[0]) != chunk ||
            reply.words[2] != offset + done) return 0;

        JcosU64 packed = reply.words[3];
        for (JcosU32 i = 0; i < chunk; ++i)
            out[done + i] = (unsigned char)((packed >> (i * 8U)) & 0xFFULL);
        done += chunk;
    }
    return 1;
}

static int block_zero(const unsigned char *block) {
    for (JcosU32 i = 0; i < JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE; ++i)
        if (block[i]) return 0;
    return 1;
}

static int octal_field(const unsigned char *field, JcosU32 width, JcosU64 *out) {
    if (!field || !width || !out) return 0;
    JcosU64 value = 0ULL;
    JcosU32 i = 0U;
    while (i < width && (field[i] == ' ' || field[i] == 0U)) ++i;
    int saw_digit = 0;
    for (; i < width; ++i) {
        unsigned char c = field[i];
        if (!c || c == ' ') break;
        if (c < '0' || c > '7') return 0;
        JcosU64 digit = (JcosU64)(c - '0');
        if (value > (~0ULL - digit) / 8ULL) return 0;
        value = value * 8ULL + digit;
        saw_digit = 1;
    }
    *out = value;
    return saw_digit || value == 0ULL;
}

static int header_checksum_valid(const unsigned char *block) {
    JcosU64 expected = 0ULL;
    if (!octal_field(block + TAR_CHECKSUM_OFFSET, TAR_CHECKSUM_SIZE, &expected)) return 0;
    JcosU64 sum = 0ULL;
    for (JcosU32 i = 0; i < JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE; ++i) {
        if (i >= TAR_CHECKSUM_OFFSET && i < TAR_CHECKSUM_OFFSET + TAR_CHECKSUM_SIZE) sum += 0x20U;
        else sum += block[i];
    }
    return sum == expected;
}

static int tar_magic_valid(const unsigned char *block) {
    static const char magic[] = "ustar";
    for (JcosU32 i = 0; i < 5U; ++i)
        if (block[TAR_MAGIC_OFFSET + i] != (unsigned char)magic[i]) return 0;
    return 1;
}

static int path_component_parent(const char *path, JcosU32 start, JcosU32 end) {
    return end - start == 2U && path[start] == '.' && path[start + 1U] == '.';
}

static int normalize_path(const char *input, char *output, JcosU32 capacity) {
    if (!input || !output || capacity < 2U) return 0;
    JcosU32 in = 0U;
    while (input[in] == '/') ++in;
    while (input[in] == '.' && input[in + 1U] == '/') in += 2U;

    JcosU32 out = 0U;
    JcosU32 component = 0U;
    while (input[in]) {
        char c = input[in++];
        if (c == '/') {
            if (out && output[out - 1U] == '/') continue;
            if (path_component_parent(output, component, out)) return 0;
            if (out + 1U >= capacity) return 0;
            output[out++] = '/';
            component = out;
            continue;
        }
        if ((unsigned char)c < 32U || (unsigned char)c > 126U) return 0;
        if (out + 1U >= capacity) return 0;
        output[out++] = c;
    }

    if (path_component_parent(output, component, out)) return 0;
    while (out && output[out - 1U] == '/') --out;
    output[out] = 0;
    return out != 0U;
}

static int field_append(char *dst, JcosU32 *length, JcosU32 capacity,
    const unsigned char *src, JcosU32 width) {
    if (!dst || !length || !src) return 0;
    for (JcosU32 i = 0; i < width && src[i]; ++i) {
        if (*length + 1U >= capacity) return 0;
        dst[(*length)++] = (char)src[i];
    }
    dst[*length] = 0;
    return 1;
}

static int entry_from_header(const unsigned char *block, JcosU64 header_offset,
    JcosU64 archive_size, JcosBootArchiveEntry *entry, JcosU64 *next_offset) {
    if (!block || !entry || !next_offset || !tar_magic_valid(block) ||
        !header_checksum_valid(block)) return 0;

    JcosU64 size = 0ULL;
    if (!octal_field(block + TAR_SIZE_OFFSET, TAR_SIZE_SIZE, &size)) return 0;
    if (header_offset > archive_size || archive_size - header_offset < JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE)
        return 0;
    JcosU64 data_offset = header_offset + JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE;
    if (size > archive_size - data_offset ||
        size > ~0ULL - (JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE - 1ULL)) return 0;
    JcosU64 padded = (size + (JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE - 1ULL)) &
        ~(JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE - 1ULL);
    if (padded < size || padded > archive_size - data_offset) return 0;

    char raw[JCOS_BOOT_ARCHIVE_PATH_CAPACITY];
    bytes_clear(raw, (JcosU32)sizeof(raw));
    JcosU32 length = 0U;
    if (block[TAR_PREFIX_OFFSET]) {
        if (!field_append(raw, &length, sizeof(raw), block + TAR_PREFIX_OFFSET, TAR_PREFIX_SIZE)) return 0;
        if (length + 1U >= sizeof(raw)) return 0;
        raw[length++] = '/';
        raw[length] = 0;
    }
    if (!field_append(raw, &length, sizeof(raw), block + TAR_NAME_OFFSET, TAR_NAME_SIZE)) return 0;

    char canonical[JCOS_BOOT_ARCHIVE_PATH_CAPACITY];
    bytes_clear(canonical, (JcosU32)sizeof(canonical));
    int named = normalize_path(raw, canonical, sizeof(canonical));

    bytes_clear(entry, (JcosU32)sizeof(*entry));
    if (named) {
        JcosU32 i = 0U;
        while (canonical[i] && i + 1U < sizeof(entry->path)) {
            entry->path[i] = canonical[i];
            ++i;
        }
        entry->path[i] = 0;
    }
    entry->header_offset = header_offset;
    entry->data_offset = data_offset;
    entry->size = size;
    unsigned char type = block[TAR_TYPE_OFFSET];
    entry->type = type == '5' ? JCOS_BOOT_ARCHIVE_ENTRY_DIRECTORY :
        ((type == 0U || type == '0') ? JCOS_BOOT_ARCHIVE_ENTRY_FILE : 0U);
    *next_offset = data_offset + padded;
    return 1;
}

int jcos_boot_archive_next(JcosBootArchive *archive, JcosU64 *cursor,
    JcosBootArchiveEntry *entry) {
    if (!archive || !archive->ready || !cursor || !entry ||
        (*cursor & (JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE - 1ULL))) return -1;

    JcosU64 offset = *cursor;
    unsigned char block[JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE];
    while (offset + JCOS_BOOT_ARCHIVE_TAR_BLOCK_SIZE <= archive->size) {
        if (!jcos_boot_archive_read(archive, offset, block, sizeof(block))) return -1;
        if (block_zero(block)) {
            *cursor = archive->size;
            return 0;
        }

        JcosU64 next = 0ULL;
        if (!entry_from_header(block, offset, archive->size, entry, &next) || next <= offset) return -1;
        *cursor = next;
        offset = next;
        if (entry->path[0] && entry->type) return 1;
        if (offset >= archive->size) return 0;
    }
    return 0;
}

static void entry_copy(JcosBootArchiveEntry *dst, const JcosBootArchiveEntry *src) {
    if (!dst || !src) return;
    for (JcosU32 i = 0; i < JCOS_BOOT_ARCHIVE_PATH_CAPACITY; ++i) dst->path[i] = src->path[i];
    dst->header_offset = src->header_offset;
    dst->data_offset = src->data_offset;
    dst->size = src->size;
    dst->type = src->type;
}

static int text_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

int jcos_boot_archive_find(JcosBootArchive *archive, const char *path,
    JcosBootArchiveEntry *entry) {
    if (!archive || !archive->ready || !path || !entry) return 0;
    char canonical[JCOS_BOOT_ARCHIVE_PATH_CAPACITY];
    bytes_clear(canonical, (JcosU32)sizeof(canonical));
    if (!normalize_path(path, canonical, sizeof(canonical))) return 0;

    JcosU64 cursor = 0ULL;
    JcosBootArchiveEntry candidate;
    for (;;) {
        int result = jcos_boot_archive_next(archive, &cursor, &candidate);
        if (result <= 0) return 0;
        if (text_equal(candidate.path, canonical)) {
            entry_copy(entry, &candidate);
            return 1;
        }
    }
}

int jcos_boot_archive_read_file(JcosBootArchive *archive,
    const JcosBootArchiveEntry *entry, JcosU64 file_offset,
    void *buffer, JcosU32 count) {
    if (!archive || !archive->ready || !entry || entry->type != JCOS_BOOT_ARCHIVE_ENTRY_FILE ||
        !buffer || !count || file_offset >= entry->size ||
        (JcosU64)count > entry->size - file_offset) return 0;
    if (entry->data_offset > archive->size || file_offset > archive->size - entry->data_offset) return 0;
    return jcos_boot_archive_read(archive, entry->data_offset + file_offset, buffer, count);
}