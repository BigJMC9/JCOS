#ifndef JA_OS_SERIAL_H
#define JA_OS_SERIAL_H

#include "types.h"

bool serial_init(void);
bool serial_available(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_clear(void);
int serial_read_nonblocking(void);

#endif
