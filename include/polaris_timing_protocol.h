#ifndef JCOS_POLARIS_TIMING_PROTOCOL_H
#define JCOS_POLARIS_TIMING_PROTOCOL_H
#include "polaris_link_protocol.h"
#include "amd_polaris_registers.h"
#define POLARIS_TIMING_VERSION 2U
#define POLARIS_TIMING_FIELDS 14U
#define POLARIS_TIMING_VALUES 80U
#define POLARIS_TIMING_WORDS 9U
#define POLARIS_TIMING_STREAM_MASK 0x1U
#define POLARIS_TIMING_AUTO_MN_MASK 0x100U
#define POLARIS_TIMING_M_MASK 0xFFFFFFU
#define POLARIS_TIMING_COORD_MASK 0x3FFFU
#define POLARIS_TIMING_HIGH_COORD_MASK 0x3FFF0000U
#define POLARIS_TIMING_HIGH_COORD_SHIFT 16U
#define POLARIS_TIMING_POL_MASK 0x1U
#define POLARIS_TIMING_INTERLACE_MASK 0x1U
/* Fixed CRTC0/FE2 observation only, using the physically observed v35 route.
 * field0 is read first:DP stream enable must be set before the other13 fields.
 * Index0..5 CRTCbefore,6..25 DIGfirst,26..39 timingfirst,
 * 40..59 DIGsecond,60..73 timingsecond,74..79 CRTCafter. */
static inline int jcos_polaris_timing_offset(JcosU32 field,JcosU64 span,JcosU32 *out){
 if(!out)return 0;*out=0;
 if(field>=POLARIS_TIMING_FIELDS)return 0;
 const JcosU32 dword[POLARIS_TIMING_FIELDS]={0x4CA4,0x1B80,0x1B81,0x1B82,0x1B83,0x1B87,0x1B8D,0x1B8E,0x1B8F,0x1B9E,0x4CA1,0x4CA8,0x4CA9,0x4CAA};
 JcosU32 offset=dword[field]*4U;
 if(span<4 || offset>span-4)return 0;*out=offset;return 1;
}
/* Security admission for new read domains,not a modesetting policy/decoder.
 * No selection supplied by userspace. Requires exact already-observed route. */
static inline int jcos_polaris_timing_read_admitted(const JcosU32 *v,JcosU32 count){
 if(!v || count!=26)return 0;
 for(JcosU32 i=0;i<26;++i)if(v[i]==~0U)return 0;
 if((v[0]&(POLARIS_CRTC_MASTER_EN_MASK|POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK|POLARIS_CRTC_DISP_READ_REQUEST_DISABLE_MASK))!=(POLARIS_CRTC_MASTER_EN_MASK|POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK))return 0;
 for(JcosU32 i=1;i<6;++i)if(v[i]&(POLARIS_CRTC_MASTER_EN_MASK|POLARIS_CRTC_CURRENT_MASTER_EN_STATE_MASK))return 0;
 for(JcosU32 i=0;i<7;++i)if((v[6+i*2]&POLARIS_LINK_ENABLE_MASK)!=(i==2))return 0;
 return (v[11]&(POLARIS_LINK_MODE_MASK|POLARIS_LINK_FRONTEND_MASK))==(4U<<POLARIS_LINK_FRONTEND_SHIFT) &&
  (v[22]&(POLARIS_LINK_FE_CLOCK_MASK|POLARIS_LINK_CRTC_MASK))==POLARIS_LINK_FE_CLOCK_MASK;
}
typedef enum {POLARIS_TIMING_OK=0,POLARIS_TIMING_INVALID,POLARIS_TIMING_ALL_ONES,
 POLARIS_TIMING_CHANGED,POLARIS_TIMING_LINK,POLARIS_TIMING_UNSUPPORTED,
 POLARIS_TIMING_GEOMETRY} PolarisTimingStatus;
#endif
