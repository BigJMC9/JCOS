#ifndef JA_OS_USER_MEMORY_H
#define JA_OS_USER_MEMORY_H

#include "process.h"
#include "types.h"

bool user_memory_read(const Process *process, u64 user_address, void *destination, u64 size);

#endif
