#ifndef JA_OS_POLARIS_MMIO_FIXTURE_H
#define JA_OS_POLARIS_MMIO_FIXTURE_H
/* Synthetic boot/config input. Never a hardware support or read-safety record. */
#include "polaris_mmio.h"
#include "boot_display.h"
#include "lib.h"
static inline void polaris_fixture_put(u8 *p, u32 width, u64 value) {
    for (u32 i = 0; i < width; ++i) p[i] = (u8)(value >> (8U * i));
}
static inline bool polaris_mmio_fixture(BootInfo *boot, PciDevice *device,
    DisplayProbeInfo *info, u8 *config) {
    k_memset(config, 0, PCI_CONFIG_SPACE_SIZE);
    polaris_fixture_put(config, 4, 0x67DF1002U);
    polaris_fixture_put(config + 4, 2, PCI_COMMAND_MEMORY_SPACE);
    polaris_fixture_put(config + 6, 2, 1U << 4);
    polaris_fixture_put(config + 8, 4, 0x030000EFU);
    polaris_fixture_put(config + 0x24, 4, 0x63400000U);
    polaris_fixture_put(config + 0x2C, 4, 0x23101458U);
    config[0x34] = 0x50U;
    config[0x50] = 1U;
    config[0x52] = 3U; /* Standard PCI PM capability version. */
    k_memset(device, 0, sizeof(*device));
    device->address.bus = 1U;
    device->vendor_id = 0x1002U; device->device_id = 0x67DFU;
    device->revision = 0xEFU; device->class_code = 3U;
    if (!display_probe_decode(config, PCI_CONFIG_SPACE_SIZE, info)) return false;
    k_memset(boot, 0, sizeof(*boot));
    boot->magic = BOOT_INFO_MAGIC; boot->version = BOOT_INFO_VERSION; boot->size = sizeof(*boot);
    boot->framebuffer_pci_valid = 1U; boot->framebuffer_pci_bus = 1U;
    boot->framebuffer_pci_vendor_device = 0x67DF1002U;
    boot->framebuffer_pci_class_revision = 0x030000EFU;
    boot->framebuffer_pci_subsystem = 0x23101458U;
    for (u32 i = 0; i < BOOT_DISPLAY_BAR_COUNT; ++i)
        boot->framebuffer_bar_query_index[i] = BOOT_DISPLAY_QUERY_NONE;
    u8 list[48]; k_memset(list, 0, sizeof(list));
    list[0] = 0x8AU; list[1] = 43U;
    polaris_fixture_put(list + 6, 8, 32U);
    polaris_fixture_put(list + 14, 8, 0x63400000U);
    polaris_fixture_put(list + 38, 8, 0x40000U);
    list[46] = 0x79U;
    BootDisplayBarQuery *q = &boot->framebuffer_bar_queries[3];
    q->prefix_size = sizeof(list); q->matching_bars = 1U << 5;
    k_memcpy(q->prefix, list, sizeof(list));
    if (!boot_display_decode_bar_audited(list, sizeof(list), 0x63400000U, 0U, 5U,
            &boot->framebuffer_bars[5], &q->audit)) return false;
    k_memcpy(&boot->framebuffer_bar_audit[5], &q->audit, sizeof(q->audit));
    boot->framebuffer_bar_query_index[5] = 3U; /* Firmware query != config slot. */
    return true;
}
#endif
