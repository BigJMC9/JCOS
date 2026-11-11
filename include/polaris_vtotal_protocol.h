#ifndef JCOS_POLARIS_VTOTAL_PROTOCOL_H
#define JCOS_POLARIS_VTOTAL_PROTOCOL_H
#include "polaris_vertical_protocol.h"
#define POLARIS_VTOTAL_REQUEST 0x5056544C00000001ULL
#define POLARIS_VTOTAL_RESPONSE 0x5056544C00000002ULL
#define POLARIS_VTOTAL_READ 1U
#define POLARIS_VTOTAL_WRITE 2U
#define POLARIS_VTOTAL_WAIT 3U
#define POLARIS_VTOTAL_POLLS 64U
#define POLARIS_VTOTAL_MESSAGES 458U
#define POLARIS_VTOTAL_TAG(seq,op,field) (((JcosU64)(seq)<<32)|((JcosU64)(op)<<16)|(field))
/* READ: MIN,MAX,CONTROL,CRTC0 control,base V_TOTAL,frame count (0..5).
 * WRITE: MIN,MAX,CONTROL semantic IDs only (0..2). No address/width transfer.
 * One primary round and one independently armed rescue round; no replay.
 * Each round: six pre-reads, three writes, CONTROL posted-read, frame baseline,
 * then at most64 six-read polls and63 waits. Completion is two progressing
 * frames plus exact register readback, never proof of effective display rate. */
static inline int jcos_polaris_vtotal_snapshot_admitted(const JcosU32 *v,JcosU32 count){
 if(!v || count!=POLARIS_VERTICAL_VALUES || !jcos_polaris_vertical_read_admitted(v,80))return 0;
 const JcosU32 fixed[13]={0x10201U,0x897U,0xC00840U,0x2C0000U,0,0x464U,0x290461U,0x50000U,0,0,0x2000000U,0x100U,0x8000U};
 for(JcosU32 i=0;i<13;++i)if(v[26+i]!=fixed[i])return 0;
 if(v[0]!=0x80410311U)return 0;
 for(JcosU32 i=1;i<6;++i)if(v[i]!=0x80400310U)return 0;
 for(JcosU32 i=80;i<86;++i)if(v[i])return 0;
 return v[39] && v[73] && !(v[39]&0xFF000000U) && !(v[73]&0xFF000000U);
}
#endif
