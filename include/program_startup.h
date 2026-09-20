#ifndef JCOS_PROGRAM_STARTUP_H
#define JCOS_PROGRAM_STARTUP_H

#include "user_abi.h"

#define JCOS_PROGRAM_STARTUP_MAGIC 0x4A434F5350524731ULL
#define JCOS_PROGRAM_STARTUP_VERSION 1U
#define JCOS_PROGRAM_STARTUP_FLAG_NONE 0U
#define JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES 8U
#define JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS 4U

typedef struct {
    JcosU64 magic;
    JcosU32 version;
    JcosU32 size;
    JcosU32 flags;
    JcosU32 capability_count;
    JcosU32 argument_count;
    JcosU32 environment_count;
    JcosCapabilityHandle capabilities[JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES];
    JcosU64 arguments[JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS];
} JcosProgramStartup;

_Static_assert(sizeof(JcosProgramStartup) == 128U, "JcosProgramStartup ABI size");

#endif
