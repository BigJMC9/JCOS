#ifndef JAOS_POLARIS_PROFILE_H
#define JAOS_POLARIS_PROFILE_H
#include "display_probe.h"
#include "framebuffer.h"
#include "polaris_vram.h"
/* Trusted compiled configuration. Pointer identity is checked before dereference.
 * Adding a descriptor does not qualify a target or create a register grant. */
typedef struct {
 u16 vendor,device,subvendor,subdevice;
 u8 revision;
} PolarisBoardIdentity;
typedef struct {
 u64 cpu_base,gpu_address,bytes;
 u32 width,height,stride,format,grph_control;
} PolarisSurfaceProfile;
typedef struct {
 u32 mmio_bar,rebind_page,link_pages[3];
 u64 mmio_bytes;
} PolarisAccessProfile;
typedef struct {
 u32 pipe,crtc_control,target_vtotal,vertical_control; /* target_vtotal is the encoded total minus one. */
 u32 original_vertical[3];
 u64 vertical_report_head,vertical_report_tail[8];
} PolarisPipelineProfile;
typedef struct {
 u32 id,dce_version;
 PolarisBoardIdentity board;
 PolarisSurfaceProfile surface;
 PolarisAccessProfile access;
 PolarisPipelineProfile pipeline;
} PolarisHardwareProfile;
/* Checked CPU backing/layout for a driver reservation, including RAM fixtures.
 * Expected GPU address is not evidence of a live mapping or a VRAM allocation.
 * No ownership, assignment, DMA or write authority is created by this object. */
typedef struct {
 const PolarisHardwareProfile *profile;
 u64 cpu_base,bytes,gpu_address;
 u32 pipe,crtc_control;
} PolarisSurfaceRuntime;
/* Independently retained kernel observations; never borrowed from Ring3. */
typedef struct {
 const PolarisHardwareProfile *profile;
 FramebufferInfo framebuffer;
 BootDisplayBar aperture;
 PolarisSurfaceRuntime surface;
 PolarisVramObservation observation;
 bool surface_observed;
} PolarisDisplayRuntime;
const PolarisHardwareProfile *polaris_profile_qualified(void);
bool polaris_profile_valid(const PolarisHardwareProfile *profile);
/* Inventory match only: selected GOP provenance, live PCI/BAR/PM, ownership,
 * CPU permissions and operation admission remain independently enforced. */
const PolarisHardwareProfile *polaris_profile_detect(const PciDevice *device,const DisplayProbeInfo *info);
bool polaris_surface_runtime_init(const PolarisHardwareProfile *profile,u64 cpu_base,u64 bytes,PolarisSurfaceRuntime *out);
bool polaris_surface_runtime_valid(const PolarisSurfaceRuntime *runtime);
bool polaris_display_runtime_init(const PolarisHardwareProfile *profile,const FramebufferInfo *info,PolarisDisplayRuntime *out);
#endif
