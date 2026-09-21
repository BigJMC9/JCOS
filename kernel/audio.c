#include "audio.h"

#include "arch.h"
#include "capability.h"
#include "lib.h"
#include "pci.h"
#include "physmap.h"
#include "pmm.h"
#include "user_memory.h"
#include "../include/media_abi.h"

#define PCI_CLASS_MULTIMEDIA 0x04U
#define PCI_SUBCLASS_AUDIO   0x01U

#define AC97_DESCRIPTOR_COUNT 32U
#define AC97_BUFFER_BYTES     JCOS_MEDIA_AUDIO_SUBMIT_MAX
#define AC97_DMA_PAGES        (1U + AC97_DESCRIPTOR_COUNT)

#define AC97_PO_BDBAR 0x10U
#define AC97_PO_CIV   0x14U
#define AC97_PO_LVI   0x15U
#define AC97_PO_SR    0x16U
#define AC97_PO_CR    0x1BU
#define AC97_GLOB_CNT 0x2CU
#define AC97_GLOB_STA 0x30U

#define AC97_CR_RUN   0x01U
#define AC97_CR_RESET 0x02U
#define AC97_SR_DCH   0x01U
#define AC97_GLOB_COLD_RESET 0x00000002U
#define AC97_GLOB_CODEC_READY 0x00000100U

typedef struct PACKED {
    u32 address;
    u32 control;
} Ac97Descriptor;

static DeviceResource g_audio;
static u16 g_mixer_base;
static u16 g_bus_master_base;
static u64 g_dma_physical;
static Ac97Descriptor *g_descriptors;
static u8 *g_buffers;
static u8 g_last_valid;
static bool g_started;
static bool g_available;

static void audio_delay(void) {
    for (u32 i = 0; i < 10000U; ++i) arch_pause();
}

static void audio_engine_reset(void) {
    if (!g_available) return;
    arch_out8(g_bus_master_base + AC97_PO_CR, 0U);
    arch_out8(g_bus_master_base + AC97_PO_CR, AC97_CR_RESET);
    for (u32 i = 0; i < 10000U; ++i) {
        if (!(arch_in8(g_bus_master_base + AC97_PO_CR) & AC97_CR_RESET)) break;
        arch_pause();
    }
    arch_out16(g_bus_master_base + AC97_PO_SR, 0x1CU);
    arch_out32(g_bus_master_base + AC97_PO_BDBAR, (u32)g_dma_physical);
    g_last_valid = 0U;
    g_started = false;
}

bool audio_init(void) {
    if (g_available) return true;
    const PciDevice *device = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, 0U);
    PciBar mixer;
    PciBar bus_master;
    if (!device || !pci_read_bar(device, 0U, &mixer) || !pci_read_bar(device, 1U, &bus_master) ||
        mixer.type != PCI_BAR_IO || bus_master.type != PCI_BAR_IO ||
        mixer.base > 0xFFFFU || bus_master.base > 0xFFFFU) return false;

    pci_enable_io_space(device);
    pci_enable_bus_master(device);
    g_mixer_base = (u16)mixer.base;
    g_bus_master_base = (u16)bus_master.base;

    arch_out32(g_bus_master_base + AC97_GLOB_CNT, AC97_GLOB_COLD_RESET);
    audio_delay();
    if (!(arch_in32(g_bus_master_base + AC97_GLOB_STA) & AC97_GLOB_CODEC_READY)) return false;

    g_dma_physical = pmm_alloc_pages(AC97_DMA_PAGES);
    if (!g_dma_physical || g_dma_physical > 0xFFFFFFFFULL -
        (u64)AC97_DMA_PAGES * FRAME_SIZE) return false;
    g_descriptors = (Ac97Descriptor *)phys_to_virt(g_dma_physical);
    g_buffers = (u8 *)phys_to_virt(g_dma_physical + FRAME_SIZE);
    if (!g_descriptors || !g_buffers) return false;
    k_memset(g_descriptors, 0, FRAME_SIZE);
    k_memset(g_buffers, 0, (usize)AC97_DESCRIPTOR_COUNT * AC97_BUFFER_BYTES);

    for (u32 i = 0; i < AC97_DESCRIPTOR_COUNT; ++i)
        g_descriptors[i].address = (u32)(g_dma_physical + FRAME_SIZE +
            (u64)i * AC97_BUFFER_BYTES);

    /* Unmute master and PCM output. The emulated codec defaults to 48 kHz. */
    arch_out16(g_mixer_base + 0x00U, 0x0000U);
    arch_out16(g_mixer_base + 0x18U, 0x0000U);
    g_available = true;
    audio_engine_reset();
    if (!device_resource_create(&g_audio, DEVICE_RESOURCE_AUDIO)) {
        g_available = false;
        return false;
    }
    return true;
}

bool audio_available(void) {
    return g_available && device_resource_valid(&g_audio);
}

u32 audio_sample_rate(void) {
    return audio_available() ? JCOS_MEDIA_AUDIO_RATE : 0U;
}

u32 audio_submit_max(void) {
    return audio_available() ? AC97_BUFFER_BYTES : 0U;
}

DeviceResource *audio_resource(void) {
    return audio_available() ? &g_audio : 0;
}

void audio_session_reset(void) {
    audio_engine_reset();
}

static bool audio_authorized(Process *process, CapabilityHandle handle) {
    CapabilityTable *caps = process ? process_capabilities(process) : 0;
    void *resource = 0;
    return caps && capability_lookup_rights(caps, handle,
        CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_WRITE, &resource) &&
        resource == &g_audio;
}

bool audio_write_user(Process *process, CapabilityHandle handle, u64 samples, u32 byte_count) {
    if (!audio_authorized(process, handle) || !samples || !byte_count ||
        byte_count > AC97_BUFFER_BYTES || (byte_count & 3U)) return false;

    u8 index = 0U;
    if (g_started) {
        index = (u8)((g_last_valid + 1U) & (AC97_DESCRIPTOR_COUNT - 1U));
        u8 current = (u8)(arch_in8(g_bus_master_base + AC97_PO_CIV) &
            (AC97_DESCRIPTOR_COUNT - 1U));
        if (index == current && !(arch_in16(g_bus_master_base + AC97_PO_SR) & AC97_SR_DCH))
            return false;
    }

    u8 *destination = g_buffers + (u32)index * AC97_BUFFER_BYTES;
    if (!user_memory_read(process, samples, destination, byte_count)) return false;
    if (byte_count < AC97_BUFFER_BYTES)
        k_memset(destination + byte_count, 0, AC97_BUFFER_BYTES - byte_count);
    g_descriptors[index].control = (byte_count / 2U) & 0xFFFFU;
    __asm__ volatile ("" : : : "memory");
    arch_out8(g_bus_master_base + AC97_PO_LVI, index);
    g_last_valid = index;

    u16 status = arch_in16(g_bus_master_base + AC97_PO_SR);
    if (!g_started || (status & AC97_SR_DCH)) {
        arch_out16(g_bus_master_base + AC97_PO_SR, 0x1CU);
        arch_out8(g_bus_master_base + AC97_PO_CR, AC97_CR_RUN);
        g_started = true;
    }
    return true;
}
