/*
 * Excess of comments due to flags and 
 * reminder of unfamiliar ahci practices and
 * data structures.
 */

#include "ahci.h"
#include "pmm.h"
#include "lib.h"
#include "block.h"

/* AHCI Generic Host Control registers. */
#define AHCI_GHC_AE (1U << 31)
/* SATA status register fields. */
#define AHCI_SSTS_DET_MASK 0x0FU
#define AHCI_SSTS_IPM_MASK 0x0F00U
#define AHCI_SSTS_DET_PRESENT 0x03U
#define AHCI_SSTS_IPM_ACTIVE  0x01U
/* Device signatures. */ 
#define AHCI_SIG_ATA    0x00000101U
#define AHCI_SIG_ATAPI  0xEB140101U
#define AHCI_SIG_SEMB   0xC33C0101U
#define AHCI_SIG_PM     0x96690101U
/* PxCMD bits*/
#define AHCI_PXCMD_ST   (1U << 0)
#define AHCI_PXCMD_FRE  (1U << 4)
#define AHCI_PXCMD_FR   (1U << 14)
#define AHCI_PXCMD_CR   (1U << 15)

#define ATA_CMD_READ_DMA_EXT 0x25U
#define ATA_DEVICE_LBA       (1U << 6)

#define AHCI_DMA_PAGE_SIZE   4096U

/* One AHCI port occupies 0x80 bytes. */
typedef struct {
    volatile u32 clb;      /* 0x00 Command list base */
    volatile u32 clbu;     /* 0x04 Command list base upper */
    volatile u32 fb;       /* 0x08 FIS base */
    volatile u32 fbu;      /* 0x0C FIS base upper */

    volatile u32 is;       /* 0x10 Interrupt status */
    volatile u32 ie;       /* 0x14 Interrupt enable */
    volatile u32 cmd;      /* 0x18 Command/status */
    volatile u32 reserved0;

    volatile u32 tfd;      /* 0x20 Task file data */
    volatile u32 sig;      /* 0x24 Signature */
    volatile u32 ssts;     /* 0x28 SATA status */
    volatile u32 sctl;     /* 0x2C SATA control */

    volatile u32 serr;     /* 0x30 SATA error */
    volatile u32 sact;     /* 0x34 SATA active */
    volatile u32 ci;       /* 0x38 Command issue */
    volatile u32 sntf;     /* 0x3C SATA notification */

    volatile u32 fbs;      /* 0x40 FIS switching */
    volatile u32 devslp;   /* 0x44 Device sleep */

    volatile u32 reserved1[10];
    volatile u32 vendor[4];
} AhciHbaPort;

_Static_assert(sizeof(AhciHbaPort) == 0x80, "AHCI port structure must be 0x80 bytes");

typedef struct {
    bool initialized;

    u32 port_number;

    volatile AhciHbaPort *regs;

    u64 command_list_phys;
    u64 fis_phys;
    u64 command_table_phys;

    u64 identify_phys;

    u64 io_buffer_phys;

    u32 logical_sector_size;
    u64 sector_count;

    BlockDevice *block_device;
} AhciPortState;

#define ATA_CMD_IDENTIFY 0xECU

#define FIS_TYPE_REG_H2D 0x27U

#define AHCI_PXIS_TFES (1U << 30)

#define ATA_STATUS_BSY 0x80U
#define ATA_STATUS_DRQ 0x08U

#define AHCI_COMMAND_SLOT 0U

typedef struct __attribute__((packed)) {

    /* Bits 0..4 = Command FIS length in DWORDs. Bit 6 = write direction. */ 
    u16 flags; 

    u16 prdt_length;

    volatile u32 prdbc;

    u32 ctba;
    u32 ctbau;

    u32 reserved[4];
} AhciCommandHeader;

_Static_assert(sizeof(AhciCommandHeader) == 32, "AHCI command header must be 32 bytes");

typedef struct __attribute__((packed)) {
    u32 dba;
    u32 dbau;

    u32 reserved;

    /* Bits 0..21 = byte count minus one. Bit 31 = interrupt on completion. */ 
    u32 dbc_i;
} AhciPrdtEntry;

_Static_assert(sizeof(AhciPrdtEntry) == 16, "AHCI PRDT entry must be 16 bytes");

