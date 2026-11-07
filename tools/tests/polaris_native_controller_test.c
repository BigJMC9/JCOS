/* Production orchestration; broker service double uses the retained reference
 * executor. Actual Ring3/broker/gate coverage is separate. No hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/polaris_native.h"
#include "../../kernel/polaris_driver.h"
#include "../../kernel/graphics_supervisor.h"
#include "../../user/graphics_policy.h"
#include "../../kernel/polaris_mmio.h"
#include "../../kernel/polaris_read_profile.h"
#include "../../kernel/display.h"
#include "../../kernel/framebuffer.h"
#include "../../kernel/device_mmio.h"
#include "../../user/drivers/amd_polaris_plane.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"native controller case%d line%d %s\n",fault,__LINE__,#x);exit(1);}}while(0)
static int fault;
static PciDevice device;
static Process supervisor;
static FramebufferInfo info;
static DisplayLeaseState lease=DISPLAY_LEASE_KERNEL;
static bool retired,render=true,binding,held,enabled,armed,reserved[3],drawing[3],output_valid,quarantined;
static unsigned writes[2],creates,loads,frame_counter=100,pings[3],diagnostics[3],progress_calls;
static bool busy[3],clock_changed,irq_lost;
static u64 ticks,output,writer;
static PolarisDriver *drivers[3];
static GraphicsPolicy policy;
static u64 policy_pid=500,policy_tid=501;
static bool policy_busy,policy_dead,policy_spinning;
static unsigned policy_injections;
bool supervisor_stop_identity(u64 pid,u64 tid){
 if(pid==policy_pid&&tid==policy_tid){policy_dead=true;policy_spinning=false;}
 return true;
}
bool supervisor_idle(void);
bool supervisor_running(void){return fault!=57&&!policy_busy&&!policy_dead;}
bool supervisor_test_fault(void){++policy_injections;if(fault==75||fault==77){policy_dead=true;return true;}return false;}
bool supervisor_test_spin(void){++policy_injections;if(fault==76||fault==78){policy_spinning=true;return true;}return false;}
bool supervisor_idle(void){return supervisor_running()&&!policy_spinning;}
u64 supervisor_process_id(void){return policy_pid;}
u64 supervisor_thread_id(void){return policy_tid;}
bool supervisor_graphics_exchange(u64 session,u64 sequence,u64 snapshot,u64 *out){
 if(policy_spinning)return false;
 u64 event=snapshot&0xFFULL;
 if((fault==58&&event==GRAPHICS_POLICY_BEGIN)||(fault==59&&event==GRAPHICS_POLICY_PRIMARY)||
    (fault==60&&event==GRAPHICS_POLICY_OBSERVE)||(fault==65&&event==GRAPHICS_POLICY_CLOSE)||
    (fault==66&&event==GRAPHICS_POLICY_RESCUED))return false;
 *out=graphics_policy_decide(&policy,session,sequence,snapshot);
 if(fault==71&&event==GRAPHICS_POLICY_BEGIN)*out=GRAPHICS_POLICY_KEEP;
 if(event==GRAPHICS_POLICY_OBSERVE){
   if(fault==62||fault==70)*out=GRAPHICS_POLICY_KEEP;
   if(fault==63||fault==64)*out=GRAPHICS_POLICY_RECOVER;
   if(fault==68)*out=GRAPHICS_POLICY_HEADLESS;
   if(fault==72)*out=GRAPHICS_POLICY_REJECT;
   if(fault==73)++policy_pid;
 }
 return true;
}

Process *process_kernel(void){return &supervisor;}
void arch_pause(void){}
bool serial_available(void){return true;}
bool scheduler_preemption_enabled(void){return true;}
bool timer_initialized(void){return true;}
u32 timer_frequency(void){return clock_changed ? 1001 : 1000;}
u64 timer_ticks(void){return ++ticks;}
bool device_mmio_interrupts_enabled(void){return !irq_lost;}
void terminal_write(const char *s){(void)s;}
void terminal_writeln(const char *s){(void)s;}
void terminal_write_u64(u64 n){(void)n;}
void terminal_write_hex(u64 n){(void)n;}
void terminal_putchar(char c){(void)c;}
void terminal_set_render_enabled(bool b){render=b;}
bool terminal_render_enabled(void){return render;}
bool framebuffer_firmware_valid(void){return !retired;}
bool framebuffer_writers_idle(void){return true;}
bool framebuffer_info(FramebufferInfo *out){*out=info;return fault!=2;}
bool framebuffer_native_install(const FramebufferInfo *i,bool rescue,u64 *id){
 CHECK(retired && lease==DISPLAY_LEASE_KERNEL && i->physical_base==info.physical_base);
 if (fault==(rescue ? 29 : 18)) return false;
 *id=++output;output_valid=true;return true;
}
bool framebuffer_output_revoke(u64 id){if(id!=output)return false;output_valid=false;return true;}
DisplayLeaseState display_direct_state(void){return lease;}
bool display_retire_firmware_scanout(void){CHECK(reserved[1]&&reserved[2]&&lease==DISPLAY_LEASE_KERNEL);retired=true;return fault!=12;}
bool display_direct_prepare_native(u64 id,u64 va,ProgramBorrowedMappingSpec *m,FramebufferInfo *i,u64 *fb){
 CHECK(output_valid && id==output && lease==DISPLAY_LEASE_KERNEL);
 *i=info;m->physical_base=info.physical_base;m->size=0x300000;m->virtual_base=va;m->flags=VM_WRITE|VM_UNCACHED;*fb=va;
 lease=DISPLAY_LEASE_HANDOFF;return true;
}
bool display_direct_commit(Process *p){CHECK(lease==DISPLAY_LEASE_HANDOFF);writer=p->id;lease=DISPLAY_LEASE_USER;return true;}
bool display_direct_reclaim(u64 pid){CHECK(pid==writer && !drawing[1]&&!drawing[2]);if(fault==23)return false;lease=DISPLAY_LEASE_KERNEL;writer=0;return true;}
bool display_direct_abort(void){CHECK(!drawing[1]&&!drawing[2]);lease=DISPLAY_LEASE_KERNEL;return true;}
u32 pci_device_count(void){return 1;}
const PciDevice *pci_device(u32 n){return n ? 0 : &device;}
bool display_probe_matches_gop(const BootInfo *b,const PciDevice *d){(void)b;return d==&device;}
bool polaris_read_profile_inspect(const BootInfo *b,PolarisReadReport *r){(void)b;memset(r,0,sizeof(*r));return fault!=1;}
bool display_probe_read(const PciDevice *d,DisplayProbeInfo *i){(void)d;memset(i,0,sizeof(*i));return true;}
bool display_probe_firmware_bar(const BootInfo *b,const PciDevice *d,const DisplayProbeInfo *i,u32 index,BootDisplayBar *bar){
 (void)b;(void)d;(void)i;if(index)return false;memset(bar,0,sizeof(*bar));bar->base=0x50000000;bar->size=0x10000000;bar->flags=15;return true;
}
bool pci_assignment_create(PciAssignment *a,const PciDevice *d){(void)d;++creates;if(fault==5)return false;memset(a,0,sizeof(*a));a->initialized=true;a->resource.id=101;return true;}
bool pci_assignment_claim_supervised(PciAssignment *a,CapabilityHandle *cap){if(fault==6)return false;a->owner_process_id=1;a->state=PCI_ASSIGNMENT_ASSIGNED;a->bus_master_exposed=true;*cap=3;return true;}
bool pci_assignment_valid(const PciAssignment *a){return a->initialized;}
u64 pci_assignment_owner_process_id(const PciAssignment *a){return a->owner_process_id;}
bool pci_assignment_destroy(PciAssignment *a){CHECK(!a->owner_process_id);a->initialized=false;return true;}
CapabilityTable *process_capabilities(Process *p){(void)p;return &supervisor.capabilities;}
bool capability_revoke(CapabilityTable *caps,CapabilityHandle cap){(void)caps;CHECK(cap==3&&!held&&!binding&&!reserved[1]&&!reserved[2]);return fault!=40;}
bool framebuffer_output_valid(void){return output_valid;}

bool pci_assignment_force_quarantine(PciAssignment *a,u64 pid){CHECK(pid==1);if(fault==21)return false;a->state=PCI_ASSIGNMENT_QUARANTINED;quarantined=true;return true;}
bool polaris_mmio_prepare_supervised(PolarisMmioBinding *b,const BootInfo *boot,PciAssignment *a,CapabilityHandle cap){
 (void)b;(void)boot;(void)a;CHECK(cap==3);binding=true;return fault!=7;
}
bool polaris_mmio_prepare_location(PolarisMmioBinding *b,const BootInfo *boot,Process *o,u64 id){(void)b;(void)boot;CHECK(o==&supervisor&&id==19);return fault!=8;}
u64 polaris_mmio_binding_identity(const PolarisMmioBinding *b){(void)b;return 19;}
bool polaris_mmio_hold_acquire(PolarisMmioHold *h,PolarisMmioBinding *b,u64 id,Process *o,u64 *out){(void)h;(void)b;CHECK(id==19&&o==&supervisor);if(fault==9)return false;held=true;*out=23;return true;}
bool polaris_mmio_binding_valid(const PolarisMmioBinding *b){(void)b;return binding;}
bool polaris_mmio_close(PolarisMmioBinding *b){(void)b;return fault!=33;}
bool polaris_mmio_hold_release(PolarisMmioHold *h,u64 id){(void)h;CHECK(held&&id==23);if(fault==34)return false;held=false;return true;}
bool polaris_mmio_cleanup(PolarisMmioBinding *b){(void)b;if(held||fault==35)return false;binding=false;return true;}
static bool scalar(u32 pipe,u32 selector,u32 *v){
 ++loads;if((fault==10&&!enabled)||(fault==25&&armed))return false;*v=0;
 if(selector==0)*v=pipe==0 ? 0x80410311 : 0x80400310;
 if(selector==3)*v=1;if(selector==4)*v=0xA10800A;if(selector==7)*v=0xF4;
 if(selector==8||selector==13)*v=1024;if(selector==14)*v=768;
 if(selector==16)*v=0x20;if(selector==17){*v=frame_counter;frame_counter+=3;}
 if(selector==18)*v=0xF5FFF400;if(selector==19)*v=0xF4000000;
 if(fault==16&&enabled&&!armed&&writes[0]==2&&selector==6)return false;
 if(fault==17&&enabled&&!armed&&writes[0]==2&&selector==0)*v=0;
 return true;
}
bool polaris_mmio_read_held(PolarisMmioHold *h,u64 id,Process *o,u32 p,PolarisDisplayRegister f,u32 *v){(void)h;CHECK(held&&id==23&&o==&supervisor);return scalar(p,f,v);}
bool polaris_mmio_read_witness_held(PolarisMmioHold *h,u64 id,Process *o,u32 p,PolarisPlaneWitnessRegister f,u32 *v){(void)h;CHECK(held&&id==23&&o==&supervisor);return scalar(p,15+f,v);}
bool polaris_mmio_read_location_held(PolarisMmioHold *h,u64 id,Process *o,PolarisLocationRegister f,u32 *v){(void)h;CHECK(held&&id==23&&o==&supervisor);return scalar(0,18+f,v);}
bool polaris_mmio_enable_rebind(PolarisMmioHold *h,u64 id){(void)h;CHECK(retired&&held&&id==23&&reserved[2]);if(fault==13)return false;enabled=true;return true;}
bool polaris_mmio_arm_recovery(PolarisMmioHold *h,u64 id){(void)h;CHECK(id==23&&held&&quarantined&&!reserved[1]&&!armed&&lease==DISPLAY_LEASE_KERNEL);if(fault==24)return false;armed=true;return true;}
bool polaris_mmio_rebind_read(PolarisMmioHold *h,u64 id,u32 f,u32 *v){(void)h;CHECK(enabled&&held&&id==23&&retired);return scalar(0,f,v);}
bool polaris_mmio_rebind_write(PolarisMmioHold *h,u64 id,PolarisDisplayRegister f,u32 v){
 (void)h;CHECK(retired&&enabled&&held&&id==23&&lease==DISPLAY_LEASE_KERNEL&&writes[armed]<2);
 CHECK(f==(writes[armed] ? POLARIS_REG_PRIMARY_LOW : POLARIS_REG_PRIMARY_HIGH));
 CHECK(v==(writes[armed] ? 0U : 0xF4U));++writes[armed];
 return (unsigned)fault!=(armed ? 26+writes[1] : 13+writes[0]);
}
bool polaris_driver_reserve(PolarisDriver *d,u32 role,const ProgramBorrowedMappingSpec *m){
 CHECK(!retired&&m->flags==VM_UNCACHED);if(fault==(role==1 ? 3 : 4))return false;
 memset(d,0,sizeof(*d));d->role=role;d->process_id=100+role;d->service.program.process.id=d->process_id;d->connection.incarnation=200+role;
 d->program_attempted=false;d->service.state=MANAGED_SERVICE_RUNNING;drivers[role]=d;reserved[role]=true;return true;
}
bool polaris_driver_plan(PolarisDriver *d,u64 id,const PolarisPlaneWitness *w,PolarisProgramPlan *p){
 if(fault==(d->role==1 ? 11 : 26))return false;
 memset(p,0,sizeof(*p));p->incarnation=d->connection.incarnation;p->observation_id=id;p->gpu_address=0xF400000000ULL;p->crtc=0x80410311;p->witness=*w;
 PolarisDisplayObservation o={0};o.surface_gpu_address=p->gpu_address;o.linear_span_bytes=0x300000;o.surface_width=o.pitch_pixels=1024;o.surface_height=768;o.depth=2;o.array_mode=1;
 for(u32 i=0;i<5;++i){u32 v;if(!polaris_plane_recipe_step(&o,w,0,i,&p->tags[i],&v))return false;p->values[i]=v;}
 return true;
}
bool polaris_driver_program(PolarisDriver *d,PolarisProgramPlan *p,const PolarisProgramIo *io,PolarisProgramResult *r){
 CHECK(!d->program_attempted);d->program_attempted=true;
 return polaris_program_execute(p,io,r);
}
bool program_borrowed_set_writable(ProgramInstance *p,u64 id,bool rw){
 CHECK(p->process.id==id&&rw&&retired&&lease==DISPLAY_LEASE_HANDOFF);
 u32 role=(u32)(id-100);if(fault==(role==1 ? 19 : 30))return false;drawing[role]=true;return true;
}
bool polaris_driver_draw(PolarisDriver *d){CHECK(drawing[d->role]&&lease==DISPLAY_LEASE_USER);return fault!=(d->role==1 ? 20 : 31);}
bool polaris_driver_stop(PolarisDriver *d){
 if(d->role && fault==(d->role==1 ? 22 : 32))return false;
 if(d->role){reserved[d->role]=false;drawing[d->role]=false;d->service.state=MANAGED_SERVICE_STOPPED;d->service.program.process.id=0;busy[d->role]=false;}return true;
}
bool managed_service_idle(ManagedService *s){return s->state==MANAGED_SERVICE_RUNNING;}
ManagedServiceState managed_service_state(ManagedService *s){return s->state;}
/* Match the real API: failed/stopped services no longer publish a live PID. */
u64 managed_service_process_id(ManagedService *s){return s->state==MANAGED_SERVICE_RUNNING ? s->program.process.id : 0;}
bool managed_service_call(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q,IpcMessage *r){
 (void)c;(void)r;CHECK((u32)q->words[0]==POLARIS_NATIVE_FAULT);s->state=MANAGED_SERVICE_FAILED;return false;
}
bool polaris_driver_ping(PolarisDriver *d,u64 nonce) {
 CHECK(nonce&&d->role&&reserved[d->role]);++pings[d->role];
 if(fault==69 || (fault==42&&d->role==1)||(fault==43&&d->role==2)||(fault==44&&d->role==2))return false;
 return !busy[d->role]&&d->service.state==MANAGED_SERVICE_RUNNING;
}
bool polaris_driver_diagnostic(PolarisDriver *d,bool spin) {
 CHECK(reserved[d->role]&&!busy[d->role]&&d->service.state==MANAGED_SERVICE_RUNNING);
 ++diagnostics[d->role];if(fault==49)return false;
 if(spin)busy[d->role]=true;else d->service.state=MANAGED_SERVICE_FAILED;
 return true;
}
bool polaris_driver_console_progress(PolarisDriver *d,bool require_busy,PolarisDriverProgress *out) {
 CHECK(!require_busy||busy[d->role]);++progress_calls;
 memset(out,0,sizeof(*out));out->console_process_id=301;out->console_incarnation=401;
 out->writes_before=10;out->writes_after=11;return fault!=50;
}
int main(int argc,char **argv){
 fault=argc>1 ? atoi(argv[1]) : 0;supervisor.id=1;supervisor.kernel=true;supervisor.initialized=true;
 memset(&info,0,sizeof(info));info.physical_base=0x50000000;info.size=0x300000;info.direct_map_safe=true;info.width=info.pixels_per_scanline=1024;info.height=768;info.pixel_format=1;
 BootInfo boot;memset(&boot,0,sizeof(boot));boot.framebuffer_base=info.physical_base;boot.framebuffer_size=info.size;boot.framebuffer_width=boot.framebuffer_pixels_per_scanline=1024;boot.framebuffer_height=768;boot.framebuffer_pixel_format=1;
 polaris_native_begin(&boot);
 if(!fault)CHECK(writes[0]==2&&drawing[1]&&!drawing[2]&&retired);
 if(fault>=41){
   unsigned io=loads+writes[0]+writes[1];
   if(fault>=75&&fault<=79) {
     if(fault==77||fault==78){polaris_native_rescue();CHECK(drawing[2]);io=loads+writes[0]+writes[1];}
     polaris_native_policy_diagnostic(fault==76||fault==78);
     if(fault!=79)CHECK(progress_calls==1);
     else CHECK(!progress_calls);
     ticks+=1000;polaris_native_poll();
     CHECK(!drawing[1]&&!drawing[2]&&io==loads+writes[0]+writes[1]&&policy_injections==1);
     polaris_native_policy_diagnostic(false);CHECK(policy_injections==1);
   }

   if(fault==57||fault==58||fault==71)CHECK(!retired&&writes[0]==0&&!drawing[1]&&!reserved[1]&&!reserved[2]);
   if(fault==59)CHECK(retired&&writes[0]==2&&!drawing[1]&&!reserved[1]);
   if(fault==60||fault==61||fault==62||fault==63||fault==67||fault==68||fault==69||fault==70||fault==72||fault==73||fault==74){
     if(fault==61)++policy_pid;
     if(fault==67)++policy_tid;
     if(fault==74)policy_busy=true;
     if(fault==62)drivers[1]->service.state=MANAGED_SERVICE_FAILED;
     if(fault==63||fault==70)drivers[2]->service.state=MANAGED_SERVICE_FAILED;
     ticks+=1000;polaris_native_poll();
     CHECK(!drawing[1]&&!drawing[2]&&writes[1]==0&&io==loads+writes[0]+writes[1]);
   }
   if(fault==64||fault==66){
     polaris_native_rescue();
     if(fault==66)CHECK(!drawing[2]&&writes[1]==2&&!reserved[2]);
     else {CHECK(drawing[2]);io=loads+writes[0]+writes[1];ticks+=1000;polaris_native_poll();CHECK(!drawing[2]&&io==loads+writes[0]+writes[1]);}
   }
   if(fault==41||fault==56){polaris_native_poll();CHECK(!pings[1]&&!pings[2]);}
   if(fault==41||fault==42||fault==43){ticks+=1000;polaris_native_poll();}
   if(fault==41)CHECK(pings[1]==1&&pings[2]==1&&io==loads+writes[0]+writes[1]&&drawing[1]);
   if(fault==42)CHECK(drawing[2]&&writes[1]==2&&!reserved[1]);
   if(fault==43)CHECK(!drawing[1]&&!drawing[2]&&writes[1]==0);
   if(fault==44||fault==47||fault==48){polaris_native_rescue();CHECK(drawing[2]);io=loads+writes[0]+writes[1];}
   if(fault==44){ticks+=1000;polaris_native_poll();CHECK(!drawing[2]&&io==loads+writes[0]+writes[1]);}
   if(fault==45||fault==46||fault==49||fault==50){
     polaris_native_diagnostic(1,fault!=46);
     if(fault==45||fault==46){CHECK(io==loads+writes[0]+writes[1]&&diagnostics[1]==1&&progress_calls==1);ticks+=1000;polaris_native_poll();CHECK(drawing[2]&&writes[1]==2);}
     else CHECK(!drawing[1]&&writes[1]==0&&io==loads+writes[0]+writes[1]);
   }
   if(fault==47||fault==48){
     polaris_native_diagnostic(2,fault==47);CHECK(diagnostics[2]==1&&progress_calls==1);
     ticks+=1000;polaris_native_poll();CHECK(!drawing[2]&&io==loads+writes[0]+writes[1]);
   }
   if(fault==51||fault==52){polaris_native_diagnostic(0,true);polaris_native_diagnostic(3,false);CHECK(!diagnostics[1]&&!diagnostics[2]&&drawing[1]);}
   if(fault==53||fault==54){clock_changed=fault==53;irq_lost=fault==54;polaris_native_poll();CHECK(!drawing[1]&&io==loads+writes[0]+writes[1]);clock_changed=irq_lost=false;}
   if(fault==55){drivers[1]->service.state=drivers[2]->service.state=MANAGED_SERVICE_FAILED;polaris_native_poll();CHECK(!drawing[1]&&writes[1]==0&&io==loads+writes[0]+writes[1]);}
   if(fault==56){polaris_native_diagnostic(2,false);CHECK(!diagnostics[2]&&drawing[1]);}
   if(fault==51){polaris_native_diagnostic(1,true);polaris_native_diagnostic(1,true);CHECK(diagnostics[1]==1);ticks+=1000;polaris_native_poll();CHECK(drawing[2]);}
   if(fault==52){polaris_native_rescue();io=loads+writes[0]+writes[1];polaris_native_diagnostic(1,true);CHECK(!diagnostics[1]&&io==loads+writes[0]+writes[1]);}
 }
 else if(fault==38){unsigned before=loads+writes[0]+writes[1];CHECK(polaris_native_power_prepare());CHECK(before==loads+writes[0]+writes[1]&&!reserved[1]&&!reserved[2]&&quarantined);}
 else if(fault==36){drivers[1]->service.state=MANAGED_SERVICE_FAILED;polaris_native_poll();CHECK(drawing[2]&&!reserved[1]);}
 else if(fault==37){drivers[2]->service.state=MANAGED_SERVICE_FAILED;polaris_native_poll();CHECK(!drawing[1]&&!drawing[2]);}
 else if(drawing[1]||(!fault))polaris_native_rescue();
 if(!fault)CHECK(writes[1]==2&&drawing[2]&&!reserved[1]&&quarantined);
 if(fault==41||fault==56){polaris_native_rescue();CHECK(drawing[2]);}
 if(fault==39){unsigned before=loads+writes[0]+writes[1];CHECK(polaris_native_power_prepare());CHECK(before==loads+writes[0]+writes[1]&&!reserved[2]&&quarantined);}
 polaris_native_finish();
 unsigned io_before=loads+writes[0]+writes[1];
 bool power=polaris_native_power_prepare();
 CHECK(loads+writes[0]+writes[1]==io_before); /* No GPU IO on power path. */
 if(!fault)CHECK(power);
 if(fault==40)CHECK(!power);
 unsigned before=writes[0]+writes[1],claims=creates;
 unsigned tests=diagnostics[1]+diagnostics[2];
 polaris_native_rescue();polaris_native_begin(&boot);polaris_native_diagnostic(1,true);polaris_native_diagnostic(2,true);polaris_native_status();polaris_native_poll();
 CHECK(tests==diagnostics[1]+diagnostics[2]);
 CHECK(writes[0]<=2&&writes[1]<=2&&before==writes[0]+writes[1]&&claims==creates);
 if(!fault)CHECK(!held&&!binding&&!reserved[1]&&!reserved[2]&&retired&&render&&quarantined&&lease==DISPLAY_LEASE_KERNEL);
 if(fault==34)CHECK(held&&binding);
 puts("PASS: production native orchestration invariants; no hardware.");
 return 0;
}
