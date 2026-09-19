#ifndef JA_OS_CAPABILITY_TEST_H
#define JA_OS_CAPABILITY_TEST_H
#include "types.h"
/* Tests the same issuer boundary helper on a private counter, not live exhaustion. */
bool capability_test_generation_boundary(void);
void capability_table_test_run(void);
void capability_lifetime_test_run(void);
#endif
