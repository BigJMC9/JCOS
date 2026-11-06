#ifndef JA_OS_DISPLAY_PROBE_H
#define JA_OS_DISPLAY_PROBE_H

#include "pci.h"

#define DISPLAY_PROBE_CAPACITY 48U

typedef struct {
    u16 vendor_id;
    u16 device_id;
    u16 subsystem_vendor_id;
    u16 subsystem_device_id;
    u16 command;
    u16 status;
    u8 revision;
    u8 subclass;
    u8 prog_if;
    u8 interrupt_line;
    u8 interrupt_pin;
    PciBar bars[PCI_STANDARD_BAR_COUNT];
    bool bar_continuation[PCI_STANDARD_BAR_COUNT];
    u32 rom_bar;
    u8 capability_count;
    u8 capability_offsets[DISPLAY_PROBE_CAPACITY];
    u8 capability_ids[DISPLAY_PROBE_CAPACITY];
    u8 pcie_offset;
    u8 pm_offset;
    u8 msi_offset;
    u8 msix_offset;
    u16 msi_control;
    u16 msix_control;
    bool flr_advertised;
    bool polaris10_candidate;
} DisplayProbeInfo;

/* Pure decoder of a conventional type-0 display-function snapshot.
 * BAR lengths remain unknown: this path never performs sizing writes.
 * Invalid input clears output, including any partially decoded candidates. */
bool display_probe_decode(const u8 *config, u32 size, DisplayProbeInfo *out);

/* Read only standard header fields and capability prefixes used by the
 * decoder. No assignment, BAR mapping/sizing, MMIO, ROM, reset or DMA writes. */
bool display_probe_read(const PciDevice *device, DisplayProbeInfo *out);

/* Firmware provenance, not a BAR-address heuristic. Config mechanism #1
 * covers segment zero only; unknown/nonzero segments never match. */
bool display_probe_matches_gop(const BootInfo *boot, const PciDevice *device);
/* Bind a copied firmware extent to the exact current GOP identity/config.
 * Successful inventory validation does not grant MMIO or mapping authority. */
bool display_probe_firmware_bar(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u32 index, BootDisplayBar *out);
void display_probe_print(const BootInfo *boot);
void display_probe_firmware_print(const BootInfo *boot);

#endif
