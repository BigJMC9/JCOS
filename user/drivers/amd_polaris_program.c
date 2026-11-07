#include "amd_polaris_program.h"
static int read_value(const PolarisUserProgramIo *io,JcosU32 field,JcosU32 *out) {
    *out=0;
    return io->exchange(io->context,POLARIS_REBIND_READ,field,0,out) && *out!=~0U;
}
int polaris_user_program(const PolarisUserProgramIo *io,JcosU32 flip,JcosU32 *delta) {
    if(!delta)return 0;
    *delta=0;
    if(!io || !io->exchange)return 0;
    JcosU32 before[6],unused;
    for(JcosU32 f=0;f<6;++f)if(!read_value(io,f,&before[f]))return 0;
    const JcosU32 taken=POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    if(before[0] || before[1]!=0xF4U || before[2]!=0x80410311U ||
       (before[3]&~taken) || before[4]!=flip || (before[5]&~POLARIS_CRTC_FRAME_COUNT_MASK))return 0;
    if(!io->exchange(io->context,POLARIS_REBIND_WRITE,POLARIS_REG_PRIMARY_HIGH,0xF4U,&unused) ||
       !io->exchange(io->context,POLARIS_REBIND_WRITE,POLARIS_REG_PRIMARY_LOW,0,&unused))return 0;
    JcosU32 posted,baseline;
    if(!read_value(io,0,&posted) || posted || !read_value(io,5,&baseline) ||
       (baseline&~POLARIS_CRTC_FRAME_COUNT_MASK))return 0;
    const JcosU32 allowed=taken|POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK;
    for(JcosU32 poll=0;poll<POLARIS_REBIND_POLLS;++poll) {
        JcosU32 v[6];
        for(JcosU32 f=0;f<6;++f)if(!read_value(io,f,&v[f]))return 0;
        if(v[0] || v[1]!=0xF4U || v[2]!=0x80410311U || (v[3]&~allowed) ||
           v[4]!=flip || (v[5]&~POLARIS_CRTC_FRAME_COUNT_MASK))return 0;
        JcosU32 progress=(v[5]-baseline)&POLARIS_CRTC_FRAME_COUNT_MASK;
        if(progress>512U)return 0;
        if(progress>=2U && !(v[3]&(POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK))) {
            *delta=progress;return 1;
        }
        if(poll+1U<POLARIS_REBIND_POLLS &&
           !io->exchange(io->context,POLARIS_REBIND_WAIT,0,0,&unused))return 0;
    }
    return 0;
}
