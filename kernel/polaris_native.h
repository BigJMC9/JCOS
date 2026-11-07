#ifndef JA_OS_POLARIS_NATIVE_H
#define JA_OS_POLARIS_NATIVE_H
#include "types.h"
/* Private core: runtime callers are serialized by polaris_watch. */
bool polaris_native_terminal(void);
void polaris_native_abort(void);
bool polaris_native_can_diagnostic(u32 role);
/* Off-monitor diagnostics never initiate a second console IPC client. */
void polaris_native_background_diagnostic(u32 role,bool spin);
void polaris_native_background_policy_diagnostic(bool spin);
void polaris_native_policy_diagnostic(bool spin);
void polaris_native_begin(const BootInfo *boot);
void polaris_native_rescue(void);
void polaris_native_finish(void);
void polaris_native_poll(void);
/* One acknowledged diagnostic per active primary/rescue incarnation. */
void polaris_native_diagnostic(u32 role,bool spin);
void polaris_native_status(void);
/* No GPU IO: stop CPU owners,retain quarantine,drop only supervisor capability. */
bool polaris_native_power_prepare(void);
#endif
