#ifndef JAOS_AMD_POLARIS_VERTICAL_H
#define JAOS_AMD_POLARIS_VERTICAL_H
#include "../../include/polaris_vertical_protocol.h"
int polaris_vertical_plan(const JcosU32 *values,JcosU32 count,JcosU64 out[POLARIS_VERTICAL_WORDS]);
#endif
