#include "polaris_plane.h"
#include "lib.h"
u32 polaris_plane_blockers(const PolarisPlaneWitness *w) {
    if (!w) return POLARIS_PLANE_BLOCK_ABSENT;
    u32 blocked=0;
    const u32 taken=POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    for (u32 s=0;s<2U;++s) {
        for (u32 f=0;f<POLARIS_PLANE_WITNESS_COUNT;++f)
            if (w->values[s][f]==~0U) blocked|=POLARIS_PLANE_BLOCK_ABSENT;
        if (w->values[s][POLARIS_WITNESS_UPDATE]&~taken) blocked|=POLARIS_PLANE_BLOCK_UPDATE;
        /* Pending-mode is preserved data, not a completion certificate.
         * No immediate/retrace/stereo/unknown control is admitted. */
        if (w->values[s][POLARIS_WITNESS_FLIP_CONTROL]&~POLARIS_GRPH_SURFACE_UPDATE_PENDING_MODE_MASK)
            blocked|=POLARIS_PLANE_BLOCK_FLIP;
        if (w->values[s][POLARIS_WITNESS_FRAME_COUNT]&~POLARIS_CRTC_FRAME_COUNT_MASK)
            blocked|=POLARIS_PLANE_BLOCK_PROGRESS;
    }
    if (w->values[0][POLARIS_WITNESS_FLIP_CONTROL]!=w->values[1][POLARIS_WITNESS_FLIP_CONTROL])
        blocked|=POLARIS_PLANE_BLOCK_FLIP;
    u32 frames=(w->values[1][POLARIS_WITNESS_FRAME_COUNT]-w->values[0][POLARIS_WITNESS_FRAME_COUNT])&POLARIS_CRTC_FRAME_COUNT_MASK;
    if (!frames || frames>512U) blocked|=POLARIS_PLANE_BLOCK_PROGRESS;
    return blocked;
}
bool polaris_plane_preview_init(PolarisPlanePreview *p,u64 incarnation,
    u64 observation_id,u64 gpu,u32 pipe,const PolarisPlaneWitness *w) {
    if (!p) return false;
    k_memset(p,0,sizeof(*p)); p->closed=true;
    if (!incarnation || !observation_id || pipe>=POLARIS_DISPLAY_PIPE_COUNT ||
        (gpu&255ULL) || gpu>(1ULL<<40)-0x300000ULL || polaris_plane_blockers(w)) return false;
    p->incarnation=incarnation; p->observation_id=observation_id; p->gpu_address=gpu;
    p->pipe=pipe; p->frame_count=w->values[1][POLARIS_WITNESS_FRAME_COUNT];
    p->closed=false; return true;
}
bool polaris_plane_preview_accept(PolarisPlanePreview *p,u64 incarnation,
    u64 observation_id,u64 tag,u64 value) {
    if (!p || p->closed) return false;
    if (!p->incarnation || !p->observation_id || p->pipe>=POLARIS_DISPLAY_PIPE_COUNT ||
        (p->gpu_address&255ULL) || p->gpu_address>(1ULL<<40)-0x300000ULL ||
        p->frame_count>POLARIS_CRTC_FRAME_COUNT_MASK ||
        p->next>=POLARIS_PLANE_RECIPE_STEPS || p->incarnation!=incarnation ||
        p->observation_id!=observation_id) { p->closed=true; return false; }
    /* Independent trusted comparison against the retained surface. Never
     * execute a raw user tag/value or derive the expected address from it. */
    static const u32 operation[5]={1,1,2,3,4};
    static const u32 field[5]={POLARIS_REG_PRIMARY_HIGH,POLARIS_REG_PRIMARY_LOW,
        POLARIS_REG_PRIMARY_LOW,POLARIS_WITNESS_UPDATE,POLARIS_WITNESS_FRAME_COUNT};
    u32 expected=p->next==0U ? (u32)(p->gpu_address>>32) :
        p->next<=2U ? (u32)p->gpu_address : p->next==3U ? 0U : p->frame_count;
    if (tag!=jcos_polaris_plane_tag(p->next,operation[p->next],p->pipe,field[p->next]) ||
        value!=expected) { p->closed=true; return false; }
    if (++p->next==POLARIS_PLANE_RECIPE_STEPS) p->closed=true;
    return true;
}
