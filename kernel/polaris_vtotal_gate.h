#ifndef JAOS_POLARIS_VTOTAL_GATE_H
#define JAOS_POLARIS_VTOTAL_GATE_H
#include "types.h"
#include "polaris_vtotal_protocol.h"
typedef struct {
 void *context;
 bool (*admitted)(void *);
 bool (*read)(void *,u32,u32 *);
 bool (*write)(void *,u32,u32);
 bool (*wait)(void *);
} PolarisVtotalIo;
typedef struct {u32 write_attempts,read_attempts,reads,polls,frame_delta;bool complete;} PolarisVtotalResult;
typedef struct {
 PolarisVtotalResult result;
 u32 state,field,sequence,baseline,initial[3];
 bool restore,initialized,poisoned;
} PolarisVtotalGate;
void polaris_vtotal_gate_init(PolarisVtotalGate *gate,bool restore);
bool polaris_vtotal_gate_step(PolarisVtotalGate *gate,const PolarisVtotalIo *io,u64 tag,u64 payload,u32 *out);
#endif
