#include "polaris_observation.h"
#include "lib.h"
void polaris_observation_init(PolarisObservationBatch *b) {
    if (!b) return;
    k_memset(b, 0, sizeof(*b));
    b->pipe = ~0U;
}
bool polaris_observation_next(PolarisObservationBatch *b, u32 *pipe, u32 *field) {
    if (!b || !pipe || !field) return false;
    *pipe = *field = ~0U;
    if (b->closed || b->pending || b->reads >= POLARIS_OBSERVATION_MAX_READS) {
        b->closed = true; return false;
    }
    *pipe = b->reads < POLARIS_DISPLAY_PIPE_COUNT ? b->reads : b->pipe;
    *field = b->reads < POLARIS_DISPLAY_PIPE_COUNT ? POLARIS_REG_CRTC_CONTROL :
        (b->reads - POLARIS_DISPLAY_PIPE_COUNT) % POLARIS_DISPLAY_REGISTER_COUNT;
    b->pending = true;
    return true;
}
bool polaris_observation_accept(PolarisObservationBatch *b, u32 value) {
    if (!b || b->closed || !b->pending || b->reads >= POLARIS_OBSERVATION_MAX_READS) {
        if (b) b->closed = true;
        return false;
    }
    b->pending = false;
    u32 index = b->reads++;
    if (value == ~0U) { b->closed = true; return false; }
    if (index < POLARIS_DISPLAY_PIPE_COUNT) {
        b->controls[index] = value;
        bool requested = (value & POLARIS_CRTC_MASTER_EN_MASK) != 0;
        bool running = (value & POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK) != 0;
        if (requested != running || (running && (value & POLARIS_CRTC_DISP_READ_REQUEST_DISABLE_MASK))) {
            b->closed = true; return false;
        }
        if (running) {
            if (b->pipe != ~0U) { b->closed = true; return false; }
            b->pipe = index;
        }
        if (b->reads == POLARIS_DISPLAY_PIPE_COUNT && b->pipe == ~0U) {
            b->closed = true; return false;
        }
        return true;
    }
    u32 field = (index - POLARIS_DISPLAY_PIPE_COUNT) % POLARIS_DISPLAY_REGISTER_COUNT;
    bool first = index < POLARIS_DISPLAY_PIPE_COUNT + POLARIS_DISPLAY_REGISTER_COUNT;
    if ((field == POLARIS_REG_CRTC_CONTROL && value != b->controls[b->pipe]) ||
        (field == POLARIS_REG_CRTC_BLANK &&
            (value & (POLARIS_CRTC_CURRENT_BLANK_STATE_MASK | POLARIS_CRTC_BLANK_DATA_EN_MASK))) ||
        (field == POLARIS_REG_GRPH_ENABLE && !(value & POLARIS_GRPH_ENABLE_MASK)) ||
        (!first && value != b->values[field])) {
        b->closed = true; return false;
    }
    if (first) b->values[field] = value;
    if (b->reads == POLARIS_OBSERVATION_MAX_READS) b->complete = b->closed = true;
    return true;
}
