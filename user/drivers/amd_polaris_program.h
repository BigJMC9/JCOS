#ifndef JCOS_AMD_POLARIS_PROGRAM_H
#define JCOS_AMD_POLARIS_PROGRAM_H
#include "../../include/polaris_rebind_protocol.h"
typedef struct {
    void *context;
    int (*exchange)(void *,JcosU32 operation,JcosU32 field,JcosU32 payload,JcosU32 *value);
} PolarisUserProgramIo;
/* Driver-side vendor sequence; the kernel independently constrains every IO. */
int polaris_user_program(const PolarisUserProgramIo *io,JcosU32 flip,JcosU32 *frame_delta);
#endif
