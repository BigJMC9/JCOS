#ifndef JA_OS_POLARIS_WATCH_H
#define JA_OS_POLARIS_WATCH_H
#include "types.h"
void polaris_watch_arm(u32 role,bool spin);
void polaris_watch_begin(const BootInfo *boot);
bool polaris_watch_launch(const BootInfo *boot);
bool polaris_watch_restart(void);
void polaris_watch_rescue(void);
void polaris_watch_diagnostic(u32 role,bool spin);
void polaris_watch_policy_diagnostic(bool spin);
void polaris_watch_status(void);
void polaris_watch_finish(void);
/* Collect completed CPU worker resources only;no active backend poll/GPU IO. */
void polaris_watch_collect(void);
void polaris_watch_present(void);
bool polaris_watch_reconnect(u64 old_output);
bool polaris_watch_power_prepare(void);
#endif
