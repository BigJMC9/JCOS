#ifndef JA_OS_FOREGROUND_PROGRAM_H
#define JA_OS_FOREGROUND_PROGRAM_H

#include "process.h"
#include "types.h"

/* Execute exactly one pending Ring3-selected immutable archive extent as a
 * foreground program. Pathname selection has already happened in userspace. */
bool foreground_program_run_pending(void);
bool foreground_program_cleanup(void);
bool foreground_program_idle(void);
u64 foreground_program_run_count(void);
bool foreground_program_last_exit(ProcessExitInfo *out);

#endif
