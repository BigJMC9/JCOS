#ifndef JA_OS_PROGRAM_ROLLBACK_TEST_H
#define JA_OS_PROGRAM_ROLLBACK_TEST_H

#include "program.h"

/* Internal cases used by test program-launch; no additional shell command. */
bool program_rollback_test_run(const ProgramLaunchSpec *spec);
bool program_rollback_test_cleanup(void);

#endif
