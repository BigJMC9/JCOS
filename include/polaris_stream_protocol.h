#ifndef JCOS_POLARIS_STREAM_PROTOCOL_H
#define JCOS_POLARIS_STREAM_PROTOCOL_H
#include "polaris_vtotal_protocol.h"
/* Copied observation only. Neither a blank/unblank recipe nor a write grant. */
#define POLARIS_STREAM_VERSION 1U
#define POLARIS_STREAM_FIELDS 5U
#define POLARIS_STREAM_VALUES 96U
#define POLARIS_STREAM_WORDS 14U
#define POLARIS_STREAM_ENABLE_MASK 0x1U
#define POLARIS_STREAM_DEFER_MASK 0x300U
#define POLARIS_STREAM_STATUS_MASK 0x10000U
#define POLARIS_STREAM_FIFO_RESET_MASK 0x1U
#define POLARIS_STREAM_DIG_START_MASK 0x400U
#define POLARIS_STREAM_UPDATE_LOCK_MASK 0x1U
#define POLARIS_STREAM_UPDATE_MODE_MASK 0x7U
/* Diagnostic flags preserve suspicious raw state; zero is not write admission. */
#define POLARIS_STREAM_FIFO_RESET 1U
#define POLARIS_STREAM_UPDATE_HELD 2U
#define POLARIS_STREAM_UPDATE_OTHER 4U
#define POLARIS_STREAM_FIFO_OTHER 8U
typedef enum {POLARIS_STREAM_OK=0,POLARIS_STREAM_INVALID,POLARIS_STREAM_ALL_ONES,
 POLARIS_STREAM_CHANGED,POLARIS_STREAM_BASELINE,POLARIS_STREAM_ROUTE} PolarisStreamStatus;
/* 0..85 unchanged vertical preflight,86..90 first and91..95 second samples:
 * STREAM_CNTL,STEER_FIFO,DIG2_FE_CNTL,CRTC0_MASTER_UPDATE_LOCK/MODE.
 * No caller-selected pipe, offset, width, page or access permission. */
static inline int jcos_polaris_stream_offset(JcosU32 field,JcosU64 span,JcosU32 *out){
 if(!out)return 0;*out=0;if(field>=POLARIS_STREAM_FIELDS)return 0;
 const JcosU32 dword[POLARIS_STREAM_FIELDS]={0x4CA4U,0x4CA5U,0x4C00U,0x1BBDU,0x1BBEU};
 JcosU32 offset=dword[field]*4U;if(span<4 || offset>span-4)return 0;*out=offset;return 1;
}
static inline int jcos_polaris_stream_read_admitted(const JcosU32 *v,JcosU32 count){
 return jcos_polaris_vtotal_snapshot_admitted(v,count);
}
/* Word0 status/version1;1..9 original timingV2;10 rawstream/FIFO;
 * 11 rawFE/updateLOCK;12 rawupdateMODE;13 diagnostic flags. */
#endif
