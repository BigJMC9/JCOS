#ifndef JA_OS_TIMER_H
#define JA_OS_TIMER_H

#include "types.h"

/*
 * v1 timer:
 *
 *   8254 PIT channel 0
 *   legacy IRQ0
 *   IDT vector 0x20
 *
 * Initialization currently requires the
 * 8259 PIC fallback controller.
 */
bool timer_init(u32 frequency_hz);

bool timer_initialized(void);
u32 timer_frequency(void);
u64 timer_ticks(void);

/* Called from interrupt_dispatch() for vector 0x20. */
void timer_handle_irq(void);

#endif