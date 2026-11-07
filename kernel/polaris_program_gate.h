#ifndef JA_OS_POLARIS_PROGRAM_GATE_H
#define JA_OS_POLARIS_PROGRAM_GATE_H
#include "polaris_program.h"
#include "polaris_rebind_protocol.h"
typedef struct {
    PolarisProgramResult result;
    u32 state,field,sequence,flip,update,baseline;
    bool initialized,poisoned;
} PolarisProgramGate;
bool polaris_program_gate_init(PolarisProgramGate *gate,PolarisProgramPlan *plan);
bool polaris_program_gate_step(PolarisProgramGate *gate,const PolarisProgramIo *io,
    u64 tag,u64 payload,u32 *out);
#endif
