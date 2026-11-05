#include "pci.h"

#include "interrupts.h"
#include "lib.h"

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

#define PCI_RFLAGS_IF (1ULL << 9)

static PciDevice g_devices[PCI_MAX_DEVICES];
static u32 g_device_count;

/* Mechanism #1 uses one shared address/data pair. Keep every transaction under
 * local IRQ exclusion in the current single-CPU profile so unrelated config
 * accesses cannot interleave between 0xCF8 and 0xCFC. */
static u64 pci_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void pci_irq_restore(u64 flags) {
    if (flags & PCI_RFLAGS_IF) interrupts_enable();
}

static inline void io_out32(u16 port, u32 value) {
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static inline void io_out16(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void io_out8(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline u32 io_in32(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline u16 io_in16(u16 port) {
    u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline u8 io_in8(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

bool pci_address_valid(PciAddress address) {
    return address.device < 32U && address.function < 8U;
}

static bool config_access_valid(PciAddress address, u16 offset, u16 width) {
    if (!pci_address_valid(address) || !width ||
        (width != 1U && width != 2U && width != 4U)) return false;
    if (offset >= PCI_CONFIG_SPACE_SIZE ||
        (u32)offset + (u32)width > PCI_CONFIG_SPACE_SIZE) return false;
    if (((u32)offset & 3U) + width > 4U) return false;
    return width != 4U || ((offset & 3U) == 0U);
}

static u32 config_address(PciAddress address, u16 aligned_offset) {
    return 0x80000000U |
        ((u32)address.bus << 16) |
        ((u32)address.device << 11) |
        ((u32)address.function << 8) |
        (u32)aligned_offset;
}

static u32 config_read32_locked(PciAddress address, u16 aligned_offset) {
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned_offset));
    return io_in32(PCI_CONFIG_DATA);
}

static u16 config_read16_locked(PciAddress address, u16 offset) {
    u16 aligned = offset & (u16)~3U;
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned));
    return io_in16((u16)(PCI_CONFIG_DATA + (offset & 3U)));
}

static u8 config_read8_locked(PciAddress address, u16 offset) {
    u16 aligned = offset & (u16)~3U;
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned));
    return io_in8((u16)(PCI_CONFIG_DATA + (offset & 3U)));
}

static void config_write32_locked(PciAddress address, u16 aligned_offset, u32 value) {
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned_offset));
    io_out32(PCI_CONFIG_DATA, value);
}

static void config_write16_locked(PciAddress address, u16 offset, u16 value) {
    u16 aligned = offset & (u16)~3U;
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned));
    io_out16((u16)(PCI_CONFIG_DATA + (offset & 3U)), value);
}

static void config_write8_locked(PciAddress address, u16 offset, u8 value) {
    u16 aligned = offset & (u16)~3U;
    io_out32(PCI_CONFIG_ADDRESS, config_address(address, aligned));
    io_out8((u16)(PCI_CONFIG_DATA + (offset & 3U)), value);
}

u32 pci_config_read32(PciAddress address, u16 offset) {
    if (!config_access_valid(address, offset, 4U)) return 0xFFFFFFFFU;
    u64 flags = pci_irq_save();
    u32 value = config_read32_locked(address, offset);
    pci_irq_restore(flags);
    return value;
}

u16 pci_config_read16(PciAddress address, u16 offset) {
    if (!config_access_valid(address, offset, 2U)) return 0xFFFFU;
    u64 flags = pci_irq_save();
    u16 value = config_read16_locked(address, offset);
    pci_irq_restore(flags);
    return value;
}

u8 pci_config_read8(PciAddress address, u16 offset) {
    if (!config_access_valid(address, offset, 1U)) return 0xFFU;
    u64 flags = pci_irq_save();
    u8 value = config_read8_locked(address, offset);
    pci_irq_restore(flags);
    return value;
}

void pci_config_write32(PciAddress address, u16 offset, u32 value) {
    if (!config_access_valid(address, offset, 4U)) return;
    u64 flags = pci_irq_save();
    config_write32_locked(address, offset, value);
    pci_irq_restore(flags);
}

void pci_config_write16(PciAddress address, u16 offset, u16 value) {
    if (!config_access_valid(address, offset, 2U)) return;
    u64 flags = pci_irq_save();
    config_write16_locked(address, offset, value);
    pci_irq_restore(flags);
}

void pci_config_write8(PciAddress address, u16 offset, u8 value) {
    if (!config_access_valid(address, offset, 1U)) return;
    u64 flags = pci_irq_save();
    config_write8_locked(address, offset, value);
    pci_irq_restore(flags);
}

