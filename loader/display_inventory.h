#ifndef JA_OS_LOADER_DISPLAY_INVENTORY_H
#define JA_OS_LOADER_DISPLAY_INVENTORY_H

#include "uefi.h"

void loader_capture_display_inventory(EFI_BOOT_SERVICES *services, BootInfo *boot,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *selected);

#endif
