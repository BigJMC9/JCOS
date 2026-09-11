#include "user_elf.h"

#include "address_space.h"
#include "lib.h"
#include "physmap.h"
#include "vmm.h"

#define ELF64_CLASS_64       2U
#define ELF64_DATA_LITTLE    1U
#define ELF64_VERSION        1U

#define ELF64_TYPE_EXEC      2U
#define ELF64_MACHINE_X86_64 62U

#define ELF64_PT_LOAD        1U
#define ELF64_PT_DYNAMIC     2U
#define ELF64_PT_INTERP      3U

#define ELF64_PF_X           1U
#define ELF64_PF_W           2U
#define ELF64_PF_R           4U

#define ELF64_MAX_PHDRS      16U

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
} Elf64Header;

typedef struct __attribute__((packed)) {
    u32 type;
    u32 flags;
    u64 offset;
    u64 virtual_address;
    u64 physical_address;
    u64 file_size;
    u64 memory_size;
    u64 alignment;
} Elf64ProgramHeader;

_Static_assert(sizeof(Elf64Header) == 64, "Elf64Header size");
_Static_assert(sizeof(Elf64ProgramHeader) == 56, "Elf64ProgramHeader size");

static u64 page_down(u64 value) {
    return value &
        ~(VM_PAGE_SIZE - 1ULL);
}

static bool page_up(u64 value, u64 *result) {
    if (!result) return false;
    if (value > ~0ULL - (VM_PAGE_SIZE - 1ULL)) return false;

    *result = (value + VM_PAGE_SIZE - 1ULL) & ~(VM_PAGE_SIZE - 1ULL);

    return true;
}

static const Elf64ProgramHeader *
program_header(const VfsNode *file, const Elf64Header *header, u16 index) {
    if (!file || !header) return 0;
    if (index >= header->program_header_count) return 0;

    u64 offset = header->program_header_offset + (u64)index * sizeof(Elf64ProgramHeader);

    if (offset > file->size) return 0;
    if (sizeof(Elf64ProgramHeader) > file->size - offset) return 0;

    return
        (const Elf64ProgramHeader *)
        (const void *)
        (file->data + offset);
}

static frame_t image_find_frame(const UserElfImage *image, u64 virtual_address) {
    if (!image) return FRAME_INVALID;
    for (u32 i = 0; i < image->page_count; ++i) {
        if (image->pages[i].virtual_address == virtual_address) {
            return image->pages[i].frame;
        }
    }

    return FRAME_INVALID;
}

static bool image_add_page(UserElfImage *image, u64 virtual_address, frame_t frame) {
    if (!image) return false;
    if (image->page_count >= USER_ELF_MAX_LOAD_PAGES) return false;

    UserElfPage *page = &image->pages[image->page_count];

    page->virtual_address = virtual_address;
    page->frame = frame;

    ++image->page_count;
    return true;
}

static bool release_pages(AddressSpace *space, UserElfImage *image) {
    if (!space || !image) return false;

    bool result = true;

    while (image->page_count) {
        u32 index = image->page_count - 1U;
        UserElfPage *page = &image->pages[index];
        frame_t old_frame = FRAME_INVALID;
        bool unmapped = address_space_unmap_page(space, page->virtual_address, &old_frame);

        if (!unmapped || old_frame != page->frame) {
            result = false;
        } else if (!frame_free(old_frame)) {
            result = false;
        }

        page->virtual_address = 0;
        page->frame = FRAME_INVALID;

        --image->page_count;
    }

    image->entry = 0;
    image->loaded = false;
    return result;
}

static bool elf_header_valid(const VfsNode *file, const Elf64Header *header) {
    if (!file || !header) return false;
    if (header->ident[0] != 0x7FU || header->ident[1] != 'E' || header->ident[2] != 'L' || header->ident[3] != 'F') {
        return false;
    }

    if (header->ident[4] != ELF64_CLASS_64) return false;
    if (header->ident[5] != ELF64_DATA_LITTLE) return false;
    if (header->ident[6] != ELF64_VERSION) return false;
    if (header->type != ELF64_TYPE_EXEC) return false;
    if (header->machine != ELF64_MACHINE_X86_64) return false;
    if (header->version != ELF64_VERSION) return false;
    if (header->header_size != sizeof(Elf64Header)) return false;
    if (header->program_header_size != sizeof(Elf64ProgramHeader)) return false;
    if (!header->program_header_count || header->program_header_count > ELF64_MAX_PHDRS) return false;
    if (header->program_header_offset > file->size) return false;

    u64 table_size = (u64)header->program_header_count * sizeof(Elf64ProgramHeader);

    if (table_size > file->size - header->program_header_offset) return false;
    if (header->entry < ADDRESS_SPACE_USER_BASE || header->entry >= ADDRESS_SPACE_USER_LIMIT) return false;
    return true;
}

