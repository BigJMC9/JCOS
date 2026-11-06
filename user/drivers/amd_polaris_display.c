#include "amd_polaris_display.h"

/* Register facts: AMD DCE_11_2 documentation in Linux v6.12
 * drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_{d,sh_mask}.h.
 * See docs/AMD_POLARIS_DISPLAY.md for sources and interpretation limits.
 * Indices are DWORDs in AMD's headers; the callback consumes BYTE offsets.
 * Pipe 3 jumps to 0x4000: a uniform stride across all six pipes is incorrect. */
static void clear_bytes(void *output, JcosU32 bytes) {
    volatile unsigned char *p = (volatile unsigned char *)output;
    while (bytes--) *p++ = 0U;
}

int polaris_display_register_offset(JcosU32 pipe, PolarisDisplayRegister field,
    JcosU64 mmio_span, JcosU32 *out_byte_offset) {
    return jcos_polaris_register_offset(pipe, field, mmio_span, out_byte_offset);
}

PolarisCaptureResult polaris_display_capture(JcosU32 pipe, JcosU64 mmio_span,
    PolarisDisplayRead32 read32, void *context, PolarisDisplaySnapshot *out) {
    if (!out) return POLARIS_CAPTURE_INVALID;
    clear_bytes(out, sizeof(*out));
    if (!read32) return POLARIS_CAPTURE_INVALID;
    JcosU32 offsets[POLARIS_DISPLAY_REGISTER_COUNT];
    JcosU32 first[POLARIS_DISPLAY_REGISTER_COUNT];
    JcosU32 second[POLARIS_DISPLAY_REGISTER_COUNT];
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i) {
        if (!polaris_display_register_offset(pipe, (PolarisDisplayRegister)i,
                mmio_span, &offsets[i])) return POLARIS_CAPTURE_INVALID;
    }
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i) {
        first[i] = ~0U;
        if (!read32(context, offsets[i], &first[i]) || first[i] == ~0U)
            return POLARIS_CAPTURE_READ_FAILED;
    }
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i) {
        second[i] = ~0U;
        if (!read32(context, offsets[i], &second[i]) || second[i] == ~0U)
            return POLARIS_CAPTURE_READ_FAILED;
    }
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i)
        if (first[i] != second[i]) return POLARIS_CAPTURE_CHANGED;
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i) out->values[i] = first[i];
    out->pipe = pipe;
    out->version = POLARIS_DISPLAY_SNAPSHOT_VERSION;
    return POLARIS_CAPTURE_OK;
}

