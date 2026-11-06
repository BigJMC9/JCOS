#include "polaris_vram.h"
#include "lib.h"
bool polaris_vram_observe(const BootInfo *boot, const BootDisplayBar *aperture,
    const u32 *v, const u32 before[2], const u32 after[2], PolarisVramObservation *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!boot || !aperture || !v || !before || !after) return false;
    for (u32 i=0; i<2U; ++i)
        if (before[i]==~0U || before[i]!=after[i]) return false;
    for (u32 i=0; i<POLARIS_DISPLAY_REGISTER_COUNT; ++i) if (v[i]==~0U) return false;
    /* First correlation profile deliberately matches the measured inherited
     * firmware layout. Broader decoder support is not address authority. */
    if (boot->framebuffer_width!=1024U || boot->framebuffer_height!=768U ||
        boot->framebuffer_pixels_per_scanline!=1024U || boot->framebuffer_pixel_format!=1U ||
        v[POLARIS_REG_GRPH_CONTROL]!=0x0A10800AU ||
        v[POLARIS_REG_GRPH_ENABLE]!=1U || v[POLARIS_REG_GRPH_SWAP] ||
        v[POLARIS_REG_CRTC_BLANK] || (v[POLARIS_REG_CRTC_INTERLACE]&1U) ||
        (v[POLARIS_REG_CRTC_CONTROL] & (POLARIS_CRTC_MASTER_EN_MASK |
            POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK |
            POLARIS_CRTC_DISP_READ_REQUEST_DISABLE_MASK))!=0x10001U ||
        v[POLARIS_REG_PITCH]!=1024U || v[POLARIS_REG_X_END]!=1024U ||
        v[POLARIS_REG_Y_END]!=768U || v[POLARIS_REG_OFFSET_X] || v[POLARIS_REG_OFFSET_Y] ||
        v[POLARIS_REG_X_START] || v[POLARIS_REG_Y_START] ||
        (v[POLARIS_REG_PRIMARY_LOW]&~POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_MASK) ||
        (v[POLARIS_REG_PRIMARY_HIGH]&~POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH_MASK))
        return false;
    const u64 limit=1ULL<<40, bytes=0x300000ULL;
    if (aperture->flags!=(BOOT_DISPLAY_BAR_VALID|BOOT_DISPLAY_BAR_MEMORY|
            BOOT_DISPLAY_BAR_64BIT|BOOT_DISPLAY_BAR_PREFETCH) || aperture->reserved ||
        !aperture->base || (aperture->base&0xFFFULL) || !aperture->size ||
        aperture->base>=limit || aperture->size>limit-aperture->base ||
        boot->framebuffer_base<aperture->base || boot->framebuffer_size<bytes)
        return false;
    u64 offset=boot->framebuffer_base-aperture->base;
    if (offset>aperture->size || bytes>aperture->size-offset ||
        boot->framebuffer_size>aperture->size-offset) return false;
    u64 start=(u64)(before[0]&POLARIS_MC_FB_BASE_MASK)<<24;
    u64 end=((u64)((before[0]&POLARIS_MC_FB_TOP_MASK)>>POLARIS_MC_FB_TOP_SHIFT)+1ULL)<<24;
    u64 hdp=(u64)(before[1]&POLARIS_HDP_NONSURFACE_BASE_MASK)<<8;
    u64 gpu=((u64)v[POLARIS_REG_PRIMARY_HIGH]<<32)|v[POLARIS_REG_PRIMARY_LOW];
    /* Require the simple unshifted aperture relationship for this experiment.
     * A different HDP window is refused, never guessed. */
    if (start>=end || end>limit || hdp!=start || aperture->size>end-hdp ||
        offset>end-hdp || bytes>end-hdp-offset || gpu!=hdp+offset) return false;
    out->vram_gpu_start=start; out->vram_gpu_end=end; out->hdp_gpu_start=hdp;
    out->surface_gpu_address=gpu; out->surface_cpu_address=boot->framebuffer_base;
    out->surface_bytes=bytes;
    return true;
}
