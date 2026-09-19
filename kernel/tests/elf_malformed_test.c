#include "elf_malformed_test.h"
#include "user_test_fixture.h"
#include "user_elf_test.h"

#include "address_space.h"
#include "lib.h"
#include "pmm.h"
#include "terminal.h"
#include "test_output.h"
#include "user_stack.h"
#include "vfs.h"
#include "vmm.h"

#define ELF64_CLASS_64        2U
#define ELF64_DATA_LITTLE     1U
#define ELF64_VERSION         1U
#define ELF64_TYPE_EXEC       2U
#define ELF64_MACHINE_X86_64 62U

#define ELF64_PT_LOAD    1U
#define ELF64_PT_DYNAMIC 2U
#define ELF64_PT_INTERP  3U
#define ELF64_PT_TLS     7U

#define ELF64_PF_X 1U
#define ELF64_PF_W 2U
#define ELF64_PF_R 4U

#define TEST_FILE_CAPACITY 1024U
#define TEST_EXEC_OFFSET   0x200ULL
#define TEST_DATA_OFFSET   0x300ULL
#define TEST_TEXT          (ADDRESS_SPACE_USER_BASE + 0x200000ULL)
#define TEST_DATA          (ADDRESS_SPACE_USER_BASE + 0x202000ULL)
#define TEST_COLLISION     (ADDRESS_SPACE_USER_BASE + 0x300000ULL)

#define TEST_PHDR_LIMIT 16U

typedef struct __attribute__((packed)) {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 program_header_offset;
    u64 section_header_offset;
    u32 flags;
    u16 header_size;
    u16 program_header_size;
    u16 program_header_count;
    u16 section_header_size;
    u16 section_header_count;
    u16 section_name_index;
} TestElf64Header;

typedef struct __attribute__((packed)) {
    u32 type;
    u32 flags;
    u64 offset;
    u64 virtual_address;
    u64 physical_address;
    u64 file_size;
    u64 memory_size;
    u64 alignment;
} TestElf64ProgramHeader;

_Static_assert(sizeof(TestElf64Header) == 64, "test ELF header size");
_Static_assert(sizeof(TestElf64ProgramHeader) == 56, "test ELF program-header size");

typedef enum {
    BAD_MAGIC = 0,
    BAD_ELF_FLAGS,
    TOO_MANY_PROGRAM_HEADERS,
    PROGRAM_HEADER_TABLE_BOUNDS,
    DYNAMIC_RUNTIME,
    INTERPRETER_RUNTIME,
    TLS_RUNTIME,
    UNKNOWN_LOAD_FLAGS,
    WRITABLE_EXECUTABLE,
    FILE_LARGER_THAN_MEMORY,
    FILE_RANGE_OUTSIDE_SOURCE,
    NON_POWER_OF_TWO_ALIGNMENT,
    ALIGNMENT_CONGRUENCE,
    USER_RANGE_OVERFLOW,
    PAGE_LEVEL_LOAD_OVERLAP,
    INITIAL_STACK_RESERVATION,
    ENTRY_IN_EXECUTABLE_BSS,
    ENTRY_IN_NONEXECUTABLE_DATA,
    NO_LOAD_SEGMENTS,
    EXISTING_MAPPING_COLLISION
} MalformedCase;

static u8 g_bytes[TEST_FILE_CAPACITY];
static VfsNode g_file;

static TestElf64Header *test_header(void) {
    return (TestElf64Header *)(void *)g_bytes;
}

static TestElf64ProgramHeader *test_program_header(u32 index) {
    return (TestElf64ProgramHeader *)(void *)(g_bytes + sizeof(TestElf64Header) +
        (u64)index * sizeof(TestElf64ProgramHeader));
}

