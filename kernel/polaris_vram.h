#ifndef JA_OS_POLARIS_VRAM_H
#define JA_OS_POLARIS_VRAM_H
#include "display_probe.h"
#include "amd_polaris_registers.h"
/* Observation of the existing firmware allocation only. Grants no VRAM access,
 * allocation, modeset or evidence that other VRAM is free. End is exclusive. */
typedef struct {
    u64 vram_gpu_start, vram_gpu_end, hdp_gpu_start;
    u64 surface_gpu_address, surface_cpu_address, surface_bytes;
} PolarisVramObservation;
bool polaris_vram_observe(const BootInfo *boot, const BootDisplayBar *aperture,
    const u32 *values, const u32 before[2], const u32 after[2], PolarisVramObservation *out);
#endif
