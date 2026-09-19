#ifndef JA_OS_TEST_REGISTRY_H
#define JA_OS_TEST_REGISTRY_H

#include "types.h"

typedef enum {
    KERNEL_TEST_MEMORY = 0,
    KERNEL_TEST_TASK,
    KERNEL_TEST_IPC,
    KERNEL_TEST_LIFETIME,
    KERNEL_TEST_USERSPACE,
    KERNEL_TEST_SCHEDULING,
    KERNEL_TEST_ACCEPTANCE,
    KERNEL_TEST_GROUP_COUNT
} KernelTestGroup;

typedef struct {
    const char *name;
    const char *description;
    KernelTestGroup group;
    void (*run)(void);
    void (*cleanup)(void);
} KernelTest;

u32 kernel_test_registry_count(void);
const KernelTest *kernel_test_registry_at(u32 index);
const KernelTest *kernel_test_registry_find(const char *name);
const char *kernel_test_group_slug(KernelTestGroup group);
const char *kernel_test_group_title(KernelTestGroup group);

#endif
