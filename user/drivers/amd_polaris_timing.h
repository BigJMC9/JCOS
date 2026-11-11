#ifndef JCOS_AMD_POLARIS_TIMING_H
#define JCOS_AMD_POLARIS_TIMING_H
#include "../../include/polaris_timing_protocol.h"
int polaris_timing_observe(const JcosU32 *values,JcosU32 count,JcosU64 report[POLARIS_TIMING_WORDS]);
#endif
