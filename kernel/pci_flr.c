#include "pci_flr.h"

#include "arch.h"
#include "scheduler.h"
#include "timer.h"

#define PCI_STATUS_OFFSET              0x06U
#define PCI_STATUS_CAPABILITIES_LIST   (1U << 4)
#define PCI_CAPABILITY_POINTER_OFFSET  0x34U
#define PCI_CAPABILITY_MIN_OFFSET      0x40U
#define PCI_CAPABILITY_MAX_OFFSET      0xFCU
#define PCI_CAPABILITY_WALK_LIMIT      48U

#define PCI_EXP_DEVICE_CAPABILITIES    0x04U
#define PCI_EXP_DEVICE_CONTROL         0x08U
#define PCI_EXP_DEVICE_STATUS          0x0AU
#define PCI_EXP_DEVICE_CAP_FLR         (1U << 28)
#define PCI_EXP_DEVICE_CONTROL_FLR     (1U << 15)
#define PCI_EXP_DEVICE_STATUS_PENDING  (1U << 5)

#define PCI_COMMAND_OFFSET             0x04U
#define PCI_VENDOR_ID_OFFSET           0x00U
#define PCI_DEVICE_ID_OFFSET           0x02U
#define PCI_FLR_MINIMUM_WAIT_MS        100U
#define PCI_FLR_READY_TIMEOUT_MS       1000U
#define PCI_FLR_RFLAGS_IF              (1ULL << 9)

static bool capability_offset_valid(u8 offset) {
    return offset >= PCI_CAPABILITY_MIN_OFFSET &&
        offset <= PCI_CAPABILITY_MAX_OFFSET && !(offset & 3U);
}

static bool capability_seen(u64 visited[4], u8 offset) {
    u32 word = (u32)offset >> 6;
    u32 bit = (u32)offset & 63U;
    u64 mask = 1ULL << bit;
    if (visited[word] & mask) return true;
    visited[word] |= mask;
    return false;
}

bool pci_find_capability(const PciDevice *device, u8 capability_id,
    u8 *out_offset) {
    if (out_offset) *out_offset = 0U;
    if (!out_offset || !capability_id || !pci_device_is_enumerated(device) ||
        (device->header_type & 0x7FU) > 1U) return false;

    u16 status = pci_config_read16(device->address, PCI_STATUS_OFFSET);
    if (status == 0xFFFFU || !(status & PCI_STATUS_CAPABILITIES_LIST)) return false;

    u8 pointer = pci_config_read8(device->address,
        PCI_CAPABILITY_POINTER_OFFSET);
    if (!pointer) return false;

    u64 visited[4] = {0ULL, 0ULL, 0ULL, 0ULL};
    for (u32 step = 0U; step < PCI_CAPABILITY_WALK_LIMIT; ++step) {
        if (!capability_offset_valid(pointer) || capability_seen(visited, pointer))
            return false;

        u8 id = pci_config_read8(device->address, pointer);
        u8 next = pci_config_read8(device->address, (u16)pointer + 1U);
        if (id == 0xFFU || next == 0xFFU) return false;
        if (id == capability_id) {
            *out_offset = pointer;
            return true;
        }
        if (!next) return false;
        pointer = next;
    }
    return false;
}

bool pci_flr_supported(const PciDevice *device, u8 *out_capability_offset) {
    if (out_capability_offset) *out_capability_offset = 0U;
    if (!out_capability_offset) return false;

    u8 capability = 0U;
    if (!pci_find_capability(device, PCI_CAPABILITY_ID_EXPRESS, &capability) ||
        capability > 0xF4U) return false;

    u32 device_capabilities = pci_config_read32(device->address,
        (u16)capability + PCI_EXP_DEVICE_CAPABILITIES);
    if (device_capabilities == 0xFFFFFFFFU ||
        !(device_capabilities & PCI_EXP_DEVICE_CAP_FLR)) return false;

    *out_capability_offset = capability;
    return true;
}

static bool interrupts_enabled(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return (flags & PCI_FLR_RFLAGS_IF) != 0ULL;
}

