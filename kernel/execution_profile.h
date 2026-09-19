#ifndef JA_OS_EXECUTION_PROFILE_H
#define JA_OS_EXECUTION_PROFILE_H

#include "types.h"

/*
 * R4 single-CPU execution profile.
 *
 * Floating-point/vector state is deliberately not exposed yet. CR0.TS stays
 * set so x87/MMX/SSE-family use traps instead of leaking unsaved state across
 * scheduler switches. A later profile may replace this with per-Thread
 * FXSAVE/XSAVE ownership.
 */
bool execution_profile_init(void);
bool execution_profile_fp_simd_restricted(void);

#endif
