#ifndef JA_OS_AHCI_H
#define JA_OS_AHCI_H

#include "types.h"
#include "pci.h"

#define AHCI_MAX_PORTS 32U

typedef enum {
    AHCI_DEVICE_NONE = 0,
    AHCI_DEVICE_SATA,
    AHCI_DEVICE_SATAPI,
    AHCI_DEVICE_SEMB,
    AHCI_DEVICE_PORT_MULTIPLIER,
    AHCI_DEVICE_UNKNOWN
} AhciDeviceType;

typedef struct {
    u8 port_number;

    bool implemented;
    bool present;
    bool command_engine_ready;
    bool identify_ok;
    bool lba48;

    AhciDeviceType type;

    u32 signature;
    u32 sata_status;

    u32 logical_sector_size;
    u64 sector_count;

    char model[41];
    char serial[21];
} AhciPortInfo;

typedef struct {
    bool initialized;

    PciAddress pci_address;

    u64 abar;

    u32 capabilities;
    u32 version;
    u32 ports_implemented;

    u32 hardware_port_count;
    u32 active_port_count;

    AhciPortInfo ports[AHCI_MAX_PORTS];
} AhciInfo;

/* Return device-visible physical DMA addresses. */
bool ahci_port_dma_info(
    u32 port_number,
    u64 *command_list,
    u64 *fis,
    u64 *command_table,
    u64 *identify_buffer
);

bool ahci_init(void);

const AhciInfo *ahci_get(void);
const char *ahci_device_type_name( AhciDeviceType type);


/*
 * Switch CPU-side DMA-buffer access to the physmap.
 * Device-facing DMA addresses remain physical.
 */
bool ahci_enable_phys_map_access(void);
bool ahci_phys_map_access_enabled(void);

#endif