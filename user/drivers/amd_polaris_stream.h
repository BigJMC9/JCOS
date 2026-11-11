#ifndef JAOS_AMD_POLARIS_STREAM_H
#define JAOS_AMD_POLARIS_STREAM_H
#include "../../include/polaris_stream_protocol.h"
int polaris_stream_observe(const JcosU32 *values,JcosU32 count,JcosU64 out[POLARIS_STREAM_WORDS]);
#endif
