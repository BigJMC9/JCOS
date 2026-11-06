#ifndef JA_OS_POLARIS_OBSERVATION_H
#define JA_OS_POLARIS_OBSERVATION_H
#include "types.h"
#include "amd_polaris_registers.h"
#define POLARIS_OBSERVATION_MAX_READS (POLARIS_DISPLAY_PIPE_COUNT + 2U * POLARIS_DISPLAY_REGISTER_COUNT)
/* Fixed experiment: six CRTC_CONTROL queries, then one uniquely running pipe,
 * fifteen fields twice. No callback/addresses/permission or hardware inference
 * beyond this model. Hardware assumptions are owned by the bring-up profile. */
typedef struct {
    u32 controls[POLARIS_DISPLAY_PIPE_COUNT];
    u32 values[POLARIS_DISPLAY_REGISTER_COUNT];
    u32 pipe, reads;
    bool pending, closed, complete;
} PolarisObservationBatch;
void polaris_observation_init(PolarisObservationBatch *batch);
bool polaris_observation_next(PolarisObservationBatch *batch, u32 *pipe, u32 *field);
bool polaris_observation_accept(PolarisObservationBatch *batch, u32 value);
#endif
