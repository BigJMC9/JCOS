#ifndef JA_OS_VMM_TEST_H
#define JA_OS_VMM_TEST_H
#include "vmm.h"
/* Test-only: exact map, fail after skip matching operations, disarmed on use. */
typedef enum { VMM_TEST_ALLOC, VMM_TEST_ACCESS, VMM_TEST_FREE, VMM_TEST_FAULT_COUNT } VmmTestFault;
bool vmm_test_fail_once(VmPageMap *map, VmmTestFault fault, u32 skip);
u32 vmm_test_faults_armed(void);
void vmm_test_clear_faults(void);
frame_t vmm_test_unlinked_table(void);
#endif
