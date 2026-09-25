#include "display_surface.h"

#include "address_space.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "vmm.h"
#include "../include/display_service_protocol.h"

static u64 g_next_surface_id = 1ULL;

bool display_surface_valid(const DisplaySurface *surface) {
    return surface && surface->allocated && surface->id && surface->frame != FRAME_INVALID;
}

u64 display_surface_id(const DisplaySurface *surface) {
    return display_surface_valid(surface) ? surface->id : 0ULL;
}

void *display_surface_data(const DisplaySurface *surface) {
    if (!display_surface_valid(surface)) return 0;
    return phys_to_virt(frame_to_phys(surface->frame));
}

bool display_surface_create(DisplaySurface *surface) {
    if (!surface || surface->allocated || !g_next_surface_id) return false;

    frame_t frame = frame_alloc();
    if (frame == FRAME_INVALID) return false;
    void *data = phys_to_virt(frame_to_phys(frame));
    if (!data) {
        (void)frame_free(frame);
        return false;
    }

    k_memset(data, 0, JCOS_DISPLAY_SURFACE_BYTES);
    k_memset(surface, 0, sizeof(*surface));
    surface->id = g_next_surface_id++;
    surface->frame = frame;
    surface->allocated = true;
    return true;
}

bool display_surface_destroy(DisplaySurface *surface) {
    if (!surface) return false;
    if (!surface->allocated) {
        k_memset(surface, 0, sizeof(*surface));
        return true;
    }
    if (surface->frame == FRAME_INVALID || surface->writer_process_id ||
        surface->reader_process_id || surface->reader_virtual_base ||
        !frame_free(surface->frame)) return false;
    k_memset(surface, 0, sizeof(*surface));
    return true;
}

bool display_surface_writer_mapping(const DisplaySurface *surface,
    ProgramBorrowedMappingSpec *out_mapping) {
    if (!out_mapping) return false;
    k_memset(out_mapping, 0, sizeof(*out_mapping));
    if (!display_surface_valid(surface) || surface->writer_process_id) return false;

    out_mapping->physical_base = frame_to_phys(surface->frame);
    out_mapping->size = JCOS_DISPLAY_SURFACE_BYTES;
    out_mapping->virtual_base = JCOS_DISPLAY_SURFACE_VIRTUAL_BASE;
    out_mapping->flags = VM_WRITE;
    return true;
}

bool display_surface_writer_claim(DisplaySurface *surface, Process *process) {
    if (!display_surface_valid(surface) || surface->writer_process_id || !process ||
        !process_storage_in_use(process) || !process->initialized || process->kernel || !process->id)
        return false;

    AddressSpace *space = process_address_space(process);
    frame_t frame = FRAME_INVALID;
    vm_flags_t flags = 0;
    if (!space || !address_space_query_page(space, JCOS_DISPLAY_SURFACE_VIRTUAL_BASE,
            &frame, &flags) || frame != surface->frame || !(flags & VM_USER) ||
        !(flags & VM_WRITE) || (flags & VM_EXEC) || (flags & VM_UNCACHED)) return false;

    surface->writer_process_id = process->id;
    return true;
}

bool display_surface_writer_release(DisplaySurface *surface, u64 owner_process_id) {
    if (!display_surface_valid(surface) || !owner_process_id ||
        surface->writer_process_id != owner_process_id) return false;
    surface->writer_process_id = 0ULL;
    return true;
}

u64 display_surface_writer_process_id(const DisplaySurface *surface) {
    return display_surface_valid(surface) ? surface->writer_process_id : 0ULL;
}

static bool reader_base_valid(u64 virtual_base) {
    return !(virtual_base & (VM_PAGE_SIZE - 1ULL)) &&
        virtual_base >= ADDRESS_SPACE_USER_BASE &&
        virtual_base <= ADDRESS_SPACE_USER_LIMIT - VM_PAGE_SIZE;
}

bool display_surface_map_reader(DisplaySurface *surface, Process *process, u64 virtual_base) {
    if (!display_surface_valid(surface) || surface->reader_process_id ||
        surface->reader_virtual_base || !reader_base_valid(virtual_base) || !process ||
        !process_storage_in_use(process) || !process->initialized || process->kernel || !process->id)
        return false;
    AddressSpace *space = process_address_space(process);
    if (!space) return false;

    frame_t mapped = FRAME_INVALID;
    if (address_space_query_page(space, virtual_base, &mapped, 0) ||
        !address_space_map_page(space, virtual_base, surface->frame, 0)) return false;

    surface->reader_process_id = process->id;
    surface->reader_virtual_base = virtual_base;
    return true;
}

bool display_surface_unmap_reader(DisplaySurface *surface, Process *process, u64 virtual_base) {
    if (!display_surface_valid(surface) || !surface->reader_process_id ||
        surface->reader_virtual_base != virtual_base || !process ||
        !process_storage_in_use(process) || !process->initialized || process->kernel ||
        process->id != surface->reader_process_id) return false;
    AddressSpace *space = process_address_space(process);
    if (!space) return false;

    frame_t mapped = FRAME_INVALID;
    frame_t old = FRAME_INVALID;
    if (!address_space_query_page(space, virtual_base, &mapped, 0) ||
        mapped != surface->frame || !address_space_unmap_page(space, virtual_base, &old) ||
        old != surface->frame) return false;

    surface->reader_process_id = 0ULL;
    surface->reader_virtual_base = 0ULL;
    return true;
}

bool display_surface_reader_mapping_valid(const DisplaySurface *surface, Process *process,
    u64 virtual_base) {
    if (!display_surface_valid(surface) || !surface->reader_process_id ||
        surface->reader_virtual_base != virtual_base || !process ||
        !process_storage_in_use(process) || !process->initialized || process->kernel ||
        process->id != surface->reader_process_id) return false;
    AddressSpace *space = process_address_space(process);
    if (!space) return false;

    frame_t frame = FRAME_INVALID;
    vm_flags_t flags = 0;
    return address_space_query_page(space, virtual_base, &frame, &flags) &&
        frame == surface->frame && (flags & VM_USER) && !(flags & VM_WRITE) &&
        !(flags & VM_EXEC) && !(flags & VM_UNCACHED);
}