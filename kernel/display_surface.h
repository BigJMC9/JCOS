#ifndef JA_OS_DISPLAY_SURFACE_H
#define JA_OS_DISPLAY_SURFACE_H

#include "process.h"
#include "program.h"
#include "types.h"

typedef struct {
    u64 id;
    frame_t frame;
    u64 writer_process_id;
    u64 reader_process_id;
    u64 reader_virtual_base;
    bool allocated;
} DisplaySurface;

bool display_surface_create(DisplaySurface *surface);
/* Every writer must be fully reaped and every reader unmapped first. */
bool display_surface_destroy(DisplaySurface *surface);
bool display_surface_valid(const DisplaySurface *surface);
u64 display_surface_id(const DisplaySurface *surface);
void *display_surface_data(const DisplaySurface *surface);

bool display_surface_writer_mapping(const DisplaySurface *surface,
    ProgramBorrowedMappingSpec *out_mapping);
bool display_surface_writer_claim(DisplaySurface *surface, Process *process);
/* Call only after the owning ProgramInstance has removed its borrowed mapping. */
bool display_surface_writer_release(DisplaySurface *surface, u64 owner_process_id);
u64 display_surface_writer_process_id(const DisplaySurface *surface);

bool display_surface_map_reader(DisplaySurface *surface, Process *process, u64 virtual_base);
bool display_surface_unmap_reader(DisplaySurface *surface, Process *process, u64 virtual_base);
bool display_surface_reader_mapping_valid(const DisplaySurface *surface, Process *process, u64 virtual_base);

#endif