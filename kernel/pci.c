#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8U
#define PCI_CONFIG_DATA    0xCFCU

#define PCI_VENDOR_ID_OFFSET      0x00U
#define PCI_DEVICE_ID_OFFSET      0x02U
#define PCI_COMMAND_OFFSET        0x04U
#define PCI_REVISION_OFFSET       0x08U
#define PCI_PROG_IF_OFFSET        0x09U
#define PCI_SUBCLASS_OFFSET       0x0AU
#define PCI_CLASS_OFFSET          0x0BU
#define PCI_HEADER_TYPE_OFFSET    0x0EU
#define PCI_BAR0_OFFSET           0x10U
#define PCI_INTERRUPT_LINE_OFFSET 0x3CU
#define PCI_INTERRUPT_PIN_OFFSET  0x3DU

static PciDevice g_devices[PCI_MAX_DEVICES];
static u32 g_device_count;

/*
 * Keep the actual x86 port-I/O instructions private to this
 * backend for now.
 *
 * Later move to arch.c/arch.asm if desired.
 */

static inline void io_out32(u16 port, u32 value) {
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static inline u32 io_in32(u16 port) {
    u32 value;

    __asm__ volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"(port)
        : "memory"
    );

    return value;
}

static u32 config_address(PciAddress address, u16 offset) {
    return
        0x80000000U |
        ((u32)address.bus << 16) |
        ((u32)address.device << 11) |
        ((u32)address.function << 8) |
        ((u32)offset & 0xFCU);
}

u32 pci_config_read32(PciAddress address, u16 offset) {
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, offset));

    return io_in32(PCI_CONFIG_DATA);
}

u16 pci_config_read16(PciAddress address, u16 offset) {
    u32 value = pci_config_read32(address, offset);
    u32 shift = ((u32)offset & 2U) * 8U;

    return (u16)((value >> shift) & 0xFFFFU);
}

u8 pci_config_read8(PciAddress address, u16 offset) {
    u32 value = pci_config_read32(address, offset);
    u32 shift = ((u32)offset & 3U) * 8U;

    return (u8)((value >> shift) & 0xFFU);
}

void pci_config_write32(PciAddress address, u16 offset, u32 value) {
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, offset));
    io_out32(PCI_CONFIG_DATA, value);
}

void pci_config_write16(PciAddress address, u16 offset, u16 value) {
    u16 aligned = offset & (u16)~3U;
    u32 original = pci_config_read32(address, aligned);
    u32 shift = ((u32)offset & 2U) * 8U;
    u32 mask = 0xFFFFU << shift;
    u32 updated = (original & ~mask) | ((u32)value << shift);

    pci_config_write32(address, aligned, updated);
}

void pci_config_write8(PciAddress address, u16 offset, u8 value) {
    u16 aligned = offset & (u16)~3U;
    u32 original = pci_config_read32(address, aligned);
    u32 shift = ((u32)offset & 3U) * 8U;
    u32 mask = 0xFFU << shift;
    u32 updated = (original & ~mask) | ((u32)value << shift);

    pci_config_write32(address, aligned, updated);
}

static bool function_exists(PciAddress address) {
    return
        pci_config_read16(address, PCI_VENDOR_ID_OFFSET) != 0xFFFFU;
}

static void record_function(PciAddress address) {
    if (g_device_count >= PCI_MAX_DEVICES) return;

    u16 vendor = pci_config_read16(address, PCI_VENDOR_ID_OFFSET);

    if (vendor == 0xFFFFU) return;

    PciDevice *device = &g_devices[g_device_count];

    device->address = address;
    device->vendor_id = vendor;
    device->device_id = pci_config_read16(address, PCI_DEVICE_ID_OFFSET);
    device->revision = pci_config_read8(address, PCI_REVISION_OFFSET);
    device->prog_if = pci_config_read8(address, PCI_PROG_IF_OFFSET);
    device->subclass = pci_config_read8(address, PCI_SUBCLASS_OFFSET);
    device->class_code = pci_config_read8(address, PCI_CLASS_OFFSET);
    device->header_type = pci_config_read8(address, PCI_HEADER_TYPE_OFFSET);
    device->interrupt_line = pci_config_read8(address, PCI_INTERRUPT_LINE_OFFSET);
    device->interrupt_pin = pci_config_read8(address, PCI_INTERRUPT_PIN_OFFSET);

    ++g_device_count;
}

void pci_init(void) {
    g_device_count = 0;

    for (u32 bus = 0; bus < 256U; ++bus) {
        for (u32 device = 0; device < 32U; ++device) {
            PciAddress address;

            address.bus = (u8)bus;
            address.device = (u8)device;
            address.function = 0;

            if (!function_exists(address)) continue;

            u8 header_type = pci_config_read8(address, PCI_HEADER_TYPE_OFFSET);

            /* Function zero always exists here. */
            record_function(address);

            /* Bit 7 = multifunction device. */
            if (!(header_type & 0x80U)) continue;
            for (u32 function = 1; function < 8U; ++function) {

                address.function = (u8)function;

                if (function_exists(address)) record_function(address);
            }
        }
    }
}

u32 pci_device_count(void) {
    return g_device_count;
}

const PciDevice *pci_device(u32 index) {
    if (index >= g_device_count) return 0;

    return &g_devices[index];
}

const PciDevice *pci_find_class(u8 class_code, u8 subclass, u32 ordinal) {
    u32 found = 0;

    for (u32 i = 0; i < g_device_count; ++i) {

        const PciDevice *device = &g_devices[i];

        if (device->class_code != class_code) continue;
        if (device->subclass != subclass) continue;
        if (found == ordinal) return device;

        ++found;
    }

    return 0;
}

bool pci_read_bar(const PciDevice *device, u8 index, PciBar *out) {
    if (!device || !out) return false;

    /* Type-zero headers have six BARs. */
    if ((device->header_type & 0x7FU) != 0) return false;
    if (index >= 6U) return false;

    u16 offset = PCI_BAR0_OFFSET + (u16)index * 4U;
    u32 low = pci_config_read32(device->address, offset);

    out->index = index;
    out->prefetchable = false;
    out->base = 0;
    out->type = PCI_BAR_NONE;

    if (low == 0) return true;

    /* I/O BAR. */
    if (low & 1U) {
        out->type = PCI_BAR_IO;
        out->base = (u64)(low & ~3U);

        return true;
    }

    u32 memory_type = (low >> 1) & 3U;

    out->prefetchable = (low & (1U << 3)) != 0;

    if (memory_type == 0U) {
        out->type = PCI_BAR_MMIO32;
        out->base = (u64)(low & ~0xFU);

        return true;
    }

    if (memory_type == 2U) {
        if (index >= 5U) return false;

        u32 high = pci_config_read32(device->address, offset + 4U);

        out->type = PCI_BAR_MMIO64;
        out->base = ((u64)high << 32) | (u64)(low & ~0xFU);

        return true;
    }

    /* Reserved memory BAR encoding. */
    return false;
}

static void enable_command_bit(const PciDevice *device, u16 bit) {
    if (!device) return;

    u16 command = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    command |= bit;
    pci_config_write16(device->address, PCI_COMMAND_OFFSET, command);
}

void pci_enable_memory_space(const PciDevice *device) {
    enable_command_bit(device, PCI_COMMAND_MEMORY_SPACE);
}

void pci_enable_io_space(const PciDevice *device) {
    enable_command_bit(device, PCI_COMMAND_IO_SPACE);
}

void pci_enable_bus_master(const PciDevice *device) {
    enable_command_bit(device, PCI_COMMAND_BUS_MASTER);
}