typedef struct __attribute__((packed)) {
    u8 fis_type;

    /* bit 7 = command/control: 1 = command, 0 = control */
    u8 flags;

    u8 command;
    u8 feature_low;

    u8 lba0;
    u8 lba1;
    u8 lba2;
    u8 device;

    u8 lba3;
    u8 lba4;
    u8 lba5;
    u8 feature_high;

    u8 count_low;
    u8 count_high;

    u8 icc;
    u8 control;

    u8 reserved[4];
} FisRegH2D;

_Static_assert(sizeof(FisRegH2D) == 20, "Register H2D FIS must be 20 bytes");

typedef struct __attribute__((packed)) {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];

    AhciPrdtEntry prdt[1];
} AhciCommandTable;

_Static_assert(sizeof(AhciCommandTable) == 144, "AHCI command table size is unexpected");
/* AHCI memory-mapped HBA registers. Ports begin at offset 0x100. */
typedef struct {
    volatile u32 cap;       /* 0x00 Host capabilities */
    volatile u32 ghc;       /* 0x04 Global host control */
    volatile u32 is;        /* 0x08 Interrupt status */
    volatile u32 pi;        /* 0x0C Ports implemented */
    volatile u32 vs;        /* 0x10 Version */

    volatile u32 ccc_ctl;   /* 0x14 */
    volatile u32 ccc_ports; /* 0x18 */
    volatile u32 em_loc;    /* 0x1C */
    volatile u32 em_ctl;    /* 0x20 */
    volatile u32 cap2;      /* 0x24 */
    volatile u32 bohc;      /* 0x28 */

    u8 reserved[0xA0 - 0x2C];
    u8 vendor[0x100 - 0xA0];

    AhciHbaPort ports[AHCI_MAX_PORTS];
} AhciHbaMemory;

static const PciDevice *g_controller;
static volatile AhciHbaMemory *g_hba;
static AhciInfo g_info;
static AhciPortState g_ports[AHCI_MAX_PORTS];
static u32 g_registered_disks;

bool ahci_port_dma_info(u32 port_number, u64 *command_list, u64 *fis, u64 *command_table, u64 *identify_buffer) {
    if (port_number >= AHCI_MAX_PORTS) return false;

    AhciPortState *state = &g_ports[port_number];

    if (!state->initialized) return false;
    if (command_list) *command_list = state->command_list_phys;
    if (fis) *fis = state->fis_phys;
    if (command_table) *command_table = state->command_table_phys;
    if (identify_buffer) *identify_buffer = state->identify_phys;

    return true;
}

static inline void ahci_dma_barrier(void) {
    __asm__ volatile (
        "mfence"
        :
        :
        : "memory"
    );
}

static void zero_info(void) {
    u8 *bytes = (u8 *)(void *)&g_info;

    for (u32 i = 0; i < sizeof(g_info); ++i) bytes[i] = 0;
}

static const PciDevice *find_controller(void) {
    u32 count = pci_device_count();

    for (u32 i = 0; i < count; ++i) {
        const PciDevice *device = pci_device(i);

        if (!device) continue;
        if (device->class_code != PCI_CLASS_MASS_STORAGE) continue;
        if (device->subclass != PCI_SUBCLASS_SATA) continue;
        if (device->prog_if != PCI_PROGIF_AHCI) continue;
        
        return device;
    }
    return 0;
}

static AhciDeviceType identify_port( volatile AhciHbaPort *port) {
    u32 ssts = port->ssts;
    u32 det = ssts & AHCI_SSTS_DET_MASK; // DET=3 device is physically present + communication established.
    u32 ipm = (ssts & AHCI_SSTS_IPM_MASK) >> 8; // IPM=1 means the interface is active.

    if (det != AHCI_SSTS_DET_PRESENT || ipm != AHCI_SSTS_IPM_ACTIVE) return AHCI_DEVICE_NONE;
    switch (port->sig) {
        case AHCI_SIG_ATA: return AHCI_DEVICE_SATA;
        case AHCI_SIG_ATAPI: return AHCI_DEVICE_SATAPI;
        case AHCI_SIG_SEMB: return AHCI_DEVICE_SEMB;
        case AHCI_SIG_PM: return AHCI_DEVICE_PORT_MULTIPLIER;
        default: return AHCI_DEVICE_UNKNOWN;
    }
}

