#ifndef JA_OS_PCI_FLR_H
#define JA_OS_PCI_FLR_H

#include "pci.h"

#define PCI_CAPABILITY_ID_EXPRESS 0x10U

typedef enum {
    PCI_FLR_RESULT_NOT_ATTEMPTED = 0,
    PCI_FLR_RESULT_COMPLETE,
    PCI_FLR_RESULT_FAILED
} PciFlrResult;

/* Walk the conventional PCI capability list with loop/range validation. */
bool pci_find_capability(const PciDevice *device, u8 capability_id,
    u8 *out_offset);

/* FLR support is advertised by the PCI Express Device Capabilities register. */
bool pci_flr_supported(const PciDevice *device, u8 *out_capability_offset);

/* Kernel-internal reset primitive. The caller must already own a quarantined,
 * CPU-quiesced function with I/O, memory and bus-master decode disabled and
 * INTx disabled. NOT_ATTEMPTED guarantees the FLR Initiate bit was not written;
 * FAILED means reset initiation occurred but post-reset verification failed. */
PciFlrResult pci_flr_execute_quiesced(const PciDevice *device,
    u16 *out_command_after);

#endif
