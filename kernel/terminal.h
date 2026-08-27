#ifndef JA_OS_TERMINAL_H
#define JA_OS_TERMINAL_H

#include "types.h"

bool terminal_init(void);
void terminal_clear(void);
void terminal_set_color(u32 color);
u32 terminal_default_color(void);
u32 terminal_accent_color(void);
u32 terminal_error_color(void);
void terminal_putchar(char c);
void terminal_write(const char *s);
void terminal_writeln(const char *s);
void terminal_write_u64(u64 value);
void terminal_write_hex(u64 value);

#endif
