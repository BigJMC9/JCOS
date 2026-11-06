#include "polaris_mmio_plan_test.h"
#include "polaris_mmio_fixture.h"
#include "amd_polaris_registers.h"
static bool zero_plan(const PolarisMmioPlan *plan) {
    const u8 *p = (const u8 *)plan;
    for (u32 i = 0; i < sizeof(*plan); ++i) if (p[i]) return false;
    return true;
}
static bool refused(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u16 pmcsr) {
    PolarisMmioPlan plan;
    k_memset(&plan, 0xA5, sizeof(plan));
    return !polaris_mmio_plan(boot, device, info, pmcsr, &plan) && zero_plan(&plan);
}
bool polaris_mmio_plan_test(void) {
    BootInfo boot; PciDevice device; DisplayProbeInfo info; u8 config[PCI_CONFIG_SPACE_SIZE];
    if (!polaris_mmio_fixture(&boot, &device, &info, config)) return false;
    PolarisMmioPlan plan;
    if (!polaris_mmio_plan(&boot, &device, &info, 0U, &plan) || plan.page_count != 4U ||
        plan.page_offsets[0] != 0x6000U || plan.page_offsets[1] != 0x7000U ||
        plan.page_offsets[2] != 0x10000U || plan.page_offsets[3] != 0x11000U ||
        plan.bar.base != 0x63400000ULL || plan.bar.size != 0x40000ULL) return false;
    /* Every named field must land wholly inside one of the four mapped pages. */
    for (u32 pipe = 0; pipe < POLARIS_DISPLAY_PIPE_COUNT; ++pipe) {
        for (u32 field = 0; field < POLARIS_DISPLAY_REGISTER_COUNT; ++field) {
            u32 offset = 0, matches = 0;
            if (!jcos_polaris_register_offset(pipe, (PolarisDisplayRegister)field, plan.bar.size, &offset))
                return false;
            for (u32 i = 0; i < plan.page_count; ++i)
                if (offset >= plan.page_offsets[i] && offset + 4U <= plan.page_offsets[i] + 4096U)
                    ++matches;
            if (matches != 1U) return false;
        }
    }
    /* Mutate every byte of the complete descriptor: either refuse or validate
     * again as a complete, matching descriptor and matching saved audits.
     * Here saved audits stay fixed, so every one-bit mutation must be refused. */
    for (u32 i = 0; i < 48U; ++i) {
        boot.framebuffer_bar_queries[3].prefix[i] ^= 1U;
        bool rejected = refused(&boot, &device, &info, 0U);
        boot.framebuffer_bar_queries[3].prefix[i] ^= 1U;
        /* EndTag checksum 0->1 and all reported fields are checked. General/
         * specific fields too are audited; resource-type never gets masked. */
        if (!rejected) return false;
    }
    boot.framebuffer_bar_queries[3].prefix_size = 46U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    boot.framebuffer_bar_queries[3].prefix_size = 48U;
    boot.framebuffer_bar_queries[3].audit.descriptor_count = 2U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    boot.framebuffer_bar_queries[3].audit.descriptor_count = 1U;
    boot.framebuffer_pci_bus++;
    if (!refused(&boot, &device, &info, 0U)) return false;
    boot.framebuffer_pci_bus--;
    device.revision++;
    if (!refused(&boot, &device, &info, 0U)) return false;
    device.revision--;
    info.subsystem_device_id++;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.subsystem_device_id--;
    info.command = 0U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.command = PCI_COMMAND_MEMORY_SPACE;
    info.pm_offset = 0U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.pm_offset = 4U;
    info.capability_offsets[0] = 4U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.capability_offsets[0] = 0x50U;
    info.pm_offset = 0xFCU;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.pm_offset = 0x50U;
    info.capability_ids[0] = 9U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.capability_ids[0] = 1U;
    for (u16 state = 1U; state <= 3U; ++state)
        if (!refused(&boot, &device, &info, state)) return false;
    if (!refused(&boot, &device, &info, 0xFFFFU)) return false;
    boot.framebuffer_bars[5].size *= 2U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    boot.framebuffer_bars[5].size /= 2U;
    info.bars[5].prefetchable = true;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.bars[5].prefetchable = false;
    info.bars[5].index = 0U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    info.bars[5].index = 5U;
    device.header_type = 1U;
    if (!refused(&boot, &device, &info, 0U)) return false;
    device.header_type = 0U;
    return refused(0, &device, &info, 0U) && refused(&boot, 0, &info, 0U) &&
        refused(&boot, &device, 0, 0U) && !polaris_mmio_plan(&boot, &device, &info, 0U, 0);
}
