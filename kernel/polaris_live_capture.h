#ifndef JA_OS_POLARIS_LIVE_CAPTURE_H
#define JA_OS_POLARIS_LIVE_CAPTURE_H
#include "types.h"
/* Explicit kernel-monitor experiment, compiled off by default. No automatic
 * boot access or native output authority. One attempted capture per boot. */
void polaris_live_capture_run(const BootInfo *boot);
#endif
