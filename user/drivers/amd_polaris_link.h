#ifndef JCOS_AMD_POLARIS_LINK_H
#define JCOS_AMD_POLARIS_LINK_H
#include "../../include/polaris_link_protocol.h"
/* Clears all report words on refusal except status in word0. */
int polaris_link_observe(const JcosU32 *values,JcosU32 count,JcosU64 report[POLARIS_LINK_WORDS]);
#endif