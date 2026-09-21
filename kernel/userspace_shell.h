#ifndef JA_OS_USERSPACE_SHELL_H
#define JA_OS_USERSPACE_SHELL_H

#include "types.h"

/* R7 normal-shell handoff: the kernel keeps the privileged hardware input
 * poller but forwards ordinary key events to the Ring3 console/shell policy
 * service. Escape or the userspace `monitor` command returns to the kernel
 * emergency/development monitor. */
bool userspace_shell_run(void);

/* Boot policy wrapper. A successful activation counts as the default-shell
 * handoff even though this call later returns when the operator requests the
 * emergency monitor. */
bool userspace_shell_run_boot_default(void);
u64 userspace_shell_session_count(void);
u64 userspace_shell_boot_handoff_count(void);

#endif