static void build_valid_elf(void) {
    k_memset(g_bytes, 0, sizeof(g_bytes));
    k_memset(&g_file, 0, sizeof(g_file));

    TestElf64Header *header = test_header();
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = ELF64_CLASS_64;
    header->ident[5] = ELF64_DATA_LITTLE;
    header->ident[6] = ELF64_VERSION;
    header->type = ELF64_TYPE_EXEC;
    header->machine = ELF64_MACHINE_X86_64;
    header->version = ELF64_VERSION;
    header->entry = TEST_TEXT;
    header->program_header_offset = sizeof(TestElf64Header);
    header->header_size = sizeof(TestElf64Header);
    header->program_header_size = sizeof(TestElf64ProgramHeader);
    header->program_header_count = 2U;

    TestElf64ProgramHeader *text = test_program_header(0U);
    text->type = ELF64_PT_LOAD;
    text->flags = ELF64_PF_R | ELF64_PF_X;
    text->offset = TEST_EXEC_OFFSET;
    text->virtual_address = TEST_TEXT;
    text->file_size = 16ULL;
    text->memory_size = VM_PAGE_SIZE;
    text->alignment = 1ULL;

    TestElf64ProgramHeader *data = test_program_header(1U);
    data->type = ELF64_PT_LOAD;
    data->flags = ELF64_PF_R | ELF64_PF_W;
    data->offset = TEST_DATA_OFFSET;
    data->virtual_address = TEST_DATA;
    data->file_size = 8ULL;
    data->memory_size = VM_PAGE_SIZE;
    data->alignment = 1ULL;

    for (u32 i = 0; i < 16U; ++i) g_bytes[TEST_EXEC_OFFSET + i] = (u8)(0x90U + (i & 0xFU));
    for (u32 i = 0; i < 8U; ++i) g_bytes[TEST_DATA_OFFSET + i] = (u8)(0x40U + i);

    g_file.type = VFS_FILE;
    g_file.data = g_bytes;
    g_file.size = sizeof(g_bytes);
}

static void mutate_elf(MalformedCase test_case) {
    TestElf64Header *header = test_header();
    TestElf64ProgramHeader *text = test_program_header(0U);
    TestElf64ProgramHeader *data = test_program_header(1U);

    switch (test_case) {
        case BAD_MAGIC:
            header->ident[0] = 0;
            break;
        case BAD_ELF_FLAGS:
            header->flags = 1U;
            break;
        case TOO_MANY_PROGRAM_HEADERS:
            header->program_header_count = TEST_PHDR_LIMIT + 1U;
            break;
        case PROGRAM_HEADER_TABLE_BOUNDS:
            g_file.size = sizeof(TestElf64Header) + 2ULL * sizeof(TestElf64ProgramHeader) - 1ULL;
            break;
        case DYNAMIC_RUNTIME:
            data->type = ELF64_PT_DYNAMIC;
            break;
        case INTERPRETER_RUNTIME:
            data->type = ELF64_PT_INTERP;
            break;
        case TLS_RUNTIME:
            data->type = ELF64_PT_TLS;
            break;
        case UNKNOWN_LOAD_FLAGS:
            data->flags |= 0x8U;
            break;
        case WRITABLE_EXECUTABLE:
            data->flags = ELF64_PF_R | ELF64_PF_W | ELF64_PF_X;
            break;
        case FILE_LARGER_THAN_MEMORY:
            data->file_size = 32ULL;
            data->memory_size = 16ULL;
            break;
        case FILE_RANGE_OUTSIDE_SOURCE:
            data->offset = TEST_FILE_CAPACITY - 4ULL;
            data->file_size = 8ULL;
            data->memory_size = 8ULL;
            break;
        case NON_POWER_OF_TWO_ALIGNMENT:
            data->alignment = 3ULL;
            break;
        case ALIGNMENT_CONGRUENCE:
            data->alignment = VM_PAGE_SIZE;
            break;
        case USER_RANGE_OVERFLOW:
            data->virtual_address = ADDRESS_SPACE_USER_LIMIT - 0x800ULL;
            data->memory_size = VM_PAGE_SIZE;
            break;
        case PAGE_LEVEL_LOAD_OVERLAP:
            text->memory_size = 0x100ULL;
            data->virtual_address = TEST_TEXT + 0x800ULL;
            data->memory_size = 0x100ULL;
            break;
        case INITIAL_STACK_RESERVATION:
            data->virtual_address = USER_STACK_INITIAL_BASE - VM_PAGE_SIZE;
            data->memory_size = VM_PAGE_SIZE;
            break;
        case ENTRY_IN_EXECUTABLE_BSS:
            header->entry = TEST_TEXT + 0x100ULL;
            break;
        case ENTRY_IN_NONEXECUTABLE_DATA:
            header->entry = TEST_DATA;
            break;
        case NO_LOAD_SEGMENTS:
            text->type = 4U;
            data->type = 4U;
            break;
        case EXISTING_MAPPING_COLLISION:
            data->virtual_address = TEST_COLLISION;
            break;
    }
}

static bool mapping_absent(const AddressSpace *space, u64 address) {
    return space && !address_space_query_page(space, address, 0, 0);
}

static bool mapping_matches(const AddressSpace *space, u64 address, vm_flags_t required, vm_flags_t forbidden) {
    frame_t frame = FRAME_INVALID;
    vm_flags_t flags = 0;
    return space && address_space_query_page(space, address, &frame, &flags) && frame != FRAME_INVALID &&
        (flags & required) == required && !(flags & forbidden);
}