static bool function_exists(PciAddress address) {
    return pci_config_read16(address, PCI_VENDOR_ID_OFFSET) != 0xFFFFU;
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
            record_function(address);

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

bool pci_device_is_enumerated(const PciDevice *device) {
    if (!device) return false;
    for (u32 i = 0U; i < g_device_count; ++i)
        if (&g_devices[i] == device) return true;
    return false;
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

static bool bar_index_is_continuation_locked(const PciDevice *device, u8 index) {
    if (!device || !index) return false;
    for (u8 current = 0U; current < index; ++current) {
        u16 offset = PCI_BAR0_OFFSET + (u16)current * 4U;
        u32 low = config_read32_locked(device->address, offset);
        if (!(low & 1U) && (((low >> 1) & 3U) == 2U)) {
            if ((u8)(current + 1U) == index) return true;
            ++current;
        }
    }
    return false;
}

static bool read_bar_locked(const PciDevice *device, u8 index, PciBar *out) {
    if (!device || !out || (device->header_type & 0x7FU) != 0U ||
        index >= PCI_STANDARD_BAR_COUNT || bar_index_is_continuation_locked(device, index))
        return false;

    k_memset(out, 0, sizeof(*out));
    out->index = index;

    u16 offset = PCI_BAR0_OFFSET + (u16)index * 4U;
    u32 low = config_read32_locked(device->address, offset);
    if (!low) return true;

    if (low & 1U) {
        out->type = PCI_BAR_IO;
        out->base = (u64)(low & ~3U);
        return true;
    }

    u32 memory_type = (low >> 1) & 3U;
    out->prefetchable = (low & (1U << 3)) != 0U;

    if (memory_type == 0U) {
        out->type = PCI_BAR_MMIO32;
        out->base = (u64)(low & ~0xFU);
        return true;
    }

    if (memory_type == 2U) {
        if (index >= PCI_STANDARD_BAR_COUNT - 1U) return false;
        u32 high = config_read32_locked(device->address, offset + 4U);
        out->type = PCI_BAR_MMIO64;
        out->base = ((u64)high << 32) | (u64)(low & ~0xFU);
        return true;
    }

    return false;
}

bool pci_read_bar(const PciDevice *device, u8 index, PciBar *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!pci_device_is_enumerated(device)) return false;

    u64 flags = pci_irq_save();
    bool result = read_bar_locked(device, index, out);
    pci_irq_restore(flags);
    return result;
}

bool pci_bar_decode_size(PciBarType type, u32 low_mask, u32 high_mask, u64 *out_size) {
    if (!out_size) return false;
    *out_size = 0ULL;

    u64 size = 0ULL;
    if (type == PCI_BAR_IO) {
        u32 mask = low_mask & ~3U;
        if (!mask) return false;
        size = (u64)((~mask) + 1U);
    } else if (type == PCI_BAR_MMIO32) {
        u32 mask = low_mask & ~0xFU;
        if (!mask) return false;
        size = (u64)((~mask) + 1U);
    } else if (type == PCI_BAR_MMIO64) {
        u64 mask = ((u64)high_mask << 32) | (u64)(low_mask & ~0xFU);
        size = (~mask) + 1ULL;
    } else {
        return false;
    }

    if (!size || (size & (size - 1ULL))) return false;
    *out_size = size;
    return true;
}

bool pci_probe_bar(const PciDevice *device, u8 index, PciBar *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!pci_device_is_enumerated(device) ||
        (device->header_type & 0x7FU) != 0U || index >= PCI_STANDARD_BAR_COUNT)
        return false;

    u64 flags = pci_irq_save();
    bool result = false;
    if (bar_index_is_continuation_locked(device, index)) goto done;

    u16 command = config_read16_locked(device->address, PCI_COMMAND_OFFSET);
    if (command & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER))
        goto done;

    u16 offset = PCI_BAR0_OFFSET + (u16)index * 4U;
    u32 original_low = config_read32_locked(device->address, offset);
    PciBarType type = PCI_BAR_NONE;
    bool prefetchable = false;
    u32 original_high = 0U;

    if (original_low & 1U) {
        type = PCI_BAR_IO;
    } else {
        u32 memory_type = (original_low >> 1) & 3U;
        prefetchable = (original_low & (1U << 3)) != 0U;
        if (memory_type == 0U) {
            type = PCI_BAR_MMIO32;
        } else if (memory_type == 2U && index < PCI_STANDARD_BAR_COUNT - 1U) {
            type = PCI_BAR_MMIO64;
            original_high = config_read32_locked(device->address, offset + 4U);
        } else {
            goto done;
        }
    }

    config_write32_locked(device->address, offset, 0xFFFFFFFFU);
    if (type == PCI_BAR_MMIO64)
        config_write32_locked(device->address, offset + 4U, 0xFFFFFFFFU);

    u32 probe_low = config_read32_locked(device->address, offset);
    u32 probe_high = type == PCI_BAR_MMIO64 ?
        config_read32_locked(device->address, offset + 4U) : 0U;

    if (type == PCI_BAR_MMIO64)
        config_write32_locked(device->address, offset + 4U, original_high);
    config_write32_locked(device->address, offset, original_low);

    u64 size = 0ULL;
    if (!pci_bar_decode_size(type, probe_low, probe_high, &size)) goto done;

    u64 base = 0ULL;
    if (type == PCI_BAR_IO) {
        base = (u64)(original_low & ~3U);
    } else if (type == PCI_BAR_MMIO32) {
        base = (u64)(original_low & ~0xFU);
    } else {
        base = ((u64)original_high << 32) | (u64)(original_low & ~0xFU);
    }
    if (base && (base & (size - 1ULL))) goto done;

    out->type = type;
    out->index = index;
    out->prefetchable = prefetchable;
    out->base = base;
    out->size = size;
    result = true;

done:
    pci_irq_restore(flags);
    return result;
}

static void enable_command_bit(const PciDevice *device, u16 bit) {
    if (!pci_device_is_enumerated(device)) return;

    u64 flags = pci_irq_save();
    u16 command = config_read16_locked(device->address, PCI_COMMAND_OFFSET);
    command |= bit;
    config_write16_locked(device->address, PCI_COMMAND_OFFSET, command);
    pci_irq_restore(flags);
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