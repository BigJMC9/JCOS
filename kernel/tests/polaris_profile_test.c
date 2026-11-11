#include "polaris_profile_test.h"
#include "polaris_profile.h"
#include "lib.h"
static bool zero(const void *p,u32 size){const u8 *b=p;for(u32 i=0;i<size;++i)if(b[i])return false;return true;}
bool polaris_profile_test(void){
 const PolarisHardwareProfile *p=polaris_profile_qualified();
 /* Independent regression constants from the accepted physical target; these
  * also guard extraction errors that self-consistent consumers would miss. */
 if(!polaris_profile_valid(p) || p->id!=1 || p->dce_version!=112 ||
  p->board.vendor!=0x1002 || p->board.device!=0x67DF || p->board.revision!=0xEF ||
  p->board.subvendor!=0x1458 || p->board.subdevice!=0x2310 ||
  p->surface.cpu_base!=0x50000000ULL || p->surface.gpu_address!=0xF400000000ULL ||
  p->surface.bytes!=0x300000 || p->surface.width!=1024 || p->surface.height!=768 ||
  p->surface.stride!=1024 || p->surface.format!=1 || p->surface.grph_control!=0x0A10800A ||
  p->access.mmio_bar!=5 || p->access.mmio_bytes!=0x40000 || p->access.rebind_page!=0x6000 ||
  p->access.link_pages[0]!=0x12000 || p->access.link_pages[1]!=0x13000 || p->access.link_pages[2]!=0x15000 ||
  p->pipeline.pipe || p->pipeline.crtc_control!=0x80410311 || p->pipeline.target_vtotal!=1132 ||
  p->pipeline.vertical_control!=0x11 || p->pipeline.vertical_report_head!=0x10000)return false;
 const u64 tail[8]={0,0x46C00000000ULL,0x46C00000000ULL,0x46C00000001ULL,0x1100000002ULL,2,0,1};
 for(u32 n=0;n<8;++n)if(p->pipeline.vertical_report_tail[n]!=tail[n])return false;
 for(u32 n=0;n<3;++n)if(p->pipeline.original_vertical[n])return false;
 PciDevice device;DisplayProbeInfo info;k_memset(&device,0,sizeof(device));k_memset(&info,0,sizeof(info));
 device.vendor_id=0x1002;device.device_id=0x67DF;device.revision=0xEF;
 info.subsystem_vendor_id=0x1458;info.subsystem_device_id=0x2310;
 if(polaris_profile_detect(&device,&info)!=p || polaris_profile_detect(0,&info) || polaris_profile_detect(&device,0))return false;
 for(u32 n=0;n<5;++n){
  PciDevice d;DisplayProbeInfo i;k_memcpy(&d,&device,sizeof(d));k_memcpy(&i,&info,sizeof(i));
  if(n==0)d.vendor_id^=1;if(n==1)d.device_id^=1;if(n==2)d.revision^=1;
  if(n==3)i.subsystem_vendor_id^=1;if(n==4)i.subsystem_device_id^=1;
  if(polaris_profile_detect(&d,&i))return false;
 }
 PolarisHardwareProfile forged;k_memcpy(&forged,p,sizeof(forged));
 const PolarisHardwareProfile *bad[3]={0,(const PolarisHardwareProfile*)1,&forged};
 FramebufferInfo f;k_memset(&f,0,sizeof(f));f.direct_map_safe=true;f.physical_base=0x50000000;
 f.size=0x300000;f.width=1024;f.height=768;f.pixels_per_scanline=1024;f.pixel_format=1;
 PolarisDisplayRuntime r;
 if(!polaris_display_runtime_init(p,&f,&r) || r.profile!=p || r.surface_observed ||
  !polaris_surface_runtime_valid(&r.surface) || !zero(&r.observation,sizeof(r.observation)) || !zero(&r.aperture,sizeof(r.aperture)))return false;
 /* Distinct retained copies: later caller changes cannot rewrite rescue state. */
 f.width=640;if(r.framebuffer.width!=1024)return false;f.width=1024;
 for(u32 n=0;n<3;++n){
  PolarisSurfaceRuntime surface;k_memset(&surface,0xA5,sizeof(surface));k_memset(&r,0xA5,sizeof(r));
  if(polaris_profile_valid(bad[n]) || polaris_surface_runtime_init(bad[n],0x50000000,0x300000,&surface) ||
   !zero(&surface,sizeof(surface)) || polaris_display_runtime_init(bad[n],&f,&r) || !zero(&r,sizeof(r)))return false;
 }
 for(u32 n=0;n<8;++n){
  FramebufferInfo changed;k_memcpy(&changed,&f,sizeof(changed));
  if(n==0)changed.direct_map_safe=false;if(n==1)++changed.physical_base;if(n==2)++changed.size;
  if(n==3)++changed.width;if(n==4)++changed.height;if(n==5)++changed.pixels_per_scanline;
  if(n==6)changed.pixel_format=0;if(n==7)changed.physical_base=0x60000000;
  k_memset(&r,0xA5,sizeof(r));if(polaris_display_runtime_init(p,&changed,&r) || !zero(&r,sizeof(r)))return false;
 }
 for(u32 n=0;n<3;++n){
  PolarisSurfaceRuntime surface;k_memset(&surface,0xA5,sizeof(surface));
  u64 cpu=n==0?0x50000001ULL:n==1?0xFFFFFFFFFFFFF000ULL:0x50000000ULL;
  if(polaris_surface_runtime_init(p,cpu,n==2?0x301000:0x300000,&surface) || !zero(&surface,sizeof(surface)))return false;
 }
 /* RAM-backed reservations remain testable without implying live VRAM. */
 PolarisSurfaceRuntime surface;
 if(!polaris_surface_runtime_init(p,0x200000,0x300000,&surface) || !polaris_surface_runtime_valid(&surface))return false;
 for(u32 n=0;n<7;++n){
  PolarisSurfaceRuntime changed;k_memcpy(&changed,&surface,sizeof(changed));
  if(n==0)changed.profile=(const PolarisHardwareProfile*)1;if(n==1)++changed.cpu_base;
  if(n==2)++changed.bytes;if(n==3)++changed.gpu_address;if(n==4)++changed.pipe;
  if(n==5)++changed.crtc_control;if(n==6)changed.cpu_base=0xFFFFFFFFFFFFF000ULL;
  if(polaris_surface_runtime_valid(&changed))return false;
 }
 return !polaris_surface_runtime_valid(0) && !polaris_display_runtime_init(p,0,&r) && zero(&r,sizeof(r)) &&
  !polaris_surface_runtime_init(p,0x50000000,0x300000,0) && !polaris_display_runtime_init(p,&f,0);
}
