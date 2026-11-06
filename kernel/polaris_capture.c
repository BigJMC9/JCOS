#include "polaris_capture.h"
#include "lib.h"
bool polaris_capture_budget_init(PolarisCaptureBudget *b, u32 pipe,
    u32 frequency, u64 now, u64 duration) {
    if (!b) return false;
    k_memset(b, 0, sizeof(*b));
    b->closed = true;
    if (pipe >= POLARIS_DISPLAY_PIPE_COUNT || !frequency || !duration ||
        duration > (u64)frequency * POLARIS_CAPTURE_MAX_SECONDS) return false;
    b->started_at = now;
    b->duration = duration;
    b->frequency = frequency;
    b->pipe = pipe;
    b->closed = false;
    return true;
}
bool polaris_capture_budget_admit(PolarisCaptureBudget *b, u32 pipe,
    u32 field, u32 frequency, u64 now) {
    if (!b || b->closed) return false;
    if (!b->frequency || frequency != b->frequency || !b->duration ||
        b->duration > (u64)b->frequency * POLARIS_CAPTURE_MAX_SECONDS ||
        now - b->started_at >= b->duration ||
        b->pipe >= POLARIS_DISPLAY_PIPE_COUNT || pipe != b->pipe ||
        b->admitted >= POLARIS_CAPTURE_READ_LIMIT ||
        field != b->admitted % POLARIS_DISPLAY_REGISTER_COUNT) {
        b->closed = true;
        return false;
    }
    if (++b->admitted == POLARIS_CAPTURE_READ_LIMIT) b->closed = true;
    return true;
}
void polaris_capture_budget_close(PolarisCaptureBudget *b) { if (b) b->closed = true; }