static bool milliseconds_to_ticks(u32 milliseconds, u64 *out_ticks) {
    if (!out_ticks || !milliseconds || !timer_initialized()) return false;
    u32 frequency = timer_frequency();
    if (!frequency || (u64)frequency > (~0ULL - 999ULL) / milliseconds)
        return false;
    u64 scaled = (u64)frequency * milliseconds;
    u64 ticks = (scaled + 999ULL) / 1000ULL;
    *out_ticks = ticks ? ticks : 1ULL;
    return true;
}

static bool wait_ticks(u64 ticks) {
    if (!ticks || !timer_initialized() || !interrupts_enabled()) return false;
    u64 started = timer_ticks();
    while ((u64)(timer_ticks() - started) < ticks) {
        if (!scheduler_yield()) arch_pause();
    }
    return true;
}

static bool wait_transaction_clear(const PciDevice *device, u8 capability) {
    u64 timeout = 0ULL;
    if (!milliseconds_to_ticks(PCI_FLR_READY_TIMEOUT_MS, &timeout)) return false;
    u64 started = timer_ticks();
    for (;;) {
        u16 status = pci_config_read16(device->address,
            (u16)capability + PCI_EXP_DEVICE_STATUS);
        if (status == 0xFFFFU) return false;
        if (!(status & PCI_EXP_DEVICE_STATUS_PENDING)) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

static bool wait_function_ready(const PciDevice *device) {
    u64 timeout = 0ULL;
    if (!milliseconds_to_ticks(PCI_FLR_READY_TIMEOUT_MS, &timeout)) return false;
    u64 started = timer_ticks();
    for (;;) {
        u16 vendor = pci_config_read16(device->address, PCI_VENDOR_ID_OFFSET);
        u16 device_id = pci_config_read16(device->address, PCI_DEVICE_ID_OFFSET);
        if (vendor == device->vendor_id && device_id == device->device_id) return true;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) arch_pause();
    }
}

static u16 quiesced_command(u16 command) {
    return (u16)((command | PCI_COMMAND_INT_DISABLE) &
        (u16)~(PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER));
}

PciFlrResult pci_flr_execute_quiesced(const PciDevice *device,
    u16 *out_command_after) {
    if (out_command_after) *out_command_after = 0U;
    if (!out_command_after || !interrupts_enabled())
        return PCI_FLR_RESULT_NOT_ATTEMPTED;

    u8 capability = 0U;
    if (!pci_flr_supported(device, &capability))
        return PCI_FLR_RESULT_NOT_ATTEMPTED;

    u16 command = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    if (command == 0xFFFFU ||
        (command & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER)) || !(command & PCI_COMMAND_INT_DISABLE))
        return PCI_FLR_RESULT_NOT_ATTEMPTED;

    if (!wait_transaction_clear(device, capability))
        return PCI_FLR_RESULT_NOT_ATTEMPTED;

    u16 control = pci_config_read16(device->address,
        (u16)capability + PCI_EXP_DEVICE_CONTROL);
    if (control == 0xFFFFU) return PCI_FLR_RESULT_NOT_ATTEMPTED;

    pci_config_write16(device->address,
        (u16)capability + PCI_EXP_DEVICE_CONTROL,
        (u16)(control | PCI_EXP_DEVICE_CONTROL_FLR));

    u64 minimum_wait = 0ULL;
    if (!milliseconds_to_ticks(PCI_FLR_MINIMUM_WAIT_MS, &minimum_wait) ||
        !wait_ticks(minimum_wait) || !wait_function_ready(device))
        return PCI_FLR_RESULT_FAILED;

    u16 command_after = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    if (command_after == 0xFFFFU) return PCI_FLR_RESULT_FAILED;
    command_after = quiesced_command(command_after);
    pci_config_write16(device->address, PCI_COMMAND_OFFSET, command_after);
    command_after = pci_config_read16(device->address, PCI_COMMAND_OFFSET);
    if (command_after == 0xFFFFU ||
        (command_after & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER)) || !(command_after & PCI_COMMAND_INT_DISABLE))
        return PCI_FLR_RESULT_FAILED;

    u8 capability_after = 0U;
    if (!pci_flr_supported(device, &capability_after) ||
        capability_after != capability) return PCI_FLR_RESULT_FAILED;

    *out_command_after = command_after;
    return PCI_FLR_RESULT_COMPLETE;
}
