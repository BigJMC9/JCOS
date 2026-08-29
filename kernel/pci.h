#ifndef JA_OS_PCI_H
#define JA_OS_PCI_H

#include "types.h"

#define PCI_MAX_DEVICES 512U

#define PCI_CLASS_MASS_STORAGE 0x01U

#define PCI_SUBCLASS_SATA      0x06U
#define PCI_SUBCLASS_NVM       0x08U

#define PCI_PROGIF_AHCI        0x01U
#define PCI_PROGIF_NVME        0x02U

#define PCI_COMMAND_IO_SPACE      (1U << 0)
#define PCI_COMMAND_MEMORY_SPACE  (1U << 1)
#define PCI_COMMAND_BUS_MASTER    (1U << 2)
#define PCI_COMMAND_INT_DISABLE   (1U << 10)

typedef struct {
    u8 bus;
    u8 device;
    u8 function;
} PciAddress;

typedef enum {
    PCI_BAR_NONE = 0,
    PCI_BAR_IO,
    PCI_BAR_MMIO32,
    PCI_BAR_MMIO64
} PciBarType;

typedef struct {
    PciBarType type;
    u8 index;
    bool prefetchable;
    u64 base;
} PciBar;

typedef struct {
    PciAddress address;

    u16 vendor_id;
    u16 device_id;

    u8 revision;
    u8 prog_if;
    u8 subclass;
    u8 class_code;

    u8 header_type;
    u8 interrupt_line;
    u8 interrupt_pin;
} PciDevice;

void pci_init(void);

u32 pci_device_count(void);

const PciDevice *pci_device(u32 index);

const PciDevice *pci_find_class(
    u8 class_code,
    u8 subclass,
    u32 ordinal
);

u8 pci_config_read8(
    PciAddress address,
    u16 offset
);

u16 pci_config_read16(
    PciAddress address,
    u16 offset
);

u32 pci_config_read32(
    PciAddress address,
    u16 offset
);

void pci_config_write8(
    PciAddress address,
    u16 offset,
    u8 value
);

void pci_config_write16(
    PciAddress address,
    u16 offset,
    u16 value
);

void pci_config_write32(
    PciAddress address,
    u16 offset,
    u32 value
);

bool pci_read_bar(
    const PciDevice *device,
    u8 index,
    PciBar *out
);

void pci_enable_memory_space(
    const PciDevice *device
);

void pci_enable_io_space(
    const PciDevice *device
);

void pci_enable_bus_master(
    const PciDevice *device
);

#endif