static bool image_empty(const UserTestFixture *fixture) {
    return fixture && !user_elf_needs_cleanup(&fixture->image) && !fixture->process.elf_image &&
        !fixture->image.entry && !fixture->image.page_count && !fixture->image.loaded &&
        !fixture->image.cleanup_pending && !fixture->image.owner && !fixture->image.owner_process_id &&
        !fixture->image.owner_space_id;
}

static bool run_preflight_reject(UserTestFixture *fixture, AddressSpace *space,
    const char *name, MalformedCase test_case) {
    build_valid_elf();
    mutate_elf(test_case);
    if (!image_empty(fixture)) return false;

    u64 free_before = pmm_stats().free_pages;
    if (!user_elf_test_fail_once(&fixture->image, USER_ELF_TEST_ALLOC, TEST_TEXT)) return false;

    bool rejected = !user_fixture_load(fixture, &g_file);
    u32 faults = user_elf_test_faults_armed();
    bool pass = rejected && faults == (1U << USER_ELF_TEST_ALLOC) && image_empty(fixture) &&
        pmm_stats().free_pages == free_before && mapping_absent(space, TEST_TEXT);

    user_elf_test_clear_faults();
    user_fixture_check(name, pass);
    return pass;
}

static bool valid_control(UserTestFixture *fixture, AddressSpace *space) {
    build_valid_elf();
    u64 free_before = pmm_stats().free_pages;
    if (!user_fixture_load(fixture, &g_file)) {
        user_fixture_check("VALID CONTROL LOAD", false);
        return false;
    }

    bool mapped = fixture->image.loaded && fixture->image.entry == TEST_TEXT && fixture->image.page_count == 2U &&
        mapping_matches(space, TEST_TEXT, VM_USER | VM_EXEC, VM_WRITE) &&
        mapping_matches(space, TEST_DATA, VM_USER | VM_WRITE, VM_EXEC);
    user_fixture_check("VALID CONTROL LOAD", mapped);
    if (!mapped) return false;

    bool unloaded = user_elf_unload(&fixture->process, &fixture->image) && image_empty(fixture) &&
        mapping_absent(space, TEST_TEXT) && mapping_absent(space, TEST_DATA) &&
        pmm_stats().free_pages == free_before;
    user_fixture_check("VALID CONTROL UNLOAD", unloaded);
    return unloaded;
}

static bool automatic_rollback(UserTestFixture *fixture, AddressSpace *space) {
    build_valid_elf();
    u64 free_before = pmm_stats().free_pages;
    if (!user_elf_test_fail_once(&fixture->image, USER_ELF_TEST_ALLOC, TEST_DATA)) return false;

    bool rejected = !user_fixture_load(fixture, &g_file);
    bool pass = rejected && !user_elf_test_faults_armed() && image_empty(fixture) &&
        mapping_absent(space, TEST_TEXT) && mapping_absent(space, TEST_DATA) &&
        pmm_stats().free_pages == free_before;
    user_fixture_check("MID-LOAD FAILURE AUTO-ROLLBACK", pass);
    return pass;
}

static bool retained_rollback_retry(UserTestFixture *fixture, AddressSpace *space) {
    build_valid_elf();
    u64 free_before = pmm_stats().free_pages;
    if (!user_elf_test_fail_once(&fixture->image, USER_ELF_TEST_ALLOC, TEST_DATA) ||
        !user_elf_test_fail_once(&fixture->image, USER_ELF_TEST_UNMAP, TEST_TEXT)) return false;

    bool rejected = !user_fixture_load(fixture, &g_file);
    frame_t text_frame = FRAME_INVALID;
    bool retained = rejected && !user_elf_test_faults_armed() && user_elf_needs_cleanup(&fixture->image) &&
        fixture->process.elf_image == &fixture->image && !fixture->image.loaded && !fixture->image.entry &&
        fixture->image.cleanup_pending && fixture->image.page_count == 1U && fixture->image.pages[0].mapped &&
        fixture->image.pages[0].virtual_address == TEST_TEXT &&
        address_space_query_page(space, TEST_TEXT, &text_frame, 0) && text_frame == fixture->image.pages[0].frame &&
        mapping_absent(space, TEST_DATA) && pmm_stats().free_pages < free_before;
    user_fixture_check("FAILED ROLLBACK RETAINS OWNERSHIP", retained);
    if (!retained) return false;

    bool retried = user_elf_unload(&fixture->process, &fixture->image) && image_empty(fixture) &&
        mapping_absent(space, TEST_TEXT) && mapping_absent(space, TEST_DATA) &&
        pmm_stats().free_pages == free_before;
    user_fixture_check("RETAINED ROLLBACK RETRY", retried);
    return retried;
}

