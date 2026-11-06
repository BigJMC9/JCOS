#include "amd_polaris_plane.h"
int polaris_plane_recipe_step(const PolarisDisplayObservation *o,
    const PolarisPlaneWitness *w, JcosU32 pipe, JcosU32 index,
    JcosU64 *tag, JcosU32 *value) {
    if (tag) *tag=0;
    if (value) *value=0;
    if (!tag || !value) return 0;
    if (!o || !w || pipe>=POLARIS_DISPLAY_PIPE_COUNT || index>=POLARIS_PLANE_RECIPE_STEPS ||
        o->restrictions || o->depth!=2U || o->format ||
        o->array_mode>POLARIS_ARRAY_LINEAR_ALIGNED ||
        o->surface_width!=1024U || o->surface_height!=768U || o->pitch_pixels!=1024U ||
        o->linear_span_bytes!=0x300000ULL || (o->surface_gpu_address&255ULL) ||
        o->surface_gpu_address>(1ULL<<40)-o->linear_span_bytes) return 0;
    const JcosU32 taken=POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    for (JcosU32 sample=0; sample<2U; ++sample)
        if ((w->values[sample][POLARIS_WITNESS_UPDATE]&~taken) ||
            (w->values[sample][POLARIS_WITNESS_FLIP_CONTROL]&~POLARIS_GRPH_SURFACE_UPDATE_PENDING_MODE_MASK) ||
            (w->values[sample][POLARIS_WITNESS_FRAME_COUNT]&~POLARIS_CRTC_FRAME_COUNT_MASK)) return 0;
    /* Preserve the stable inherited pending-mode bit; never clear it to
     * manufacture completion. Recipe validation still executes no writes. */
    if (w->values[0][POLARIS_WITNESS_FLIP_CONTROL]!=w->values[1][POLARIS_WITNESS_FLIP_CONTROL]) return 0;
    JcosU32 frames=(w->values[1][POLARIS_WITNESS_FRAME_COUNT]-w->values[0][POLARIS_WITNESS_FRAME_COUNT])&POLARIS_CRTC_FRAME_COUNT_MASK;
    if (!frames || frames>512U) return 0; /* finite observation, not a refresh-rate claim */
    JcosU32 operation,field;
    if (index<2U) {
        operation=POLARIS_PLANE_WRITE;
        field=index ? POLARIS_REG_PRIMARY_LOW : POLARIS_REG_PRIMARY_HIGH;
        *value=index ? (JcosU32)o->surface_gpu_address : (JcosU32)(o->surface_gpu_address>>32);
    } else if (index==2U) {
        operation=POLARIS_PLANE_POST_READ; field=POLARIS_REG_PRIMARY_LOW;
        *value=(JcosU32)o->surface_gpu_address;
    } else if (index==3U) {
        operation=POLARIS_PLANE_CHECK_IDLE; field=POLARIS_WITNESS_UPDATE;
    } else {
        operation=POLARIS_PLANE_CHECK_FRAME; field=POLARIS_WITNESS_FRAME_COUNT;
        *value=w->values[1][POLARIS_WITNESS_FRAME_COUNT];
    }
    *tag=jcos_polaris_plane_tag(index,operation,pipe,field);
    return *tag!=0;
}
