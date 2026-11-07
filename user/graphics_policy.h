#ifndef JCOS_USER_GRAPHICS_POLICY_H
#define JCOS_USER_GRAPHICS_POLICY_H
#include "../include/user_abi.h"
#include "../include/graphics_supervisor_protocol.h"
typedef struct {
    JcosU64 session,sequence;
    JcosU32 stage;
} GraphicsPolicy;
/* Invalid/replayed envelopes return REJECT without changing the ledger.
 * Closed sessions cannot be reopened; a later distinct session must increase. */
JcosU64 graphics_policy_decide(GraphicsPolicy *policy,JcosU64 session,
    JcosU64 sequence,JcosU64 snapshot);
#endif
