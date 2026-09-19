#ifndef JA_OS_VMM_TEST_H
#define JA_OS_VMM_TEST_H
#include "vmm.h"
/* Test-only: exact map, fail after skip matching operations, disarmed on use. */
typedef enum { VMM_TEST_ALLOC, VMM_TEST_ACCESS, VMM_TEST_FREE, VMM_TEST_FAULT_COUNT } VmmTestFault;

typedef struct {
    frame_t frame;
    vm_flags_t pml4_flags;
    vm_flags_t pdpt_flags;
    vm_flags_t pd_flags;
    vm_flags_t pt_flags;
    vm_flags_t effective_flags;
} VmmTestPageWalk;

bool vmm_test_fail_once(VmPageMap *map, VmmTestFault fault, u32 skip);
u32 vmm_test_faults_armed(void);
void vmm_test_clear_faults(void);
frame_t vmm_test_unlinked_table(void);

/* Test-only structural inspection of one present 4 KiB mapping. Flags use the
 * public VM_WRITE / VM_USER / VM_EXEC vocabulary at every paging level. */
bool vmm_test_page_walk(const VmPageMap *map, u64 virtual_address, VmmTestPageWalk *out);
#endif
