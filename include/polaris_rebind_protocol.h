#ifndef JCOS_POLARIS_REBIND_PROTOCOL_H
#define JCOS_POLARIS_REBIND_PROTOCOL_H
#include "amd_polaris_registers.h"
/* Private scalar conversation on the driver's existing endpoint pair.
 * No addresses, offsets, arbitrary widths or capability transfers. */
#define POLARIS_REBIND_REQUEST 0x5052424400000001ULL
#define POLARIS_REBIND_RESPONSE 0x5052424400000002ULL
#define POLARIS_REBIND_READ 1U
#define POLARIS_REBIND_WRITE 2U
#define POLARIS_REBIND_WAIT 3U
#define POLARIS_REBIND_POLLS 64U
#define POLARIS_REBIND_MESSAGES (10U+6U*POLARIS_REBIND_POLLS+POLARIS_REBIND_POLLS-1U)
#define POLARIS_REBIND_TAG(sequence,operation,field) \
    (((JcosU64)(sequence)<<32)|((JcosU64)(operation)<<16)|(JcosU64)(field))
/* READ fields: low,high,crtc,update,flip,frame (0..5).
 * WRITE fields: the existing semantic primary-high/low enum only.
 * WAIT field/payload=0. Sequence begins at1, never wraps or retries.
 * Request: magic,incarnation,tag,payload.
 * Reply: magic,incarnation,tag,value (zero for WRITE/WAIT).
 * Completion: normal native reply,incarnation,EXECUTE,frame delta.
 * Only the kernel's independently completed gate can authorize drawing. */
#endif
