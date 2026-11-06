#ifndef JA_OS_POLARIS_CAPTURE_H
#define JA_OS_POLARIS_CAPTURE_H
#include "types.h"
#include "amd_polaris_registers.h"

#define POLARIS_CAPTURE_SAMPLES 2U
#define POLARIS_CAPTURE_READ_LIMIT (POLARIS_CAPTURE_SAMPLES * POLARIS_DISPLAY_REGISTER_COUNT)
#define POLARIS_CAPTURE_MAX_SECONDS 2U

/* Software admission only. An elapsed deadline cannot cancel a bus transaction.
 * A budget is not device authority and grants no permission to read hardware. */
typedef struct {
    u64 started_at;
    u64 duration;
    u32 frequency;
    u32 pipe;
    u32 admitted;
    bool closed;
} PolarisCaptureBudget;
bool polaris_capture_budget_init(PolarisCaptureBudget *budget, u32 pipe,
    u32 frequency, u64 now, u64 duration);
/* One pipe, fields 0..14 twice, at most thirty admissions. Wrong order, clock
 * change or expiry permanently closes this budget. The final admission succeeds
 * and closes it. Callers must independently validate owner/resource/rights. */
bool polaris_capture_budget_admit(PolarisCaptureBudget *budget, u32 pipe,
    u32 field, u32 frequency, u64 now);
void polaris_capture_budget_close(PolarisCaptureBudget *budget);
#endif
