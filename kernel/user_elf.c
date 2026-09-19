#include "user_elf.h"
#include "user_elf_test.h"
#include "arch.h"
#include "thread.h"

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

/* Callers cannot recycle the owning Process while this ledger is retained. */
bool user_elf_needs_cleanup(const UserElfImage *image) {
    return image && (image->owner || image->owner_process_id || image->owner_space_id ||
        image->page_count || image->loaded || image->cleanup_pending || image->entry);
}

static u64 elf_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    arch_cli();
    return flags;
}

static void elf_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) arch_sti();
}

typedef struct {
    UserElfImage *image;
    u64 address;
    bool armed;
} ElfTestFault;

static ElfTestFault g_elf_faults[USER_ELF_TEST_FAULT_COUNT];

bool user_elf_test_fail_once(UserElfImage *image, UserElfTestFault fault, u64 address) {
    if (!image || (u32)fault >= USER_ELF_TEST_FAULT_COUNT ||
        address < ADDRESS_SPACE_USER_BASE || address >= ADDRESS_SPACE_USER_LIMIT ||
        (address & (VM_PAGE_SIZE - 1ULL))) return false;
    u64 flags = elf_irq_save();
    ElfTestFault *slot = &g_elf_faults[fault];
    bool ready = !slot->armed;
    if (ready) {
        slot->image = image;
        slot->address = address;
        slot->armed = true;
    }
    elf_irq_restore(flags);
    return ready;
}

u32 user_elf_test_faults_armed(void) {
    u64 flags = elf_irq_save();
    u32 mask = 0;
    for (u32 i = 0; i < USER_ELF_TEST_FAULT_COUNT; ++i) {
        if (g_elf_faults[i].armed) mask |= 1U << i;
    }
    elf_irq_restore(flags);
    return mask;
}

void user_elf_test_clear_faults(void) {
    u64 flags = elf_irq_save();
    k_memset(g_elf_faults, 0, sizeof(g_elf_faults));
    elf_irq_restore(flags);
}

static bool fail_operation(UserElfImage *image, UserElfTestFault fault, u64 address) {
    ElfTestFault *slot = &g_elf_faults[fault];
    if (!slot->armed || slot->image != image || slot->address != address) return false;
    k_memset(slot, 0, sizeof(*slot));
    return true;
}

static bool process_available(Process *process, AddressSpace **out_space) {
    if (!process || !process->initialized || !process->id || process->kernel ||
        !process->owns_address_space || process->address_space != &process->owned_address_space) return false;
    AddressSpace *space = process_address_space(process);
    if (!space || space->kernel || !space->id || !address_space_cr3(space)) return false;
    /* Current VMM unmap has no active-address-space TLB invalidation contract. */
    if ((arch_read_cr3() & ~(VM_PAGE_SIZE - 1ULL)) == address_space_cr3(space)) return false;
    *out_space = space;
    return true;
}

static bool threads_unpublished(const Process *process) {
    const Thread *thread = process->thread_head;
    const Thread *previous = 0;
    for (u64 i = 0; i < process->thread_count; ++i) {
        if (!thread || !thread->id || thread->process != process ||
            thread->process_prev != previous || thread->state != THREAD_STATE_READY ||
            thread->on_run_queue || thread->run_next || thread->interrupt_context_ready ||
            thread->interrupt_rsp || thread_wait_active(thread)) return false;
        previous = thread;
        thread = thread->process_next;
    }
    return !thread && previous == process->thread_tail;
}

static bool image_owner_matches(Process *process, AddressSpace *space, const UserElfImage *image) {
    return process->elf_image == image && image->owner == process &&
        image->owner_process_id == process->id && image->owner_space_id == space->id &&
        image->page_count <= USER_ELF_MAX_LOAD_PAGES;
}

