#include "polaris_program.h"
#include "lib.h"
static bool load(const PolarisProgramIo *io,PolarisRebindRead field,u32 *v,PolarisProgramResult *r) {
    *v=0;
    if (!io->admitted(io->context) || !io->read(io->context,field,v)) return false;
    ++r->reads; return *v!=~0U;
}
bool polaris_program_execute(PolarisProgramPlan *p,const PolarisProgramIo *io,PolarisProgramResult *r) {
    if (!r) return false;
    k_memset(r,0,sizeof(*r));
    if (!p || p->consumed) return false;
    p->consumed=true;
    if (!io || !io->admitted || !io->write || !io->read || !io->wait ||
        p->pipe!=0U || p->gpu_address!=0xF400000000ULL || p->crtc!=0x80410311U) return false;
    PolarisPlanePreview preview;
    if (!polaris_plane_preview_init(&preview,p->incarnation,p->observation_id,p->gpu_address,p->pipe,&p->witness)) return false;
    for (u32 i=0;i<5;++i)
        if (!polaris_plane_preview_accept(&preview,p->incarnation,p->observation_id,p->tags[i],p->values[i])) return false;
    u32 before[6];
    for (u32 f=0;f<6;++f) if (!load(io,(PolarisRebindRead)f,&before[f],r)) return false;
    const u32 taken=POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    if (before[0]!=(u32)p->gpu_address || before[1]!=(u32)(p->gpu_address>>32) ||
        before[2]!=p->crtc || (before[3]&~taken) ||
        before[4]!=p->witness.values[1][POLARIS_WITNESS_FLIP_CONTROL] ||
        (before[5]&~POLARIS_CRTC_FRAME_COUNT_MASK)) return false;
    for (u32 i=0;i<2;++i) {
        if (!io->admitted(io->context)) return false;
        ++r->write_attempts; /* Record before the potentially ambiguous store. */
        if (!io->write(io->context,i ? POLARIS_REG_PRIMARY_LOW : POLARIS_REG_PRIMARY_HIGH,(u32)p->values[i])) return false;
    }
    u32 posted,baseline;
    if (!load(io,POLARIS_REBIND_LOW,&posted,r) || posted!=(u32)p->gpu_address ||
        !load(io,POLARIS_REBIND_FRAME,&baseline,r) || (baseline&~POLARIS_CRTC_FRAME_COUNT_MASK)) return false;
    const u32 allowed=POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|
        POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    for (u32 poll=0;poll<POLARIS_PROGRAM_POLLS;++poll) {
        u32 v[6]; ++r->polls;
        for (u32 f=0;f<6;++f) if (!load(io,(PolarisRebindRead)f,&v[f],r)) return false;
        if (v[POLARIS_REBIND_LOW]!=(u32)p->gpu_address ||
            v[POLARIS_REBIND_HIGH]!=(u32)(p->gpu_address>>32) ||
            v[POLARIS_REBIND_CRTC]!=p->crtc || (v[POLARIS_REBIND_UPDATE]&~allowed) ||
            v[POLARIS_REBIND_FLIP]!=p->witness.values[1][POLARIS_WITNESS_FLIP_CONTROL] ||
            (v[POLARIS_REBIND_FRAME]&~POLARIS_CRTC_FRAME_COUNT_MASK)) return false;
        u32 delta=(v[POLARIS_REBIND_FRAME]-baseline)&POLARIS_CRTC_FRAME_COUNT_MASK;
        if (delta>512U) return false;
        if (delta>=2U && !(v[POLARIS_REBIND_UPDATE]&
                (POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK))) {
            r->frame_delta=delta; r->complete=true; return true;
        }
        if (poll+1U<POLARIS_PROGRAM_POLLS && !io->wait(io->context)) return false;
    }
    return false;
}
