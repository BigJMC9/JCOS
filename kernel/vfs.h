#ifndef JA_OS_VFS_H
#define JA_OS_VFS_H

#include "types.h"

#define VFS_NAME_MAX 63
#define VFS_PATH_MAX 256

typedef enum {
    VFS_FILE,
    VFS_DIRECTORY
} VfsNodeType;

typedef struct VfsNode {
    char name[VFS_NAME_MAX + 1];

    VfsNodeType type;

    const u8 *data;
    u64 size;

    struct VfsNode *parent;
    struct VfsNode *first_child;
    struct VfsNode *next_sibling;
} VfsNode;

void vfs_init(void);

VfsNode *vfs_root(void);

VfsNode *vfs_find_child(
    VfsNode *directory,
    const char *name
);

VfsNode *vfs_resolve(
    VfsNode *start,
    const char *path
);

bool vfs_add_file(
    const char *path,
    const u8 *data,
    u64 size
);

bool vfs_add_directory(
    const char *path
);

bool vfs_get_path(
    const VfsNode *node,
    char *buffer,
    u32 capacity
);

#endif