static bool release_pages(Process *process, AddressSpace *space, UserElfImage *image) {
    if (!image_owner_matches(process, space, image)) return false;
    image->entry = 0;
    image->loaded = false;
    image->cleanup_pending = true;

    while (image->page_count) {
        UserElfPage *page = &image->pages[image->page_count - 1U];
        if (page->frame == FRAME_INVALID) return false;
        if (page->mapped) {
            frame_t current = FRAME_INVALID;
            if (!address_space_query_page(space, page->virtual_address, &current, 0) ||
                current != page->frame) return false;
            if (fail_operation(image, USER_ELF_TEST_UNMAP, page->virtual_address)) return false;
            frame_t old = FRAME_INVALID;
            if (!address_space_unmap_page(space, page->virtual_address, &old)) return false;
            /* Publish unmap progress before attempting the independent free. */
            page->mapped = false;
            if (old != page->frame) return false;
        }
        if (fail_operation(image, USER_ELF_TEST_FREE, page->virtual_address) ||
            !frame_free(page->frame)) return false;

        k_memset(page, 0, sizeof(*page));
        --image->page_count;
    }

    process->elf_image = 0;
    k_memset(image, 0, sizeof(*image));
    return true;
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
        if (segment->file_size > segment->memory_size) return false;
        if (!segment->memory_size) continue;

        saw_load = true;

        if (segment->file_size > segment->memory_size) return false;
        if (segment->offset > file->size) return false;
        if (segment->file_size > file->size - segment->offset) return false;
        if (segment->virtual_address < ADDRESS_SPACE_USER_BASE ||
            segment->virtual_address >= ADDRESS_SPACE_USER_LIMIT) return false;
        if (segment->memory_size > ADDRESS_SPACE_USER_LIMIT - segment->virtual_address) return false;

        /* Reject writable/executable segments; hardware NX is a separate gate. */
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
    if (segment->flags & ELF64_PF_X) flags |= VM_EXEC;

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

        if (image->page_count >= USER_ELF_MAX_LOAD_PAGES ||
            fail_operation(image, USER_ELF_TEST_ALLOC, virtual_address)) return false;
        frame_t frame = frame_alloc();
        if (frame == FRAME_INVALID) return false;

        /* Own the allocation before any subsequent operation can fail. */
        UserElfPage *page = &image->pages[image->page_count++];
        page->virtual_address = virtual_address;
        page->frame = frame;
        page->mapped = false;

        void *direct = phys_to_virt(frame_to_phys(frame));
        if (!direct) return false;
        k_memset(direct, 0, (usize)VM_PAGE_SIZE);

        if (fail_operation(image, USER_ELF_TEST_MAP, virtual_address) ||
            !address_space_map_page(space, virtual_address, frame, flags)) return false;
        page->mapped = true;
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

static bool load_locked(Process *process, const VfsNode *file, UserElfImage *image) {
    if (!file || !image || user_elf_needs_cleanup(image)) return false;
    AddressSpace *space = 0;
    if (!process_available(process, &space) || process->elf_image ||
        !threads_unpublished(process)) return false;
    if (file->type != VFS_FILE || !file->data || file->size < sizeof(Elf64Header)) return false;

    const Elf64Header *header = (const Elf64Header *)(const void *)file->data;
    u32 expected_pages = 0;
    if (!elf_header_valid(file, header) || !validate_segments(file, header, &expected_pages)) return false;

    k_memset(image, 0, sizeof(*image));
    image->owner = process;
    image->owner_process_id = process->id;
    image->owner_space_id = space->id;
    process->elf_image = image;

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
    /* A false return can leave an explicitly retained cleanup ledger. */
    (void)release_pages(process, space, image);
    return false;
}

bool user_elf_load(Process *process, const VfsNode *file, UserElfImage *image) {
    u64 flags = elf_irq_save();
    bool result = load_locked(process, file, image);
    elf_irq_restore(flags);
    return result;
}

bool user_elf_unload(Process *process, UserElfImage *image) {
    u64 flags = elf_irq_save();
    AddressSpace *space = 0;
    bool result = false;
    if (image && process_available(process, &space) &&
        !process->thread_count && !process->thread_head && !process->thread_tail &&
        image_owner_matches(process, space, image)) {
        result = release_pages(process, space, image);
    }
    elf_irq_restore(flags);
    return result;
}