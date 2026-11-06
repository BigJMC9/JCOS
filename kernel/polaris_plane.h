#ifndef JA_OS_POLARIS_PLANE_H
#define JA_OS_POLARIS_PLANE_H
#include "types.h"
#include "polaris_plane_protocol.h"
#define POLARIS_PLANE_BLOCK_ABSENT 1U
#define POLARIS_PLANE_BLOCK_UPDATE 2U
#define POLARIS_PLANE_BLOCK_FLIP 4U
#define POLARIS_PLANE_BLOCK_PROGRESS 8U
typedef struct {
    u64 incarnation,observation_id,gpu_address;
    u32 pipe,frame_count,next;
    bool closed;
} PolarisPlanePreview;
/* Pure state/sequence validation. No access rights, mapping or effectors.
 * Caller supplies independently pinned surface/pipe and copied witnesses. */
u32 polaris_plane_blockers(const PolarisPlaneWitness *witness);
bool polaris_plane_preview_init(PolarisPlanePreview *preview,u64 incarnation,
    u64 observation_id,u64 surface_gpu_address,u32 pipe,const PolarisPlaneWitness *witness);
bool polaris_plane_preview_accept(PolarisPlanePreview *preview,u64 incarnation,
    u64 observation_id,u64 tag,u64 value);
#endif