static bool ahci_stop_port(volatile AhciHbaPort *port) {
    port->cmd &= ~AHCI_PXCMD_ST;

    for (u32 timeout = 0; timeout < 1000000U; ++timeout) {
        if (!(port->cmd & AHCI_PXCMD_CR)) break;
    }

    if (port->cmd & AHCI_PXCMD_CR) return false;
    port->cmd &= ~AHCI_PXCMD_FRE;

    for (u32 timeout = 0; timeout < 1000000U; ++timeout) {
        if (!(port->cmd & AHCI_PXCMD_FR)) return true;
    }
    return false;
}

static bool ahci_start_port(volatile AhciHbaPort *port) {
    for (u32 timeout = 0; timeout < 1000000U; ++timeout) {
        if (!(port->cmd & AHCI_PXCMD_CR)) break;
    }
    if (port->cmd & AHCI_PXCMD_CR) return false;

    port->cmd |= AHCI_PXCMD_FRE; port->cmd |= AHCI_PXCMD_ST;
    return true;
}

static bool ahci_wait_ready(volatile AhciHbaPort *port) {
    if (!port) return false;
    for (u32 timeout = 0; timeout < 1000000U; ++timeout) {

        u32 status = port->tfd & 0xFFU;

        if (!(status & (ATA_STATUS_BSY | ATA_STATUS_DRQ))) return true;
    }
    return false;
}

static bool ahci_allocate_port_memory(AhciPortState *state) {
    if (!state) return false;

    state->command_list_phys = pmm_alloc_page();
    state->fis_phys = pmm_alloc_page();
    state->command_table_phys = pmm_alloc_page();
    state->identify_phys = pmm_alloc_page();
    state->io_buffer_phys = pmm_alloc_page();

    if (!state->command_list_phys || !state->fis_phys || !state->command_table_phys || !state->identify_phys || !state->io_buffer_phys) return false;

    k_memset((void *)(u64)state->command_list_phys, 0, AHCI_DMA_PAGE_SIZE);
    k_memset((void *)(u64)state->fis_phys, 0, AHCI_DMA_PAGE_SIZE);
    k_memset((void *)(u64)state->command_table_phys, 0, AHCI_DMA_PAGE_SIZE);
    k_memset((void *)(u64)state->identify_phys, 0, AHCI_DMA_PAGE_SIZE);
    k_memset((void *)(u64)state->io_buffer_phys, 0, AHCI_DMA_PAGE_SIZE);

    return true;
}

static void ahci_program_port_memory(AhciPortState *state) {
    volatile AhciHbaPort *port = state->regs;

    u64 command_list = state->command_list_phys;
    u64 fis = state->fis_phys;

    /* Command List Base Address */
    port->clb = (u32)(command_list & 0xFFFFFFFFULL); port->clbu = (u32)(command_list >> 32);
    /* Received FIS Base Address */
    port->fb = (u32)(fis & 0xFFFFFFFFULL); port->fbu = (u32)(fis >> 32);
}

static bool ahci_prepare_port(u32 port_number) {
    if (!g_hba || port_number >= AHCI_MAX_PORTS) return false;

    AhciPortState *state = &g_ports[port_number];
    k_memset(state, 0, sizeof(*state));
    state->port_number = port_number; state->regs = &g_hba->ports[port_number];

    if (!ahci_stop_port(state->regs)) return false;
    if (!ahci_allocate_port_memory(state)) return false;

    ahci_program_port_memory(state);
    state->regs->is = 0xFFFFFFFFU; state->regs->serr = 0xFFFFFFFFU;

    if (!ahci_start_port(state->regs)) return false;

    state->initialized = true;
    return true;
}

static void ata_copy_string(char *destination, u32 destination_size, const u16 *words, u32 first_word, u32 word_count) {
    
    if (!destination || destination_size == 0 || !words) return;
    u32 output = 0;

    for (u32 i = 0; i < word_count; ++i) {

        u16 word = words[first_word + i];
        char first = (char)((word >> 8) & 0xFFU);
        char second = (char)(word & 0xFFU);
        if (output + 1 < destination_size) destination[output++] = first;
        if (output + 1 < destination_size) destination[output++] = second;
    }

    /* ATA strings are padded with spaces. */
    while (output > 0 && destination[output - 1] == ' ') --output;

    destination[output] = 0;
}