int polaris_display_decode(const PolarisDisplaySnapshot *snapshot,
    PolarisDisplayObservation *out) {
    if (!out) return 0;
    clear_bytes(out, sizeof(*out));
    if (!snapshot || snapshot->version != POLARIS_DISPLAY_SNAPSHOT_VERSION ||
        snapshot->pipe >= POLARIS_DISPLAY_PIPE_COUNT) return 0;
    const JcosU32 *v = snapshot->values;
    for (JcosU32 i = 0U; i < POLARIS_DISPLAY_REGISTER_COUNT; ++i)
        if (v[i] == ~0U) return 0;

    JcosU32 control = v[POLARIS_REG_GRPH_CONTROL];
    out->surface_gpu_address = ((JcosU64)(v[POLARIS_REG_PRIMARY_HIGH] & POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH_MASK) << 32) |
        (v[POLARIS_REG_PRIMARY_LOW] & POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
    out->pitch_pixels = v[POLARIS_REG_PITCH] & POLARIS_GRPH_PITCH_MASK;
    out->depth = control & POLARIS_GRPH_DEPTH_MASK;
    out->format = (control & POLARIS_GRPH_FORMAT_MASK) >> POLARIS_GRPH_FORMAT_SHIFT;
    out->array_mode = (control & POLARIS_GRPH_ARRAY_MODE_MASK) >> POLARIS_GRPH_ARRAY_MODE_SHIFT;
    const JcosU32 enabled = POLARIS_CRTC_MASTER_EN_MASK | POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK;
    if ((v[POLARIS_REG_CRTC_CONTROL] & enabled) != enabled ||
        (v[POLARIS_REG_CRTC_CONTROL] & POLARIS_CRTC_DISP_READ_REQUEST_DISABLE_MASK) ||
        !(v[POLARIS_REG_GRPH_ENABLE] & POLARIS_GRPH_ENABLE_MASK) ||
        (v[POLARIS_REG_CRTC_BLANK] & (POLARIS_CRTC_CURRENT_BLANK_STATE_MASK | POLARIS_CRTC_BLANK_DATA_EN_MASK)))
        out->restrictions |= POLARIS_OBSERVATION_INACTIVE;
    /* DCE11.2 ARRAY_LINEAR_ALIGNED is linear, not a tiled layout. Firmware
     * retains bank/pipe tiling metadata in this mode; these fields do not enter
     * the linear byte-offset calculation. Z/privilege/microtile remain refused.
     * Keep the older linear-general profile's conservative control policy. */
    JcosU32 allowed = POLARIS_GRPH_DEPTH_MASK | POLARIS_GRPH_FORMAT_MASK |
        POLARIS_GRPH_ARRAY_MODE_MASK | POLARIS_GRPH_ADDRESS_TRANSLATION_ENABLE_MASK |
        POLARIS_GRPH_COLOR_EXPANSION_MODE_MASK;
    if (out->array_mode == POLARIS_ARRAY_LINEAR_ALIGNED)
        allowed |= POLARIS_GRPH_NUM_BANKS_MASK | POLARIS_GRPH_BANK_WIDTH_MASK |
            POLARIS_GRPH_BANK_HEIGHT_MASK | POLARIS_GRPH_TILE_SPLIT_MASK |
            POLARIS_GRPH_MACRO_TILE_ASPECT_MASK | POLARIS_GRPH_PIPE_CONFIG_MASK;
    if (control & ~allowed)
        out->restrictions |= POLARIS_OBSERVATION_CONTROL;
    if (v[POLARIS_REG_GRPH_ENABLE] & ~(POLARIS_GRPH_ENABLE_MASK | POLARIS_GRPH_KEYER_ALPHA_SEL_MASK))
        out->restrictions |= POLARIS_OBSERVATION_RESERVED;
    if (out->array_mode > POLARIS_ARRAY_LINEAR_ALIGNED)
        out->restrictions |= POLARIS_OBSERVATION_TILED;
    /* This profile accepts only a conservative 256-byte row alignment. */
    if (out->array_mode == POLARIS_ARRAY_LINEAR_ALIGNED && (out->pitch_pixels & 63U))
        out->restrictions |= POLARIS_OBSERVATION_GEOMETRY;
    if (control & POLARIS_GRPH_ADDRESS_TRANSLATION_ENABLE_MASK) out->restrictions |= POLARIS_OBSERVATION_TRANSLATED;
    if (out->depth != 2U || out->format || v[POLARIS_REG_GRPH_SWAP] ||
        (control & POLARIS_GRPH_COLOR_EXPANSION_MODE_MASK) || (v[POLARIS_REG_GRPH_ENABLE] & POLARIS_GRPH_KEYER_ALPHA_SEL_MASK))
        out->restrictions |= POLARIS_OBSERVATION_FORMAT;
    if (v[POLARIS_REG_OFFSET_X] || v[POLARIS_REG_OFFSET_Y] ||
        v[POLARIS_REG_X_START] || v[POLARIS_REG_Y_START])
        out->restrictions |= POLARIS_OBSERVATION_OFFSET;
    if (v[POLARIS_REG_CRTC_INTERLACE] & POLARIS_CRTC_INTERLACE_ENABLE_MASK)
        out->restrictions |= POLARIS_OBSERVATION_INTERLACED;
    if ((v[POLARIS_REG_PRIMARY_HIGH] & ~POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH_MASK) ||
        (v[POLARIS_REG_PRIMARY_LOW] & ~POLARIS_GRPH_PRIMARY_SURFACE_ADDRESS_MASK))
        out->restrictions |= POLARIS_OBSERVATION_ADDRESS;
    if ((v[POLARIS_REG_PITCH] & ~POLARIS_GRPH_PITCH_MASK) ||
        (v[POLARIS_REG_X_START] & ~POLARIS_GRPH_X_START_MASK) ||
        (v[POLARIS_REG_Y_START] & ~POLARIS_GRPH_Y_START_MASK) ||
        (v[POLARIS_REG_X_END] & ~POLARIS_GRPH_X_END_MASK) ||
        (v[POLARIS_REG_Y_END] & ~POLARIS_GRPH_Y_END_MASK))
        out->restrictions |= POLARIS_OBSERVATION_RESERVED;
    JcosU32 x_start = v[POLARIS_REG_X_START] & POLARIS_GRPH_X_START_MASK;
    JcosU32 y_start = v[POLARIS_REG_Y_START] & POLARIS_GRPH_Y_START_MASK;
    JcosU32 x_end = v[POLARIS_REG_X_END] & POLARIS_GRPH_X_END_MASK;
    JcosU32 y_end = v[POLARIS_REG_Y_END] & POLARIS_GRPH_Y_END_MASK;
    if (x_end <= x_start || y_end <= y_start || x_end > out->pitch_pixels)
        out->restrictions |= POLARIS_OBSERVATION_GEOMETRY;
    else {
        out->surface_width = x_end - x_start;
        out->surface_height = y_end - y_start;
    }
    if (!out->restrictions) {
        JcosU64 span = ((JcosU64)(out->surface_height - 1U) * out->pitch_pixels + out->surface_width) * 4ULL;
        if (span > (1ULL << 40) - out->surface_gpu_address)
            out->restrictions |= POLARIS_OBSERVATION_ADDRESS;
        else out->linear_span_bytes = span;
    }
    return 1;
}