void elf_malformed_test_run(void) {
    if (!user_fixture_available()) return;
    terminal_writeln("ELF MALFORMED / ROLLBACK ACCEPTANCE TEST:");

    UserTestFixture *fixture = user_fixture_begin("ELF MALFORMED / ROLLBACK ACCEPTANCE TEST");
    if (!fixture) return;

    bool pass = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", pass);
    if (!pass) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    pass = space && !space->kernel && address_space_cr3(space);
    user_fixture_check("ADDRESS SPACE", pass);
    if (!pass) goto finish;

    if (!valid_control(fixture, space)) { pass = false; goto finish; }

    if (!run_preflight_reject(fixture, space, "BAD MAGIC REJECTED PRE-ALLOC", BAD_MAGIC) ||
        !run_preflight_reject(fixture, space, "NONZERO ELF FLAGS REJECTED PRE-ALLOC", BAD_ELF_FLAGS) ||
        !run_preflight_reject(fixture, space, "PHDR COUNT LIMIT REJECTED PRE-ALLOC", TOO_MANY_PROGRAM_HEADERS) ||
        !run_preflight_reject(fixture, space, "PHDR TABLE BOUNDS REJECTED PRE-ALLOC", PROGRAM_HEADER_TABLE_BOUNDS) ||
        !run_preflight_reject(fixture, space, "PT_DYNAMIC REJECTED PRE-ALLOC", DYNAMIC_RUNTIME) ||
        !run_preflight_reject(fixture, space, "PT_INTERP REJECTED PRE-ALLOC", INTERPRETER_RUNTIME) ||
        !run_preflight_reject(fixture, space, "PT_TLS REJECTED PRE-ALLOC", TLS_RUNTIME) ||
        !run_preflight_reject(fixture, space, "UNKNOWN LOAD FLAGS REJECTED PRE-ALLOC", UNKNOWN_LOAD_FLAGS) ||
        !run_preflight_reject(fixture, space, "W+X LOAD REJECTED PRE-ALLOC", WRITABLE_EXECUTABLE) ||
        !run_preflight_reject(fixture, space, "FILESZ > MEMSZ REJECTED PRE-ALLOC", FILE_LARGER_THAN_MEMORY) ||
        !run_preflight_reject(fixture, space, "FILE RANGE REJECTED PRE-ALLOC", FILE_RANGE_OUTSIDE_SOURCE) ||
        !run_preflight_reject(fixture, space, "BAD ALIGNMENT REJECTED PRE-ALLOC", NON_POWER_OF_TWO_ALIGNMENT) ||
        !run_preflight_reject(fixture, space, "ALIGNMENT CONGRUENCE REJECTED PRE-ALLOC", ALIGNMENT_CONGRUENCE) ||
        !run_preflight_reject(fixture, space, "USER RANGE OVERFLOW REJECTED PRE-ALLOC", USER_RANGE_OVERFLOW) ||
        !run_preflight_reject(fixture, space, "PT_LOAD PAGE OVERLAP REJECTED PRE-ALLOC", PAGE_LEVEL_LOAD_OVERLAP) ||
        !run_preflight_reject(fixture, space, "INITIAL STACK RESERVATION REJECTED PRE-ALLOC", INITIAL_STACK_RESERVATION) ||
        !run_preflight_reject(fixture, space, "EXECUTABLE BSS ENTRY REJECTED PRE-ALLOC", ENTRY_IN_EXECUTABLE_BSS) ||
        !run_preflight_reject(fixture, space, "NONEXECUTABLE ENTRY REJECTED PRE-ALLOC", ENTRY_IN_NONEXECUTABLE_DATA) ||
        !run_preflight_reject(fixture, space, "NO PT_LOAD REJECTED PRE-ALLOC", NO_LOAD_SEGMENTS)) {
        pass = false;
        goto finish;
    }

    pass = user_fixture_stack_create(fixture, 0U, TEST_COLLISION, 0, 0);
    user_fixture_check("EXISTING USER MAPPING CREATED", pass);
    if (!pass) goto finish;
    if (!run_preflight_reject(fixture, space, "EXISTING MAPPING COLLISION REJECTED PRE-ALLOC",
            EXISTING_MAPPING_COLLISION)) {
        pass = false;
        goto finish;
    }

    if (!automatic_rollback(fixture, space) || !retained_rollback_retry(fixture, space)) {
        pass = false;
        goto finish;
    }

finish: {
        bool finished = user_fixture_finish(fixture, pass);
        test_output_final("ELF MALFORMED / ROLLBACK ACCEPTANCE TEST", finished);
    }
}