static bool validate_segments(const VfsNode *file, const Elf64Header *header, u32 *page_count_out) {
    if (!file || !header || !page_count_out) return false;

    u32 total_pages = 0;
    bool saw_load = false;
    bool entry_executable = false;

    for (u16 i = 0; i < header->program_header_count; ++i) {
        const Elf64ProgramHeader *segment = program_header(file, header, i);

        if (!segment) return false;
        if (segment->type == ELF64_PT_DYNAMIC || segment->type == ELF64_PT_INTERP) return false;
        if (segment->type != ELF64_PT_LOAD) continue;
        if (!segment->memory_size) continue;

        saw_load = true;

        if (segment->file_size > segment->memory_size) return false;
        if (segment->offset > file->size) return false;
        if (segment->file_size > file->size - segment->offset) return false;
        if (segment->virtual_address < ADDRESS_SPACE_USER_BASE) return false;
        if (segment->memory_size > ADDRESS_SPACE_USER_LIMIT - segment->virtual_address) return false;

        /* Keep the first loader simple and enforce W^X at ELF segment level. */
        if ((segment->flags & ELF64_PF_W) && (segment->flags & ELF64_PF_X)) return false;
        if (segment->alignment > 1ULL) {
            if (segment->alignment & (segment->alignment - 1ULL)) return false;
            if ((segment->virtual_address % segment->alignment) != (segment->offset % segment->alignment)) return false;
        }

        u64 segment_end = segment->virtual_address + segment->memory_size;
        u64 page_first = page_down(segment->virtual_address);
        u64 page_end = 0;

        if (!page_up(segment_end, &page_end)) return false;
        if (page_first < ADDRESS_SPACE_USER_BASE || page_end > ADDRESS_SPACE_USER_LIMIT || page_end <= page_first) {
            return false;
        }

        u64 pages = (page_end - page_first) / VM_PAGE_SIZE;

        if (pages > USER_ELF_MAX_LOAD_PAGES) return false;
        if (total_pages > USER_ELF_MAX_LOAD_PAGES - (u32)pages) return false;

        total_pages += (u32)pages;

        if ((segment->flags & ELF64_PF_X) && header->entry >= segment->virtual_address && header->entry < segment_end) {
            entry_executable = true;
        }
    }

    if (!saw_load || !total_pages || !entry_executable) return false;

    *page_count_out = total_pages;

    return true;
}

static bool map_segment(AddressSpace *space, const VfsNode *file, const Elf64ProgramHeader *segment,
    UserElfImage *image) {
    if (!space || !file || !segment || !image) return false;
    if (!segment->memory_size) return true;

    u64 segment_end = segment->virtual_address + segment->memory_size;
    u64 page_first = page_down(segment->virtual_address);
    u64 page_end = 0;

    if (!page_up(segment_end, &page_end)) return false;

    vm_flags_t flags = 0;

    if (segment->flags & ELF64_PF_W) flags |= VM_WRITE;

    /*
     * Allocate and zero every page before
     * copying file contents. This also gives
     * correct zero-fill semantics for BSS.
     */
    for (u64 virtual_address = page_first; virtual_address < page_end; virtual_address += VM_PAGE_SIZE) {
        if (address_space_query_page(space, virtual_address, 0, 0)) {
            /* Overlapping PT_LOAD pages and pre-existing mappings are rejected by this first loader. */
            return false;
        }

        frame_t frame = frame_alloc();

        if (frame == FRAME_INVALID) return false;

        void *direct = phys_to_virt(frame_to_phys(frame));

        if (!direct) {
            (void)frame_free(frame);
            return false;
        }

        k_memset(direct, 0, (usize)VM_PAGE_SIZE);

        if (!address_space_map_page(space, virtual_address, frame, flags)) {
            (void)frame_free(frame);
            return false;
        }

        if (!image_add_page(image, virtual_address, frame)) {
            frame_t old_frame = FRAME_INVALID;

            (void)address_space_unmap_page(space, virtual_address, &old_frame);

            (void)frame_free(frame);
            return false;
        }
    }

    /* Copy p_filesz into the newly allocated pages. Remaining p_memsz bytes stay zero. */
    u64 copied = 0;

    while (copied < segment->file_size) {
        u64 destination = segment->virtual_address + copied;
        u64 page_address = page_down(destination);
        u64 page_offset = destination - page_address;
        u64 remaining = segment->file_size - copied;
        u64 amount = VM_PAGE_SIZE - page_offset;

        if (amount > remaining) amount = remaining;

        frame_t frame = image_find_frame(image, page_address);

        if (frame == FRAME_INVALID) return false;

        u8 *direct = (u8 *)phys_to_virt(frame_to_phys(frame));

        if (!direct) return false;

        k_memcpy(direct + page_offset, file->data + segment->offset + copied, (usize)amount);

        copied += amount;
    }

    return true;
}

bool user_elf_load(Process *process, const VfsNode *file, UserElfImage *image) {
    if (!process || !file || !image) return false;
    if (!process->initialized || process->kernel) return false;
    if (file->type != VFS_FILE || !file->data || file->size < sizeof(Elf64Header)) return false;

    AddressSpace *space = process_address_space(process);

    if (!space || space->kernel) return false;

    k_memset(image, 0, sizeof(*image));

    const Elf64Header *header = (const Elf64Header *) (const void *)file->data;

    if (!elf_header_valid(file, header)) return false;

    u32 expected_pages = 0;

    if (!validate_segments(file, header, &expected_pages)) return false;
    for (u16 i = 0; i < header->program_header_count; ++i) {
        const Elf64ProgramHeader *segment = program_header(file, header, i);

        if (!segment) goto fail;
        if (segment->type != ELF64_PT_LOAD) continue;
        if (!map_segment(space, file, segment, image)) goto fail;
    }

    if (image->page_count != expected_pages) goto fail;

    image->entry = header->entry;
    image->loaded = true;
    return true;

fail:
    (void)release_pages(space, image);
    return false;
}

bool user_elf_unload(Process *process, UserElfImage *image) {
    if (!process || !image) return false;
    if (!image->loaded && !image->page_count) return false;

    AddressSpace *space = process_address_space(process);

    if (!space || space->kernel) return false;
    return release_pages(space, image);
}