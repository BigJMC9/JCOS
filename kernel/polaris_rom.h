#ifndef JAOS_POLARIS_ROM_H
#define JAOS_POLARIS_ROM_H
#include "types.h"
#include "../include/polaris_rom_protocol.h"
bool polaris_rom_handoff_valid(const BootInfo *boot);
/* Kernel-selected RAM only. Diagnostic borrows RO/NX pages; never frees source. */
bool polaris_rom_inspect(u64 physical,u32 bytes,u32 identity,u64 values[POLARIS_ROM_QUERIES]);
#define POLARIS_ROM_BYTE_WINDOW 256U
/* Fresh isolated RO/NX service reads;caller supplies private256byte destination,cleared on failure. */
bool polaris_rom_copy_bytes(const BootInfo *boot,u64 offset,u32 count,u8 output[POLARIS_ROM_BYTE_WINDOW]);
void polaris_rom_print_bytes(const BootInfo *boot,u64 offset,u32 count);
void polaris_rom_print(const BootInfo *boot);
#endif
