#include "vfs.h"

#define VFS_MAX_NODES 256

static VfsNode g_nodes[VFS_MAX_NODES];
static u32 g_node_count;
static VfsNode *g_root;


static void zero_node(VfsNode *node) {
    u8 *bytes = (u8 *)(void *)node;

    for (u32 i = 0; i < sizeof(VfsNode); ++i)
        bytes[i] = 0;
}


static u32 string_length(const char *s) {
    u32 length = 0;

    if (!s)
        return 0;

    while (s[length])
        ++length;

    return length;
}


static bool string_equal(
    const char *a,
    const char *b
) {
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (*a != *b)
            return false;

        ++a;
        ++b;
    }

    return *a == *b;
}


static VfsNode *allocate_node(void) {
    if (g_node_count >= VFS_MAX_NODES)
        return 0;

    VfsNode *node = &g_nodes[g_node_count++];

    zero_node(node);

    return node;
}


static bool set_node_name(
    VfsNode *node,
    const char *name
) {
    if (!node || !name)
        return false;

    u32 length = string_length(name);

    if (length > VFS_NAME_MAX)
        return false;

    for (u32 i = 0; i < length; ++i)
        node->name[i] = name[i];

    node->name[length] = 0;

    return true;
}


static void append_child(
    VfsNode *parent,
    VfsNode *child
) {
    child->parent = parent;
    child->next_sibling = 0;

    if (!parent->first_child) {
        parent->first_child = child;
        return;
    }

    VfsNode *current = parent->first_child;

    while (current->next_sibling)
        current = current->next_sibling;

    current->next_sibling = child;
}


/*
 * Reads one path component.
 *
 * Returns:
 *   1  component read
 *   0  end of path
 *  -1  component too long
 */
static int next_component(
    const char **cursor,
    char component[VFS_NAME_MAX + 1]
) {
    const char *p = *cursor;

    while (*p == '/')
        ++p;

    if (!*p) {
        *cursor = p;
        return 0;
    }

    u32 length = 0;

    while (*p && *p != '/') {
        if (length >= VFS_NAME_MAX)
            return -1;

        component[length++] = *p++;
    }

    component[length] = 0;

    *cursor = p;

    return 1;
}


void vfs_init(void) {
    g_node_count = 0;
    g_root = allocate_node();

    if (!g_root)
        return;

    g_root->name[0] = 0;
    g_root->type = VFS_DIRECTORY;
    g_root->parent = 0;
    g_root->first_child = 0;
    g_root->next_sibling = 0;
}


VfsNode *vfs_root(void) {
    return g_root;
}


VfsNode *vfs_find_child(
    VfsNode *directory,
    const char *name
) {
    if (!directory ||
        directory->type != VFS_DIRECTORY ||
        !name) {
        return 0;
    }

    for (VfsNode *node = directory->first_child;
         node;
         node = node->next_sibling) {

        if (string_equal(node->name, name))
            return node;
    }

    return 0;
}


static VfsNode *ensure_directory(
    VfsNode *parent,
    const char *name
) {
    VfsNode *existing =
        vfs_find_child(parent, name);

    if (existing) {
        if (existing->type != VFS_DIRECTORY)
            return 0;

        return existing;
    }

    VfsNode *node = allocate_node();

    if (!node)
        return 0;

    if (!set_node_name(node, name))
        return 0;

    node->type = VFS_DIRECTORY;

    append_child(parent, node);

    return node;
}


VfsNode *vfs_resolve(
    VfsNode *start,
    const char *path
) {
    if (!g_root || !path)
        return 0;

    VfsNode *current;

    if (path[0] == '/')
        current = g_root;
    else
        current = start ? start : g_root;

    const char *cursor = path;
    char component[VFS_NAME_MAX + 1];

    for (;;) {
        int result =
            next_component(&cursor, component);

        if (result == 0)
            return current;

        if (result < 0)
            return 0;

        if (string_equal(component, "."))
            continue;

        if (string_equal(component, "..")) {
            if (current->parent)
                current = current->parent;

            continue;
        }

        current =
            vfs_find_child(current, component);

        if (!current)
            return 0;
    }
}


bool vfs_add_directory(
    const char *path
) {
    if (!g_root || !path)
        return false;

    VfsNode *current = g_root;
    const char *cursor = path;
    char component[VFS_NAME_MAX + 1];

    for (;;) {
        int result =
            next_component(&cursor, component);

        if (result == 0)
            return true;

        if (result < 0)
            return false;

        if (string_equal(component, "."))
            continue;

        /*
         * Don't allow TAR entries to escape the root
         * filesystem using "../".
         */
        if (string_equal(component, ".."))
            return false;

        current =
            ensure_directory(current, component);

        if (!current)
            return false;
    }
}


bool vfs_add_file(
    const char *path,
    const u8 *data,
    u64 size
) {
    if (!g_root || !path)
        return false;

    VfsNode *current = g_root;
    const char *cursor = path;
    char component[VFS_NAME_MAX + 1];

    for (;;) {
        int result =
            next_component(&cursor, component);

        if (result <= 0)
            return false;

        if (string_equal(component, "."))
            continue;

        if (string_equal(component, ".."))
            return false;

        /*
         * See whether another component follows.
         */
        const char *look = cursor;

        while (*look == '/')
            ++look;

        bool final_component = (*look == 0);

        if (!final_component) {
            current =
                ensure_directory(
                    current,
                    component
                );

            if (!current)
                return false;

            continue;
        }

        VfsNode *existing =
            vfs_find_child(
                current,
                component
            );

        if (existing) {
            if (existing->type != VFS_FILE)
                return false;

            existing->data = data;
            existing->size = size;

            return true;
        }

        VfsNode *file = allocate_node();

        if (!file)
            return false;

        if (!set_node_name(file, component))
            return false;

        file->type = VFS_FILE;
        file->data = data;
        file->size = size;

        append_child(current, file);

        return true;
    }
}


bool vfs_get_path(
    const VfsNode *node,
    char *buffer,
    u32 capacity
) {
    if (!node || !buffer || capacity == 0)
        return false;

    if (node == g_root) {
        if (capacity < 2)
            return false;

        buffer[0] = '/';
        buffer[1] = 0;

        return true;
    }

    u32 total = 0;
    u32 depth = 0;

    const VfsNode *current = node;

    while (current && current != g_root) {
        u32 length =
            string_length(current->name);

        if (!length)
            return false;

        total += length + 1;

        current = current->parent;

        ++depth;

        if (depth > VFS_MAX_NODES)
            return false;
    }

    /*
     * Node isn't attached to our root.
     */
    if (current != g_root)
        return false;

    if (total + 1 > capacity)
        return false;

    buffer[total] = 0;

    u32 position = total;

    current = node;

    while (current != g_root) {
        u32 length =
            string_length(current->name);

        position -= length;

        for (u32 i = 0; i < length; ++i)
            buffer[position + i] =
                current->name[i];

        --position;
        buffer[position] = '/';

        current = current->parent;
    }

    return true;
}