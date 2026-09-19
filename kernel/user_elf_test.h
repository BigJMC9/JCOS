#ifndef JA_OS_USER_ELF_TEST_H
#define JA_OS_USER_ELF_TEST_H

#include "user_elf.h"

/* Diagnostic-only, exact image/address, one-shot failures before operations. */
typedef enum {
    USER_ELF_TEST_ALLOC = 0,
    USER_ELF_TEST_MAP,
    USER_ELF_TEST_UNMAP,
    USER_ELF_TEST_FREE,
    USER_ELF_TEST_FAULT_COUNT
} UserElfTestFault;

bool user_elf_test_fail_once(UserElfImage *image, UserElfTestFault fault, u64 address);
u32 user_elf_test_faults_armed(void);
void user_elf_test_clear_faults(void);

#endif