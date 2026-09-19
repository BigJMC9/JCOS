#ifndef JA_OS_CONSTRUCTION_TEST_H
#define JA_OS_CONSTRUCTION_TEST_H

#include "thread.h"
#include "address_space.h"

/* Exact-target, one-shot diagnostic hooks; no userspace ABI. */
typedef enum {
    THREAD_CREATE_TEST_ALLOCATE = 0,
    THREAD_CREATE_TEST_ACCESS,
    THREAD_CREATE_TEST_ATTACH
} ThreadCreateTestFault;

typedef enum {
    SPACE_CREATE_TEST_ALLOCATE = 0,
    SPACE_CREATE_TEST_AFTER_ROOT,
    SPACE_CREATE_TEST_AFTER_SHARE
} SpaceCreateTestFault;

bool thread_test_fail_create_once(Thread *target, ThreadCreateTestFault fault, bool fail_rollback);
bool thread_test_create_fault_armed(void);
void thread_test_clear_create_fault(void);
frame_t thread_test_unpublished_stack(void);

bool address_space_test_fail_create_once(AddressSpace *target, SpaceCreateTestFault fault, bool fail_rollback);
bool address_space_test_create_fault_armed(void);
void address_space_test_clear_create_fault(void);
frame_t address_space_test_unpublished_root(void);

#endif
