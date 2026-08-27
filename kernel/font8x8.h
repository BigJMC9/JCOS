#ifndef JA_OS_FONT8X8_H
#define JA_OS_FONT8X8_H

#include "types.h"

#define FONT8X8_WIDTH  8U
#define FONT8X8_HEIGHT 8U

extern const u8 font8x8[128][8]
    __attribute__((visibility("hidden")));

#endif