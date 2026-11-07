#ifndef JA_OS_POLARIS_PROGRAM_H
#define JA_OS_POLARIS_PROGRAM_H
#include "polaris_plane.h"
#define POLARIS_PROGRAM_POLLS 64U
typedef enum {
    POLARIS_REBIND_LOW=0,POLARIS_REBIND_HIGH,POLARIS_REBIND_CRTC,
    POLARIS_REBIND_UPDATE,POLARIS_REBIND_FLIP,POLARIS_REBIND_FRAME
} PolarisRebindRead;
typedef struct {
    void *context;
    bool (*admitted)(void *);
    bool (*write)(void *,PolarisDisplayRegister,u32);
    bool (*read)(void *,PolarisRebindRead,u32 *);
    bool (*wait)(void *);
} PolarisProgramIo;
typedef struct {
    u64 incarnation,observation_id,gpu_address;
    u64 tags[5],values[5];
    PolarisPlaneWitness witness;
    u32 pipe,crtc;
    bool consumed;
} PolarisProgramPlan;
typedef struct {
    u32 write_attempts,reads,polls,frame_delta;
    bool complete;
} PolarisProgramResult;
/* Host-test reference only; excluded from the kernel build.
 * Normal sequencing is user/drivers/amd_polaris_program.c; the privileged
 * finite validator is polaris_program_gate.c. No raw-offset interpreter.
 * All five Ring3 proposals validate before any IO. Exactly HIGH then LOW.
 * IO implementation must pin exact retired supervisor mapping/assignment.
 * False after an attempted write is ambiguous: preserve quarantine/resources.
 * Success is same-buffer readback/frame evidence, never monitor/DMA proof. */
bool polaris_program_execute(PolarisProgramPlan *plan,const PolarisProgramIo *io,
    PolarisProgramResult *out);
#endif
