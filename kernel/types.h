#ifndef JA_OS_KERNEL_TYPES_H
#define JA_OS_KERNEL_TYPES_H

#include "../include/boot_info.h"

typedef u64 usize;
typedef s64 isize;

typedef enum { false = 0, true = 1 } bool;

#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define ARRAY_COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))

#endif
