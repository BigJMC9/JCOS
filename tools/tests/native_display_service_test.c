/* Production framebuffer/display/terminal/service integration; launch/IPC doubles.
 * Fixture RAM is simulated MMIO. No device or actual child is executed here. */
#define main retained_scanout_fixture_main
#define process_storage_in_use fixture_process_storage_in_use
#include "framebuffer_scanout_test.c"
#undef process_storage_in_use
#undef main
#include "../../kernel/display_service.h"
#include "../../kernel/compositor_service.h"
#include "../../kernel/managed_service.h"
#include "../../include/display_service_protocol.h"
#include "../../kernel/timer.h"
#include "../../kernel/scheduler.h"
#include "../../kernel/arch.h"
#include "../../kernel/ipc.h"
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"native presenter case%d line%d %s\n",fault,__LINE__,#x);exit(1);}}while(0)
#include <stdlib.h>
static ManagedService *running;
static unsigned launches,stops,calls;
static int fault;
static bool release_failure;
static u64 ticks;
bool process_storage_in_use(const Process *p) {
 return fixture_process_storage_in_use(p) || (running && p==&running->program.process && p->initialized);
}
bool timer_initialized(void){return true;}
u32 timer_frequency(void){return 100;}
u64 timer_ticks(void){return ++ticks;}
void arch_pause(void){}
bool scheduler_yield(void){return true;}
ManagedServiceState compositor_service_state(void){return MANAGED_SERVICE_STOPPED;}
bool compositor_service_running(void){return false;}
bool compositor_service_surface_remove(u32 n){(void)n;REQUIRE(0);return false;}
bool compositor_service_compose(u32 *n,u32 *p){(void)n;(void)p;REQUIRE(0);return false;}
bool display_surface_valid(const DisplaySurface *s){(void)s;return false;}
bool display_surface_unmap_reader(DisplaySurface *s,Process *p,u64 v){(void)s;(void)p;(void)v;REQUIRE(0);return false;}
static void drop_service(ManagedService *s){memset(&s->program,0,sizeof(s->program));s->state=MANAGED_SERVICE_STOPPED;}
bool managed_service_start_ex(ManagedService *s,const ManagedServiceSpec *spec,const ManagedServiceLaunchExtras *e){
 ++launches;running=s;
 REQUIRE(!strcmp(spec->path,"/bin/displayservice.elf")&&display_direct_state()==DISPLAY_LEASE_HANDOFF);
 FramebufferInfo info;REQUIRE(framebuffer_output_info(&info));
 REQUIRE(e->startup_grant_count==0&&e->startup_argument_count==3);
 REQUIRE(e->borrowed_mapping.physical_base==info.physical_base&&e->borrowed_mapping.size==info.size);
 REQUIRE(e->borrowed_mapping.virtual_base==USER_BASE&&e->borrowed_mapping.flags==(VM_WRITE|VM_UNCACHED));
 REQUIRE(e->startup_arguments[0]==USER_BASE&&JCOS_DISPLAY_WIDTH(e->startup_arguments[1])==WIDTH&&
  JCOS_DISPLAY_HEIGHT(e->startup_arguments[1])==HEIGHT&&JCOS_DISPLAY_STRIDE(e->startup_arguments[2])==PITCH);
 if(fault==1){drop_service(s);return false;}
 s->program.process.id=77+launches;s->program.process.initialized=fault!=3;s->program.process_created=true;
 s->incarnation=100+launches;s->state=fault==2?MANAGED_SERVICE_FAILED:MANAGED_SERVICE_RUNNING;
 if(fault==2)return false;
 if(fault==6){FramebufferOutputStatus out;framebuffer_output_status(&out);REQUIRE(framebuffer_output_revoke(out.identity));}
 return true;
}
ManagedServiceState managed_service_state(ManagedService *s){return s->state;}
bool managed_service_idle(ManagedService *s){return s->state==MANAGED_SERVICE_RUNNING;}
bool managed_service_running(ManagedService *s){return s->state==MANAGED_SERVICE_RUNNING;}
bool managed_service_connect_kernel(ManagedService *s,ManagedServiceConnection *c){c->incarnation=s->incarnation;return true;}
bool managed_service_call(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q,IpcMessage *r){
 ++calls;memset(r,0,sizeof(*r));r->words[1]=c->incarnation;r->words[2]=JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION;
 if(JCOS_DISPLAY_SERVICE_HEADER_OP(q->words[0])==JCOS_DISPLAY_SERVICE_OP_SET_PIXEL_MASKS){
  REQUIRE(q->word_count==3);r->word_count=3;r->words[0]=JCOS_DISPLAY_SERVICE_REPLY_MASKS_SET;return fault!=7;
 }
 REQUIRE(JCOS_DISPLAY_SERVICE_HEADER_OP(q->words[0])==JCOS_DISPLAY_SERVICE_OP_PING&&s==running);
 r->word_count=4;r->words[0]=JCOS_DISPLAY_SERVICE_REPLY_PONG;r->words[3]=q->words[2];return true;
}
ManagedServiceStopResult managed_service_stop_bounded(ManagedService *s){
 ++stops;
 if(release_failure){s->state=MANAGED_SERVICE_REAP_PENDING;return MANAGED_SERVICE_STOP_FAILED;}
 drop_service(s);return MANAGED_SERVICE_STOP_GRACEFUL;
}
bool managed_service_recover(ManagedService *s){
 if(release_failure)return false;drop_service(s);return true;
}
/* Unused subsystem paths must never execute in this launch/lease harness. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
bool capability_insert(
    CapabilityTable *table,
    void *object,
    CapabilityType type,
    CapabilityRights rights,
    CapabilityHandle *out_handle
) { abort(); }
bool capability_revoke(
    CapabilityTable *table,
    CapabilityHandle handle
) { abort(); }
bool compositor_service_focus_clear(void) { abort(); }
u32 compositor_service_focus_slot(void) { abort(); }
bool compositor_service_focus_surface(u32 slot) { abort(); }
bool compositor_service_surface_configure(u32 slot, u32 x, u32 y,
    u32 width, u32 height, u32 z) { abort(); }
bool display_surface_map_reader(DisplaySurface *surface, Process *process, u64 virtual_base) { abort(); }
bool display_surface_reader_mapping_valid(const DisplaySurface *surface, Process *process, u64 virtual_base) { abort(); }
u64 display_surface_writer_process_id(const DisplaySurface *surface) { abort(); }
bool endpoint_closed(const Endpoint *endpoint) { abort(); }
bool endpoint_create(Endpoint *endpoint) { abort(); }
bool endpoint_destroy(Endpoint *endpoint) { abort(); }
bool endpoint_storage_in_use(const Endpoint *endpoint) { abort(); }
bool ipc_endpoint_close(Endpoint *endpoint) { abort(); }
bool ipc_try_send(
    Process *process,
    CapabilityHandle handle,
    const IpcMessage *message
) { abort(); }
u64 managed_service_incarnation(const ManagedService *service) { abort(); }
bool managed_service_last_exit_info(const ManagedService *service, ProcessExitInfo *out) { abort(); }
u64 managed_service_process_id(ManagedService *service) { abort(); }
bool managed_service_send_oneway(ManagedService *service, const ManagedServiceConnection *connection,
    const IpcMessage *request) { abort(); }
u64 managed_service_thread_id(ManagedService *service) { abort(); }
Process *process_kernel(void) { abort(); }
#pragma clang diagnostic pop
int main(int argc,char **argv){
 REQUIRE(argc==2);fault=atoi(argv[1]);release_failure=fault==4;
 BootMemoryDescriptor memory={0};memory.type=11;memory.physical_start=(u64)pixels;memory.number_of_pages=sizeof(pixels)/4096;
 BootInfo boot={0};boot.framebuffer_base=(u64)pixels;boot.framebuffer_size=sizeof(pixels);boot.framebuffer_width=WIDTH;
 boot.framebuffer_height=HEIGHT;boot.framebuffer_pixels_per_scanline=PITCH;boot.framebuffer_pixel_format=fault>=7?2:1;
 boot.framebuffer_red_mask=0xFF0000;boot.framebuffer_green_mask=0xFF00;boot.framebuffer_blue_mask=0xFF;
 boot.memory_map=(u64)&memory;boot.memory_map_size=sizeof(memory);boot.memory_map_descriptor_size=sizeof(memory);
 REQUIRE(framebuffer_init(&boot)&&display_init()&&terminal_init());
 FramebufferInfo info,saved_boot;REQUIRE(framebuffer_info(&info));saved_boot=info;
 REQUIRE(!display_service_start_native(0)&&!display_service_start_native(1)&&!display_service_start_native(~0ULL)&&!launches);
 if(fault>=7){
  bool masks_started=display_service_start();
  REQUIRE(masks_started==(fault==8)&&!display_service_output_identity());
  REQUIRE(calls==1&&display_service_cleanup()&&display_direct_state()==DISPLAY_LEASE_KERNEL);
  REQUIRE(display_retire_firmware_scanout());
  u64 refused=0;REQUIRE(!framebuffer_native_install(&info,false,&refused)&&!refused);
  puts("Firmware bitmask configuration and native format refusal: PASS (no hardware)");return 0;
 }
 REQUIRE(display_retire_firmware_scanout()&&!framebuffer_firmware_valid());
 u64 id=0;REQUIRE(framebuffer_native_install(&info,false,&id)&&id);
 REQUIRE(!display_service_start()&&!display_service_start_native(id+1)&&!launches&&display_direct_state()==DISPLAY_LEASE_KERNEL);
 if(fault==0){
  ProgramBorrowedMappingSpec prior_mapping;FramebufferInfo prior_info;u64 prior_va=0;
  user.id=19;user.initialized=true;
  REQUIRE(display_direct_prepare_native(id,USER_BASE,&prior_mapping,&prior_info,&prior_va));
  REQUIRE(display_direct_commit(&user)&&display_direct_owner_process_id()==user.id);
  REQUIRE(!display_service_start_native(id)&&!launches&&display_direct_owner_process_id()==user.id);
  REQUIRE(display_direct_reclaim(user.id));
  REQUIRE(framebuffer_write_begin());
  REQUIRE(!display_service_start_native(id)&&!launches&&display_direct_state()==DISPLAY_LEASE_KERNEL);
  framebuffer_write_end();
 }
 bool started=display_service_start_native(id);
 if(fault==1||fault==3||fault==6||fault==7){
  REQUIRE(!started&&display_service_state()==MANAGED_SERVICE_STOPPED&&display_direct_state()==DISPLAY_LEASE_KERNEL&&!display_service_output_identity());
 }else if(fault==2){
  REQUIRE(!started&&display_service_output_identity()==id&&display_direct_state()==DISPLAY_LEASE_HANDOFF);
  REQUIRE(display_service_cleanup()&&display_direct_state()==DISPLAY_LEASE_KERNEL&&!display_service_output_identity());
 }else{
  REQUIRE(started&&display_service_output_identity()==id&&display_direct_state()==DISPLAY_LEASE_USER);
  REQUIRE(display_direct_owner_process_id()==running->program.process.id&&!terminal_render_enabled());
  REQUIRE(!display_service_start_native(id)&&launches==1);
  u64 pong=0;REQUIRE(display_service_ping(123,&pong)&&pong==123);
  if(fault==4){
   REQUIRE(!display_service_cleanup()&&display_service_output_identity()==id&&display_direct_state()==DISPLAY_LEASE_USER);
   release_failure=false;REQUIRE(display_service_cleanup()&&!display_service_output_identity()&&display_direct_state()==DISPLAY_LEASE_KERNEL);
  }else if(fault==5){
   REQUIRE(display_service_cleanup()&&!display_service_output_identity()&&display_direct_state()==DISPLAY_LEASE_KERNEL);
  }else{
   REQUIRE(framebuffer_output_revoke(id));
   unsigned before=calls;REQUIRE(!display_service_ping(123,&pong)&&!pong&&calls==before);
   u32 count=1,sample=1;REQUIRE(!display_service_compose(&count,&sample)&&!count&&!sample);
   REQUIRE(display_service_cleanup()&&!display_service_output_identity()&&display_direct_state()==DISPLAY_LEASE_KERNEL);
   u64 rescue=0;REQUIRE(framebuffer_native_install(&info,true,&rescue)&&rescue>id);
   before=launches;REQUIRE(!display_service_start_native(id)&&launches==before);
   REQUIRE(display_service_start_native(rescue)&&display_service_output_identity()==rescue);
   REQUIRE(display_service_cleanup()&&!display_service_output_identity());
  }
 }
 REQUIRE(framebuffer_info(&info)&&!memcmp(&info,&saved_boot,sizeof(info))&&!framebuffer_firmware_valid());
 printf("Native presentation case%d: PASS (real output generation/lease/service startup,retained cleanup and firmware stays retired;no hardware)\n",fault);
 return 0;
}
