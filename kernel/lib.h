#ifndef JA_OS_LIB_H
#define JA_OS_LIB_H

#include "types.h"

void *k_memset(void *dst, u8 value, usize count);
void *k_memcpy(void *dst, const void *src, usize count);
usize k_strlen(const char *s);
bool k_streq(const char *a, const char *b);
bool k_strieq(const char *a, const char *b);
char k_ascii_lower(char c);
bool k_ascii_space(char c);

#endif
