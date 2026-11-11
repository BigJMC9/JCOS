#include "polaris_vertical_test.h"
#include "polaris_vertical_fixture.h"
#include "polaris_link.h"
#include "managed_service.h"
#include "register_probe_protocol.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "address_space.h"
#include "endpoint.h"
#include "thread.h"
#include "process_exit_queue.h"
#include "terminal.h"
static ManagedService bad;
static bool stop(void){
 if(managed_service_state(&bad)==MANAGED_SERVICE_STOPPED)return !program_instance_needs_cleanup(&bad.program);
 ManagedServiceStopResult r=managed_service_stop_bounded(&bad);
 return (r==MANAGED_SERVICE_STOP_GRACEFUL || r==MANAGED_SERVICE_STOP_FORCED) && !program_instance_needs_cleanup(&bad.program);
}
bool polaris_vertical_test(void){
 if(!stop())return false;
 u64 pages=pmm_stats().free_pages;u32 processes=process_object_count(),spaces=address_space_object_count(),endpoints=endpoint_object_count(),threads=thread_object_count(),queues=process_exit_queue_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
 u32 values[86];u64 words[18];polaris_vertical_fixture(values);
 const u64 expected[18]={0x10000ULL,0x0002020000020000ULL,0x00C0084007800898ULL,0x0029046104380465ULL,0x00050000002C0000ULL,0,0x0000010002000000ULL,0x0000758D00008000ULL,0x0000005000010201ULL,0x0000758F0000758DULL,0x0000046400000464ULL,0x0000046C00000000ULL,0x0000046C00000000ULL,0x0000046C00000001ULL,0x0000001100000002ULL,0x0000000000000002ULL,0x0000046400000000ULL,0x0000046400000001ULL};
 if(!polaris_vertical_decode_copy(values,words))return false;
 for(u32 i=0;i<18;++i)if(words[i]!=expected[i])return false;
 terminal_writeln("  ACTUAL RING3 VERTICAL PLAN / 18 GOLDEN WORDS / SYNTHETIC CONTROL BASELINE: PASS");
 for(u32 n=0;n<4;++n){
  polaris_vertical_fixture(values);
  if(n==0)values[83]^=1;
  else if(n==1)values[85]=~0U;
  else if(n==2)values[82]=values[85]=0x8000;
  else values[33]^=1;
  u32 status=n==0?POLARIS_VERTICAL_CHANGED:n==1?POLARIS_VERTICAL_ALL_ONES:n==2?POLARIS_VERTICAL_CONTROLS:POLARIS_VERTICAL_TIMING;
  if(!polaris_vertical_decode_copy(values,words) || words[0]!=status)return false;
  for(u32 i=1;i<18;++i)if(words[i])return false;
 }
 terminal_writeln("  ACTUAL VERTICAL REFUSALS / CLEARED PLAN / NO GPU IO: PASS");
 ManagedServiceSpec spec;spec.path="/bin/registerprobe.elf";spec.shutdown_message=REGISTER_PROBE_SHUTDOWN;spec.shutdown_reply=REGISTER_PROBE_STOPPED;
 for(u32 n=0;n<9;++n){
  ManagedServiceConnection c;IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=REGISTER_PROBE_VERTICAL_BEGIN;q.words[1]=POLARIS_VERTICAL_VERSION;
  if(!managed_service_start(&bad,&spec) || !managed_service_connect_kernel(&bad,&c))goto fail;
  if(n==4){q.words[1]=2;if(managed_service_call(&bad,&c,&q,&r) || !stop())goto fail;continue;}
  if(!managed_service_call(&bad,&c,&q,&r) || r.word_count!=4 || r.words[2]!=1 || r.words[3]!=86)goto fail;
  if(n>=6){
   polaris_vertical_fixture(values);
   for(u32 i=0;i<86;++i){q.words[0]=REGISTER_PROBE_VERTICAL_DATA;q.words[1]=i;q.words[2]=values[i];if(!managed_service_call(&bad,&c,&q,&r) || r.words[2]!=i+1)goto fail;}
   q.words[0]=REGISTER_PROBE_VERTICAL_QUERY;q.words[1]=0;q.words[2]=1;
   if(n==7){if(!managed_service_call(&bad,&c,&q,&r) || r.words[3]!=expected[0])goto fail;}
   else if(n==6)q.words[1]=18;
   else {q.words[0]=REGISTER_PROBE_VERTICAL_DATA;q.words[1]=86;q.words[2]=0;}
  }else if(n==2){q.words[0]=REGISTER_PROBE_VERTICAL_DATA;q.words[1]=0;q.words[2]=0;if(!managed_service_call(&bad,&c,&q,&r))goto fail;}
  else if(n==3){q.words[0]=REGISTER_PROBE_VERTICAL_DATA;q.words[1]=0;q.words[2]=0x100000000ULL;}
  else if(n==1){q.words[0]=REGISTER_PROBE_TIMING_BEGIN;q.words[1]=2;}
  else if(n==0){q.words[0]=REGISTER_PROBE_VERTICAL_QUERY;q.words[1]=0;q.words[2]=1;}
  /* n5 replays BEGIN; n2 replays DATA; n7 replays QUERY. */
  if(managed_service_call(&bad,&c,&q,&r) || !stop())goto fail;
 }
 terminal_writeln("  ACTUAL VERTICAL ORDER-ROLE-VERSION-WIDE-DUPLICATE REFUSALS / PROCESS CLOSED: PASS");
 if(pages!=pmm_stats().free_pages || processes!=process_object_count() || spaces!=address_space_object_count() || endpoints!=endpoint_object_count() || threads!=thread_object_count() || queues!=process_exit_queue_object_count() || caps!=capability_table_count(process_capabilities(process_kernel())))return false;
 terminal_writeln("  ACTUAL VERTICAL PLAN / FULL CPU RESOURCE BASELINE / NO GPU IO: PASS");return true;
fail:(void)stop();return false;
}
