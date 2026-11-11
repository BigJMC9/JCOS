#include "polaris_profile.h"
#include "lib.h"
/* Sole authorized physical profile. Fixed bring-up values deliberately remain
 * fixed until each dynamic layout/mode/memory/reuse contract is implemented. */
static const PolarisHardwareProfile rx570_gigabyte_dp={
 .id=1,.dce_version=112,
 .board={.vendor=0x1002,.device=0x67DF,.subvendor=0x1458,.subdevice=0x2310,.revision=0xEF},
 .surface={.cpu_base=0x50000000ULL,.gpu_address=0xF400000000ULL,.bytes=0x300000ULL,
  .width=1024,.height=768,.stride=1024,.format=1,.grph_control=0x0A10800AU},
 .access={.mmio_bar=5,.rebind_page=0x6000,.link_pages={0x12000,0x13000,0x15000},.mmio_bytes=0x40000ULL},
 .pipeline={.pipe=0,.crtc_control=0x80410311U,.target_vtotal=1132,.vertical_control=0x11,
  .original_vertical={0,0,0},.vertical_report_head=0x10000ULL,
  .vertical_report_tail={0,0x46C00000000ULL,0x46C00000000ULL,0x46C00000001ULL,0x1100000002ULL,2,0,1}}
};
const PolarisHardwareProfile *polaris_profile_qualified(void){return &rx570_gigabyte_dp;}
bool polaris_profile_valid(const PolarisHardwareProfile *p){return p==&rx570_gigabyte_dp;}
const PolarisHardwareProfile *polaris_profile_detect(const PciDevice *d,const DisplayProbeInfo *i){
 const PolarisHardwareProfile *p=&rx570_gigabyte_dp;
 if(!d || !i || d->vendor_id!=p->board.vendor || d->device_id!=p->board.device ||
  d->revision!=p->board.revision || i->subsystem_vendor_id!=p->board.subvendor ||
  i->subsystem_device_id!=p->board.subdevice)return 0;
 return p;
}
bool polaris_surface_runtime_init(const PolarisHardwareProfile *p,u64 cpu,u64 bytes,PolarisSurfaceRuntime *out){
 if(!out)return false;k_memset(out,0,sizeof(*out));
 if(!polaris_profile_valid(p) || (cpu&0xFFFULL) || bytes!=p->surface.bytes || bytes>~0ULL-cpu)return false;
 out->profile=p;out->cpu_base=cpu;out->bytes=bytes;out->gpu_address=p->surface.gpu_address;
 out->pipe=p->pipeline.pipe;out->crtc_control=p->pipeline.crtc_control;return true;
}
bool polaris_surface_runtime_valid(const PolarisSurfaceRuntime *r){
 return r && polaris_profile_valid(r->profile) && !(r->cpu_base&0xFFFULL) &&
  r->bytes==r->profile->surface.bytes && r->bytes<=~0ULL-r->cpu_base &&
  r->gpu_address==r->profile->surface.gpu_address && r->pipe==r->profile->pipeline.pipe &&
  r->crtc_control==r->profile->pipeline.crtc_control;
}
bool polaris_display_runtime_init(const PolarisHardwareProfile *p,const FramebufferInfo *i,PolarisDisplayRuntime *out){
 if(!out)return false;k_memset(out,0,sizeof(*out));
 if(!polaris_profile_valid(p) || !i || !i->direct_map_safe || i->physical_base!=p->surface.cpu_base ||
  i->size!=p->surface.bytes || i->width!=p->surface.width || i->height!=p->surface.height ||
  i->pixels_per_scanline!=p->surface.stride || i->pixel_format!=p->surface.format)return false;
 if(!polaris_surface_runtime_init(p,i->physical_base,i->size,&out->surface))return false;
 out->profile=p;out->framebuffer=*i;return true;
}
