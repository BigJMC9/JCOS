#ifndef JA_OS_BOOT_LOGO_H
#define JA_OS_BOOT_LOGO_H

#include "types.h"

#define BOOT_LOGO_WIDTH  256U
#define BOOT_LOGO_HEIGHT 256U

extern const u32 boot_logo[BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT]
    __attribute__((visibility("hidden")));

#endif