static void ahci_parse_identify(AhciPortInfo *info, const u16 *words) {
    if (!info || !words) return;

    ata_copy_string(info->serial, sizeof(info->serial), words, 10, 10); ata_copy_string(info->model, sizeof(info->model), words, 27, 20);

    /* ATA IDENTIFY word 83 bit 10: 48-bit LBA supported. */
    info->lba48 = (words[83] & (1U << 10)) != 0;
    if (info->lba48) {
        info->sector_count = ((u64)words[100]) | ((u64)words[101] << 16) | ((u64)words[102] << 32) | ((u64)words[103] << 48);
    }
    else info->sector_count = ((u64)words[60]) | ((u64)words[61] << 16);

    /* Standard ATA logical sector is 512 bytes. */
    info->logical_sector_size = 512U;

    /*
     * Word 106 describes logical-sector sizing.
     *
     * Bit 14 = words 117/118 are valid 
     * Bit 15 = must be zero 
     * Bit 12 = logical sector > 256 words 
     */ 
    u16 word106 = words[106];

    if ((word106 & 0xC000U) == 0x4000U && (word106 & (1U << 12))) {
        u32 logical_words = ((u32)words[117]) | ((u32)words[118] << 16);

        if (logical_words != 0 && logical_words <= 0x7FFFFFFFU) info->logical_sector_size = logical_words * 2U;
    }
}

static bool ahci_identify_port(u32 port_number, AhciPortInfo *info) {
    if (!info || port_number >= AHCI_MAX_PORTS) return false;

    AhciPortState *state = &g_ports[port_number];

    if (!state->initialized || !state->regs) return false;

    volatile AhciHbaPort *port = state->regs;

    /* For now JCOS owns command slot 0 exclusively. */
    u32 slot_mask = 1U << AHCI_COMMAND_SLOT;

    if ((port->ci | port->sact) & slot_mask) return false;

    /* Command list contains 32 headers + Command table for slot zero. */
    AhciCommandHeader *headers = (AhciCommandHeader *)(u64) state->command_list_phys;
    AhciCommandHeader *header = &headers[AHCI_COMMAND_SLOT];
    AhciCommandTable *table = (AhciCommandTable *)(u64) state->command_table_phys;

    /* Clear everything before command. */
    k_memset(header, 0, sizeof(*header)); 
    k_memset(table, 0, sizeof(*table)); 
    k_memset((void *)(u64)state->identify_phys, 0, 512);

    /*
     * Register H2D FIS is 20 bytes:
     *
     * 20 / 4 = 5 DWORDs. 
     * W bit stays clear because the drive is writing data into RAM. 
     */ 
    header->flags = 5U;
    header->prdt_length = 1U; 
    header->ctba = (u32)(state->command_table_phys & 0xFFFFFFFFULL); 
    header->ctbau = (u32)(state->command_table_phys >> 32);

    /* One PRDT entry pointing at 512-byte IDENTIFY DMA buffer. */
    AhciPrdtEntry *prdt = &table->prdt[0];

    prdt->dba = (u32)(state->identify_phys & 0xFFFFFFFFULL); 
    prdt->dbau = (u32)(state->identify_phys >> 32);

    /* DBC stores byte - 1. 512 bytes => 511. Completion interrupts not yet impl. */
    prdt->dbc_i = 511U; 

    FisRegH2D *fis = (FisRegH2D *)(void *) table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;

    /* Bit 7 command bit. */
    fis->flags = (1U << 7); 
    fis->command = ATA_CMD_IDENTIFY;

    /* Clear stale interrupt/error status. */
    port->is = 0xFFFFFFFFU; 
    port->serr = 0xFFFFFFFFU;

    if (!ahci_wait_ready(port)) return false;

    /* Issue command slot zero. */
    ahci_dma_barrier();
    port->ci |= slot_mask;

    /* Poll until hardware clears CI bit zero. */
    for (u32 timeout = 0; timeout < 10000000U; ++timeout) {
        if (port->is & AHCI_PXIS_TFES) return false;
        if (!(port->ci & slot_mask)) break;
    }

    if (port->ci & slot_mask) return false;
    if (port->is & AHCI_PXIS_TFES) return false;

    ahci_dma_barrier();

    const u16 *identify = (const u16 *)(u64) state->identify_phys;

    ahci_parse_identify(info, identify);

    /* A zero-sector disk clearly wasn't identified correctly. */
    if (!info->sector_count) return false;

    return true;
}

