#ifndef JA_OS_DEVICE_MMIO_H
#define JA_OS_DEVICE_MMIO_H
#include "types.h"
/* Privileged x86 UC-load primitive. Caller validates/pins the exact mapping and
 * semantic operation. No timeout, device authority or DMA proof is implied. */
bool device_mmio_interrupts_enabled(void);
u32 device_mmio_load32(u64 address);
void device_mmio_store32(u64 address,u32 value);
#endif
