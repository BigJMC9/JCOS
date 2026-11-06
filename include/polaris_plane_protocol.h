#ifndef JCOS_POLARIS_PLANE_PROTOCOL_H
#define JCOS_POLARIS_PLANE_PROTOCOL_H
#include "amd_polaris_registers.h"
#define POLARIS_PLANE_RECIPE_VERSION 1ULL
#define POLARIS_PLANE_RECIPE_STEPS 5U
#define POLARIS_PLANE_WRITE 1U
#define POLARIS_PLANE_POST_READ 2U
#define POLARIS_PLANE_CHECK_IDLE 3U
#define POLARIS_PLANE_CHECK_FRAME 4U
/* Scalar recipe data only, never an executable GPU grant or raw offset.
 * 0=write primary high;1=write primary low (triggers);2=post-read low;
 * 3=check update idle;4=check later frame against retained frame count.
 * Actual polling bounds, effectors, leases and hardware proof are separate. */
typedef struct {
    JcosU32 values[2][POLARIS_PLANE_WITNESS_COUNT];
} PolarisPlaneWitness;
static inline JcosU64 jcos_polaris_plane_tag(JcosU32 index, JcosU32 operation,
    JcosU32 pipe, JcosU32 field) {
    if (index >= POLARIS_PLANE_RECIPE_STEPS || pipe >= POLARIS_DISPLAY_PIPE_COUNT ||
        !operation || operation > POLARIS_PLANE_CHECK_FRAME ||
        field >= (operation <= POLARIS_PLANE_POST_READ ? POLARIS_DISPLAY_REGISTER_COUNT :
                  POLARIS_PLANE_WITNESS_COUNT)) return 0;
    return (POLARIS_PLANE_RECIPE_VERSION << 56) | ((JcosU64)index << 40) |
        ((JcosU64)operation << 32) | ((JcosU64)pipe << 16) | field;
}
#endif