static bool ahci_read_one_sector(AhciPortState *state, u64 lba, void *destination) {
    if (!state || !state->initialized || !state->regs || !destination) return false;
    if (!state->logical_sector_size || state->logical_sector_size > AHCI_DMA_PAGE_SIZE) return false;
    if (lba >= state->sector_count) return false;

    volatile AhciHbaPort *port = state->regs;
    u32 slot_mask = 1U << AHCI_COMMAND_SLOT;

    /* Slot zero must not already be in use. */
    if ((port->ci | port->sact) & slot_mask) return false;

    AhciCommandHeader *headers = (AhciCommandHeader *)(u64) state->command_list_phys;
    AhciCommandHeader *header = &headers[AHCI_COMMAND_SLOT];
    AhciCommandTable *table = (AhciCommandTable *)(u64) state->command_table_phys;

    /* Fresh command. */
    k_memset(header, 0, sizeof(*header));
    k_memset(table, 0, sizeof(*table));

    /* Not required, but useful while bringing the driver up. */
    k_memset((void *)(u64)state->io_buffer_phys, 0, state->logical_sector_size);

    
    
    /* Command FIS is 20 bytes = 5 DWORDs. W=0 because it is READ, device -> RAM. */ 
    header->flags = 5U;
    header->prdt_length = 1U;
    header->ctba = (u32)(state->command_table_phys & 0xFFFFFFFFULL);
    header->ctbau = (u32)(state->command_table_phys >> 32);

    /* One PRDT entry points to our bounce buffer. */
    AhciPrdtEntry *prdt = &table->prdt[0];

    prdt->dba = (u32)(state->io_buffer_phys & 0xFFFFFFFFULL);
    prdt->dbau = (u32)(state->io_buffer_phys >> 32);

    prdt->dbc_i = state->logical_sector_size - 1U;

    /* Construct 48-bit ATA READ DMA EXT FIS. */
    FisRegH2D *fis = (FisRegH2D *)(void *) table->cfis;

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->flags = 1U << 7;
    fis->command = ATA_CMD_READ_DMA_EXT;

    /* 48-bit LBA. */
    fis->lba0 = (u8)(lba >> 0);
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    fis->lba3 = (u8)(lba >> 24);
    fis->lba4 = (u8)(lba >> 32);
    fis->lba5 = (u8)(lba >> 40);

    /* Select LBA addressing. */
    fis->device = ATA_DEVICE_LBA;

    /* Exactly one sector. READ DMA EXT uses a 16-bit count. */
    fis->count_low = 1U; fis->count_high = 0U;

    /* Clear stale AHCI status. */
    port->is = 0xFFFFFFFFU;
    port->serr = 0xFFFFFFFFU;

    if (!ahci_wait_ready(port)) return false;

    /* Command structures must become visible before ringing the AHCI command doorbell. */
    ahci_dma_barrier();

    port->ci |= slot_mask;

    /* Poll for completion. */
    for (u32 timeout = 0; timeout < 10000000U; ++timeout) {
        if (port->is & AHCI_PXIS_TFES) return false;
        if (!(port->ci & slot_mask)) break;
    }

    if (port->ci & slot_mask) return false;
    if (port->is & AHCI_PXIS_TFES) return false;

    /* Make sure data written through DMA is visible before the CPU copies it. */
    ahci_dma_barrier();

    k_memcpy(destination, (const void *)(u64) state->io_buffer_phys, state->logical_sector_size);

    return true;
}

static bool ahci_block_read(BlockDevice *device, u64 lba, u32 count, void *buffer) {
    if (!device || !device->driver_data || !buffer || count == 0) return false;

    AhciPortState *state = (AhciPortState *) device->driver_data;

    if (!state->initialized) return false;
    if (device->block_size != state->logical_sector_size) return false;

    /* Avoid offset multiplication overflow. */
    if ((u64)count > (~0ULL / device->block_size)) return false;

    u8 *output = (u8 *)buffer;

    /*
     * First implementation:
     * one AHCI command per sector.
     * Slow but extremely easy to debug.
     */
    for (u32 i = 0; i < count; ++i) {

        u64 current_lba = lba + (u64)i;

        if (current_lba < lba) return false;

        u64 offset = (u64)i * device->block_size;

        if (!ahci_read_one_sector(state, current_lba, output + offset)) return false;
    }

    return true;
}

