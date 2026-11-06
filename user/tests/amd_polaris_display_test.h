#ifndef JCOS_AMD_POLARIS_DISPLAY_TEST_H
#define JCOS_AMD_POLARIS_DISPLAY_TEST_H
#include "../../include/user_abi.h"
#define POLARIS_DISPLAY_MODEL_VERSION 1U
#define POLARIS_DISPLAY_MODEL_EXPECTED 0x3FU
/* Synthetic-reader diagnostics only. No physical MMIO/PIO callback exists. */
JcosU32 polaris_display_model_test(void);
#endif
