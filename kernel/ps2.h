#ifndef JA_OS_PS2_H
#define JA_OS_PS2_H

#include "types.h"
#include "key_event.h"

bool ps2_init(void);
void ps2_handle_irq(void);
void ps2_poll(void);
bool ps2_get_event(KeyEvent *event);

/* Compatibility byte interface for code that only accepts text input. */
int ps2_getchar(void);

bool ps2_present(void);
u64 ps2_irq_count(void);
u64 ps2_scancode_count(void);
u64 ps2_dropped_count(void);

#endif
