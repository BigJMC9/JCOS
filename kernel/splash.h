#ifndef JA_OS_SPLASH_H
#define JA_OS_SPLASH_H

#include "types.h"

void splash_show(void);
void splash_progress(u32 percent);
void splash_status(const char *text);
void splash_finish(void);

#endif