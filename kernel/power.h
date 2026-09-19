#ifndef JA_OS_POWER_H
#define JA_OS_POWER_H

#include "types.h"

typedef enum {
    POWER_RESULT_OK = 0,
    POWER_RESULT_BUSY,
    POWER_RESULT_UNSUPPORTED,
    POWER_RESULT_BAD_CONTEXT,
    POWER_RESULT_RUNNABLE_STATE,
    POWER_RESULT_OBJECT_STATE,
    POWER_RESULT_STORAGE_UNSAFE
} PowerResult;

PowerResult power_check_shutdown(void);
PowerResult power_check_reboot(void);
PowerResult power_shutdown(void);
PowerResult power_reboot(void);
bool power_storage_safe(void);
const char *power_result_name(PowerResult result);

#endif
