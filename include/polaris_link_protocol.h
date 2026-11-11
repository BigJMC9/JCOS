#ifndef JCOS_POLARIS_LINK_PROTOCOL_H
#define JCOS_POLARIS_LINK_PROTOCOL_H
#include "user_abi.h"
/* Pinned DCE11.2 register facts and copied-data contract;no access authority. */
#define POLARIS_LINK_BACKENDS 7U
#define POLARIS_LINK_FRONTENDS 6U
#define POLARIS_LINK_FIELDS 3U
#define POLARIS_LINK_VALUES 52U
#define POLARIS_LINK_WORDS 4U
#define POLARIS_LINK_VERSION 1U
#define POLARIS_LINK_PAGES 3U
#define POLARIS_LINK_ENABLE_MASK 0x1U
#define POLARIS_LINK_FRONTEND_MASK 0x7F00U
#define POLARIS_LINK_FRONTEND_SHIFT 8U
#define POLARIS_LINK_MODE_MASK 0x70000U
#define POLARIS_LINK_MODE_SHIFT 16U
#define POLARIS_LINK_HPD_MASK 0x70000000U
#define POLARIS_LINK_HPD_SHIFT 28U
#define POLARIS_LINK_CRTC_MASK 0x7U
#define POLARIS_LINK_FE_CLOCK_MASK 0x1000000U
typedef enum {POLARIS_LINK_ENABLE=0,POLARIS_LINK_BACKEND,POLARIS_LINK_FRONTEND} PolarisLinkRegister;
typedef enum {
 POLARIS_LINK_OK=0,POLARIS_LINK_INVALID,POLARIS_LINK_ALL_ONES,POLARIS_LINK_CHANGED,
 POLARIS_LINK_NO_CRTC,POLARIS_LINK_CRTC_AMBIGUOUS,POLARIS_LINK_SELECTOR,
 POLARIS_LINK_NO_BACKEND,POLARIS_LINK_BACKEND_AMBIGUOUS,POLARIS_LINK_CRTC_UNSUPPORTED
} PolarisLinkStatus;
static inline int jcos_polaris_link_offset(JcosU32 unit,PolarisLinkRegister field,JcosU64 span,JcosU32 *out){
 if(!out)return 0;*out=0;
 if(unit>=POLARIS_LINK_BACKENDS || (JcosU32)field>=POLARIS_LINK_FIELDS || (field==POLARIS_LINK_FRONTEND && unit>=POLARIS_LINK_FRONTENDS))return 0;
 const JcosU32 delta[3]={0x48U,0x47U,0};
 JcosU32 offset=((unit==6 ? 0x5400U:0x4A00U+unit*0x100U)+delta[field])*4U;
 if(span<4 || offset>span-4)return 0;*out=offset;return 1;
}
/* Index0..5 CRTCbefore;6..19 first seven [enable,BE] pairs;20..25 first
 * six FE controls;26..45 repeated DIG values;46..51 CRTCafter.
 * Every word is a copied uint32,never an address. */
#endif