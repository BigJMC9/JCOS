#include "amd_polaris_display_test.h"
#include "../drivers/amd_polaris_display.h"

typedef struct {
    JcosU32 reads;
    JcosU32 fail_at;
    JcosU32 change_at;
} ModelReader;

static int model_read(void *context, JcosU32 offset, JcosU32 *out) {
    ModelReader *model = (ModelReader *)context;
    ++model->reads;
    if (model->fail_at == model->reads) return 0;
    /* Pipe 3 byte offsets from the AMD reference, independent of plan lookup. */
    if (offset == 0x10670U) *out = 0x10001U;
    else if (offset == 0x10000U) *out = 1U;
    else if (offset == 0x10004U) *out = 2U;
    else if (offset == 0x10010U) *out = 0x200000U;
    else if (offset == 0x1001CU) *out = 1U;
    else if (offset == 0x10018U || offset == 0x10034U) *out = 1024U;
    else if (offset == 0x10038U) *out = 768U;
    else if (offset == 0x10674U || offset == 0x10678U || offset == 0x1000CU ||
        offset == 0x10024U || offset == 0x10028U || offset == 0x1002CU ||
        offset == 0x10030U) *out = 0U;
    else return 0;
    if (model->reads == model->change_at) *out ^= 1U;
    return 1;
}

JcosU32 polaris_display_model_test(void) {
    JcosU32 passed = 0U, offset = 0U;
    if (polaris_display_register_offset(3U, POLARIS_REG_CRTC_CONTROL, 0x40000ULL, &offset) &&
        offset == 0x10670U &&
        polaris_display_register_offset(5U, POLARIS_REG_PITCH, 0x40000ULL, &offset) &&
        offset == 0x11018U) passed |= 1U;
    ModelReader reader = {0};
    PolarisDisplaySnapshot snapshot;
    PolarisDisplayObservation observed;
    if (polaris_display_capture(3U, 0x40000ULL, model_read, &reader, &snapshot) ==
            POLARIS_CAPTURE_OK && reader.reads == 30U &&
        polaris_display_decode(&snapshot, &observed) && !observed.restrictions &&
        observed.surface_gpu_address == 0x100200000ULL &&
        observed.surface_width == 1024U && observed.surface_height == 768U &&
        observed.linear_span_bytes == 0x300000ULL) passed |= 2U;
    reader.reads = 0U;
    if (polaris_display_capture(3U, 0x1067BULL, model_read, &reader, &snapshot) ==
            POLARIS_CAPTURE_INVALID && !reader.reads && !snapshot.version) passed |= 4U;
    reader.reads = 0U;
    reader.fail_at = 8U;
    if (polaris_display_capture(3U, 0x40000ULL, model_read, &reader, &snapshot) ==
            POLARIS_CAPTURE_READ_FAILED && reader.reads == 8U && !snapshot.version)
        passed |= 8U;
    reader.reads = 0U;
    reader.fail_at = 0U;
    reader.change_at = 30U;
    if (polaris_display_capture(3U, 0x40000ULL, model_read, &reader, &snapshot) ==
            POLARIS_CAPTURE_CHANGED && reader.reads == 30U && !snapshot.version)
        passed |= 16U;
    reader.reads = 0U;
    reader.change_at = 0U;
    if (polaris_display_capture(3U, 0x40000ULL, model_read, &reader, &snapshot) ==
            POLARIS_CAPTURE_OK) {
        snapshot.values[POLARIS_REG_GRPH_CONTROL] |= 4U << 20U;
        if (polaris_display_decode(&snapshot, &observed) &&
            (observed.restrictions & POLARIS_OBSERVATION_TILED) && !observed.linear_span_bytes)
            passed |= 32U;
    }
    return passed;
}