static bool ahci_register_disk(u32 port_number, AhciPortInfo *info) {
    if (!info || port_number >= AHCI_MAX_PORTS) return false;
    if (!info->identify_ok || !info->sector_count || !info->logical_sector_size) return false;

    /* Our bounce buffer is one 4 KiB page. */
    if (info->logical_sector_size > AHCI_DMA_PAGE_SIZE) return false;

    /* Linux-like sda..sdz naming for now. */
    if (g_registered_disks >= 26U) return false;

    AhciPortState *state = &g_ports[port_number];

    state->logical_sector_size = info->logical_sector_size;
    state->sector_count = info->sector_count;

    char name[4];

    name[0] = 's'; name[1] = 'd'; name[2] = (char)('a' + g_registered_disks); name[3] = 0;

    /*
     * READ-ONLY for now.
     *
     * Haven't implemented WRITE DMA EXT yet,
     * so write callback = NULL. */ 
    BlockDevice *device = block_register(name, BLOCK_DEVICE_AHCI, state->logical_sector_size, state->sector_count, true, ahci_block_read, 0, state);

    if (!device) return false;

    state->block_device = device;

    ++g_registered_disks;

    return true;
}

bool ahci_init(void) {
    g_registered_disks = 0;
    zero_info();
    g_controller = 0; g_hba = 0;
    const PciDevice *controller = find_controller();

    if (!controller) return false;

    /* AHCI ABAR is BAR5 for a standard AHCI controller. */
    PciBar abar;

    if (!pci_read_bar(controller, 5, &abar)) return false;
    if (abar.type != PCI_BAR_MMIO32 && abar.type != PCI_BAR_MMIO64) return false;
    if (!abar.base) return false;

    /*
     * The controller must be allowed to respond to MMIO.
     *
     * Bus mastering isn't required merely to inspect the
     * registers, but it's needed for DMA shortly, so turn
     * it on during controller initialization.
     */
    pci_enable_memory_space(controller); pci_enable_bus_master(controller);

    /*
     * For the current JCOS paging setup, treat the physical
     * MMIO address as directly accessible.
     *
     * Once JCOS owns its page tables, this should become:
     *
     *     g_hba = mmio_map(abar.base, ...);
     */
    volatile AhciHbaMemory *hba = (volatile AhciHbaMemory *)(u64)abar.base;
    /* Enable AHCI mode. */
    hba->ghc |= AHCI_GHC_AE;
    g_controller = controller; g_hba = hba; g_info.pci_address = controller->address; g_info.abar = abar.base; g_info.capabilities = hba->cap; g_info.version = hba->vs; g_info.ports_implemented = hba->pi;
    g_info.hardware_port_count = (hba->cap & 0x1FU) + 1U; /* CAP.NP is zero-based: 0 => 1 port, 5 => 6 ports */

    if (g_info.hardware_port_count > AHCI_MAX_PORTS) {g_info.hardware_port_count = AHCI_MAX_PORTS; }
    for (u32 i = 0; i < AHCI_MAX_PORTS; ++i) {
        AhciPortInfo *info = &g_info.ports[i];

        info->port_number = (u8)i;

        if (!(hba->pi & (1U << i))) continue;

        info->implemented = true;

        volatile AhciHbaPort *port = &hba->ports[i];

        info->signature = port->sig; info->sata_status = port->ssts; info->type = identify_port(port);

        if (info->type == AHCI_DEVICE_NONE) continue;

        info->present = true;

        ++g_info.active_port_count;

        /*
        * For now, only initialize normal SATA disks.
        * Port 2 QEMU is SATAPI, so leave that device alone.
        */
        if (info->type == AHCI_DEVICE_SATA) {
            info->command_engine_ready = ahci_prepare_port(i);

            if (!info->command_engine_ready) continue;

            info->identify_ok = ahci_identify_port(i, info);

            if (!info->identify_ok) continue;

            (void)ahci_register_disk(i, info);
        }
    }
    g_info.initialized = true;
    return true;
}

const AhciInfo *ahci_get(void) {
    return &g_info;
}

const char *ahci_device_type_name(AhciDeviceType type) {
    switch (type) {
        case AHCI_DEVICE_SATA: return "SATA";
        case AHCI_DEVICE_SATAPI: return "SATAPI";
        case AHCI_DEVICE_SEMB: return "SEMB";
        case AHCI_DEVICE_PORT_MULTIPLIER: return "PORT MULTIPLIER";
        case AHCI_DEVICE_UNKNOWN: return "UNKNOWN";
        case AHCI_DEVICE_NONE:
        default: return "NONE";
    }
}