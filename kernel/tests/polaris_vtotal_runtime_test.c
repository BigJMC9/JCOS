#include "polaris_vtotal_runtime_test.h"
#include "polaris_driver.h"
#include "pmm.h"
#include "physmap.h"
#include "process.h"
#include "address_space.h"
#include "endpoint.h"
#include "thread.h"
#include "process_exit_queue.h"
#include "device_resource.h"
#include "font8x8.h"
#include "lib.h"
#include "terminal.h"
static PolarisDriver primary,rescue;
static u64 storage;
typedef struct {u32 reg[3],frame,writes,fail_after;bool restore;} Ram;
static bool admitted(void *c){(void)c;return true;}
static bool read_v(void *c,u32 f,u32 *out){
 Ram *r=c;if(f<3)*out=r->reg[f];else if(f==3)*out=0x80410311;else if(f==4)*out=1124;
 else if(f==5){r->frame=(r->frame+3)&0xFFFFFFU;*out=r->frame;}else return false;return true;
}
static bool write_v(void *c,u32 f,u32 value){
 Ram *r=c;u32 n=r->writes;
 if(n>=3 || f!=(r->restore?(n+2)%3:n) || value!=(r->restore?0:n<2?1132:0x11))return false;
 r->reg[f]=value;++r->writes;return !r->fail_after || r->writes!=r->fail_after;
}
static bool wait_v(void *c){(void)c;return true;}
typedef struct {u32 writes,frame;} Plane;
static bool read_plane(void *c,PolarisRebindRead f,u32 *out){
 Plane *p=c;switch(f){case 0:*out=0;break;case 1:*out=0xF4;break;case 2:*out=0x80410311;break;case 3:*out=0;break;case 4:*out=0x20;break;case 5:p->frame+=3;*out=p->frame;break;default:return false;}return true;
}
static bool write_plane(void *c,PolarisDisplayRegister f,u32 value){
 Plane *p=c;if(p->writes>=2 || f!=(p->writes?POLARIS_REG_PRIMARY_LOW:POLARIS_REG_PRIMARY_HIGH) || value!=(p->writes?0U:0xF4U))return false;++p->writes;return true;
}
static bool plane(PolarisDriver *d,u64 id){
 PolarisPlaneWitness witness;k_memset(&witness,0,sizeof(witness));witness.values[0][1]=witness.values[1][1]=0x20;witness.values[0][2]=100;witness.values[1][2]=110;
 PolarisProgramPlan plan;Plane p={0};PolarisProgramIo io={&p,admitted,write_plane,read_plane,wait_v};PolarisProgramResult result;
 return polaris_driver_plan(d,id,&witness,&plan) && polaris_driver_program(d,&plan,&io,&result) && result.complete&&p.writes==2;
}
static bool line_matches(const u32 *pixels,const char *text,u32 y,u32 bg){
 for(u32 n=0;text[n];++n){
  for(u32 row=0;row<8;++row){
   for(u32 col=0;col<8;++col){
    u32 color=(font8x8[(u8)text[n]][row]&(1U<<(7U-col)))?0xFFFFFFU:bg;
    if(pixels[(y+row)*1024U+32U+n*8U+col]!=color)return false;
   }
  }
 }
 return pixels[0]==bg&&pixels[1024U*768U-1]==bg;
}
static bool stop(void){
 bool a=polaris_driver_stop(&primary),b=polaris_driver_stop(&rescue);
 if(!a||!b)return false;
 if(storage){if(!frame_free_range(phys_to_frame(storage),768))return false;storage=0;}return true;
}
bool polaris_vtotal_runtime_test(void){
 if(!stop())return false;
 u32 scenario=~0U;
 u64 pages=pmm_stats().free_pages;u32 processes=process_object_count(),spaces=address_space_object_count(),endpoints=endpoint_object_count(),threads=thread_object_count(),queues=process_exit_queue_object_count(),objects=device_resource_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
 for(u32 n=0;n<10;++n){
  scenario=n;
  storage=pmm_alloc_pages(768);if(!storage)goto fail;k_memset(phys_to_virt(storage),0,0x300000);
  ProgramBorrowedMappingSpec map;map.physical_base=storage;map.size=0x300000;map.virtual_base=POLARIS_NATIVE_FB_VA;map.flags=0;
  if(!polaris_driver_reserve(&primary,1,&map)||!polaris_driver_reserve(&rescue,2,&map)||!plane(&primary,100+n))goto fail;
  u64 failed_pid=primary.process_id,failed_inc=primary.connection.incarnation;
  if(!failed_pid || !rescue.process_id || failed_pid==rescue.process_id || failed_inc==rescue.connection.incarnation)goto fail;
  Ram r={0};r.frame=0xFFFFF9;PolarisVtotalIo io={&r,admitted,read_v,write_v,wait_v};PolarisVtotalResult result;
  u32 prefix=n>=1&&n<=3?n:0;bool ok=polaris_driver_vtotal(&primary,&io,prefix,&result);
  if(ok!=(prefix==0)||r.writes!=(prefix?prefix:3)||result.write_attempts!=r.writes||result.complete!=(prefix==0))goto fail;
  if(!prefix){
   if(!program_borrowed_set_writable(&primary.service.program,primary.process_id,true)||!polaris_driver_draw(&primary)||
    !line_matches(phys_to_virt(storage),"VERTICAL OVERRIDE READBACK VERIFIED - TARGET 1133",88,0x103018U))goto fail;
  }else{
   for(u32 i=0;i<768;++i){frame_t f;vm_flags_t flags;if(!address_space_query_page(process_address_space(&primary.service.program.process),map.virtual_base+(u64)i*VM_PAGE_SIZE,&f,&flags)||f!=phys_to_frame(storage)+i||flags!=VM_USER)goto fail;}
  }
  u32 before=r.writes;PolarisVtotalResult again;if(polaris_driver_vtotal(&primary,&io,0,&again)||r.writes!=before||!primary.vtotal_attempted)goto fail;
  if(!polaris_driver_stop(&primary)||managed_service_process_id(&primary.service)||primary.process_id!=failed_pid||!managed_service_idle(&rescue.service))goto fail;
  r.restore=true;r.writes=0;r.fail_after=n>=4&&n<=6?n-3:0;
  if(n>=7){
   /* Diagnostic requests cannot be used on the primary or normal rescue entry. */
   if(polaris_driver_rescue_vtotal_fault(&rescue,&io,0,&result)||rescue.vtotal_attempted||
      polaris_driver_rescue_vtotal_fault(&rescue,&io,4,&result)||rescue.vtotal_attempted)goto fail;
   IpcMessage q,reply;k_memset(&q,0,sizeof(q));q.word_count=4;
   q.words[0]=POLARIS_NATIVE_HEADER(POLARIS_NATIVE_RESCUE_VTOTAL_FAULT);q.words[1]=2;
   for(u32 bad=0;bad<4;++bad){
    q.words[2]=bad==0?0:bad==1?4:1;q.words[3]=bad==2?1:0;q.words[1]=bad==3?1:2;
    if(!managed_service_call(&rescue.service,&rescue.connection,&q,&reply)||reply.words[2]||reply.words[3])goto fail;
   }
   if(!polaris_driver_ping(&rescue,900+n))goto fail;
  }
  ok=n>=7?polaris_driver_rescue_vtotal_fault(&rescue,&io,n-6,&result):polaris_driver_vtotal(&rescue,&io,0,&result);
  if(ok!=(n<4)||r.writes!=(n<4?3:n>=7?n-6:n-3)||result.write_attempts!=r.writes)goto fail;
  if(n>=7){
   ProcessExitInfo exit;
   if(!process_exit_info(&rescue.service.program.process,&exit)||exit.reason!=PROCESS_EXIT_FAULT||
      exit.vector!=6||exit.process_id!=rescue.process_id||!exit.thread_id||
      managed_service_process_id(&rescue.service)||result.reads!=6||result.polls||result.complete)goto fail;
   if(!line_matches(phys_to_virt(storage),"VERTICAL OVERRIDE READBACK VERIFIED - TARGET 1133",88,0x103018U))goto fail;
  }
  before=r.writes;if(polaris_driver_vtotal(&rescue,&io,0,&again)||r.writes!=before||
   polaris_driver_rescue_vtotal_fault(&rescue,&io,1,&again)||r.writes!=before)goto fail;
  if(n<4){
   if(r.reg[0]||r.reg[1]||r.reg[2]||!result.complete||!plane(&rescue,200+n)||
    !program_borrowed_set_writable(&rescue.service.program,rescue.process_id,true)||!polaris_driver_draw(&rescue)||
    !line_matches(phys_to_virt(storage),"ZERO VERTICAL OVERRIDE RESTORED - BASE TOTAL 1125",88,0x182030U))goto fail;
  }else{
   for(u32 i=0;i<768;++i){frame_t f;vm_flags_t flags;if(!address_space_query_page(process_address_space(&rescue.service.program.process),map.virtual_base+(u64)i*VM_PAGE_SIZE,&f,&flags)||flags!=VM_USER)goto fail;}
  }
  if(!stop())goto fail;
 }
 terminal_writeln("  ACTUAL RING3 VERTICAL PRIMARY / THREE STORES / READBACK AND FRAMES (RAM): PASS");
 terminal_writeln("  ACTUAL PRIMARY EXCEPTION AFTER EACH VERTICAL STORE / FRESH RESCUE / ZERO RESTORATION (RAM): PASS");
 terminal_writeln("  ACTUAL RESCUE PARTIAL STORE FAILURE / NO REPLAY / RO FRAMEBUFFER RETAINED: PASS");
 terminal_writeln("  ACTUAL RESCUE UD2 AFTER EACH RESTORATION STORE ACK / NO COMPLETION OR REPLAY (RAM): PASS");
 terminal_writeln("  RESCUE DIAGNOSTIC INVALID ROLE/PREFIX/RESERVED REFUSED / RO PIXELS PRESERVED: PASS");
 storage=pmm_alloc_pages(768);if(!storage)goto fail;
 ProgramBorrowedMappingSpec map;map.physical_base=storage;map.size=0x300000;map.virtual_base=POLARIS_NATIVE_FB_VA;map.flags=0;
 if(!polaris_driver_reserve(&primary,1,&map))goto fail;
 IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=(4ULL<<32)|POLARIS_NATIVE_PING;
 if(managed_service_call(&primary.service,&primary.connection,&q,&r)||!stop())goto fail;
 if(pages!=pmm_stats().free_pages||processes!=process_object_count()||spaces!=address_space_object_count()||endpoints!=endpoint_object_count()||threads!=thread_object_count()||queues!=process_exit_queue_object_count()||objects!=device_resource_object_count()||caps!=capability_table_count(process_capabilities(process_kernel())))goto fail;
 terminal_writeln("  ACTUAL VERTICAL DRIVER PROTOCOL5 / OLD VERSION REFUSED / FULL CPU BASELINE: PASS");return true;
fail:terminal_write("ACTUAL VTOTAL RAM FAILURE CASE=");terminal_write_u64(scenario);terminal_putchar('\n');(void)stop();return false;
}
