#ifndef JA_OS_USER_ELF_H
#define JA_OS_USER_ELF_H

#include "types.h"
#include "pmm.h"
#include "process.h"
#include "vfs.h"

#define USER_ELF_MAX_LOAD_PAGES 64U

typedef struct {
    u64 virtual_address;
    frame_t frame;
} UserElfPage;

typedef struct {
    u64 entry;
    UserElfPage pages[USER_ELF_MAX_LOAD_PAGES];
    u32 page_count;
    bool loaded;
} UserElfImage;

bool user_elf_load(Process *process, const VfsNode *file, UserElfImage *image);
bool user_elf_unload(Process *process, UserElfImage *image);

#endif