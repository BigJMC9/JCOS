#ifndef JA_OS_PMM_TEST_H
#define JA_OS_PMM_TEST_H

#include "pmm.h"

/* Kernel diagnostic hooks. The fault fires after validation, before commit. */
bool pmm_test_fail_free_range_once(frame_t first, u64 count);
bool pmm_test_free_failure_armed(void);
void pmm_test_clear_free_failure(void);
bool pmm_test_frame_releasable(frame_t frame);

#endif
