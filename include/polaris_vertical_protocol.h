#ifndef JCOS_POLARIS_VERTICAL_PROTOCOL_H
#define JCOS_POLARIS_VERTICAL_PROTOCOL_H
#include "polaris_timing_protocol.h"
/* Read-only preparation for a future fixed vertical-total experiment.
 * Existing 80 timing values, then two [MIN,MAX,CONTROL] samples. No write grant. */
#define POLARIS_VERTICAL_VERSION 1U
#define POLARIS_VERTICAL_FIELDS 3U
#define POLARIS_VERTICAL_VALUES 86U
#define POLARIS_VERTICAL_WORDS 18U
#define POLARIS_VERTICAL_COORD_MASK 0x3FFFU
#define POLARIS_VERTICAL_MIN_SEL_MASK 0x1U
#define POLARIS_VERTICAL_MAX_SEL_MASK 0x10U
#define POLARIS_VERTICAL_BASE_TOTAL 1125U
#define POLARIS_VERTICAL_TARGET_TOTAL 1133U
typedef enum {POLARIS_VERTICAL_MIN=0,POLARIS_VERTICAL_MAX,POLARIS_VERTICAL_CONTROL} PolarisVerticalField;
typedef enum {POLARIS_VERTICAL_OK=0,POLARIS_VERTICAL_INVALID,POLARIS_VERTICAL_ALL_ONES,
 POLARIS_VERTICAL_CHANGED,POLARIS_VERTICAL_TIMING,POLARIS_VERTICAL_PROFILE,POLARIS_VERTICAL_CONTROLS} PolarisVerticalStatus;
static inline int jcos_polaris_vertical_offset(JcosU32 field,JcosU64 span,JcosU32 *out){
 if(!out)return 0;*out=0;
 if(field>=POLARIS_VERTICAL_FIELDS)return 0;
 JcosU32 offset=(0x1B88U+field)*4U;
 if(span<4 || offset>span-4)return 0;*out=offset;return 1;
}
/* Security admission before the six new reads; no vendor mode policy. */
static inline int jcos_polaris_vertical_read_admitted(const JcosU32 *v,JcosU32 count){
 if(!v || count!=POLARIS_TIMING_VALUES)return 0;
 if(!jcos_polaris_timing_read_admitted(v,26))return 0;
 for(JcosU32 i=0;i<count;++i)if(v[i]==~0U)return 0;
 for(JcosU32 i=0;i<6;++i)if(v[i]!=v[74+i])return 0;
 for(JcosU32 i=0;i<33;++i)if(v[6+i]!=v[40+i])return 0;
 return (v[26]&POLARIS_TIMING_STREAM_MASK) && (v[60]&POLARIS_TIMING_STREAM_MASK);
}
/* Word0=status/version1. Words1..9 retain the entire timing V2 report.
 * Word10=original MIN low32/MAX high32;11=original CONTROL/target total-minus-one.
 * Words12..14=proposed primary MIN,MAX,CONTROL;15..17=restoration CONTROL,MIN,MAX.
 * Each proposal is a semantic field in low32 and value in high32. These are
 * immutable copied data, never offsets, executable commands or access authority.
 * Restoration data must be retained independently before any future mutation. */
#endif
