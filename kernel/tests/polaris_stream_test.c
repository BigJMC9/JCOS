#include "polaris_stream_test.h"
#include "polaris_stream_fixture.h"
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
bool polaris_stream_test(void){
 if(!stop())return false;
 u64 pages=pmm_stats().free_pages;u32 processes=process_object_count(),spaces=address_space_object_count(),endpoints=endpoint_object_count(),threads=thread_object_count(),queues=process_exit_queue_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
 u32 values[96];u64 words[14];polaris_stream_fixture(values);
 if(!polaris_stream_decode_copy(values,words))return false;
 for(u32 i=0;i<14;++i)if(words[i]!=polaris_stream_golden[i])return false;
 terminal_writeln("  ACTUAL RING3 DP STREAM / 14 GOLDEN WORDS / SYNTHETIC FIFO-UPDATE TAIL: PASS");
 for(u32 n=0;n<5;++n){
  polaris_stream_fixture(values);
  if(n==0)values[91]^=1;
  else if(n==1)values[95]=~0U;
  else if(n==2)values[80]=values[83]=1;
  else if(n==3)values[88]=values[93]=0;
  else {values[87]=values[92]=0x10;values[89]=values[94]=1;values[90]=values[95]=8;}
  if(!polaris_stream_decode_copy(values,words))return false;
  if(n==4){if(words[13]!=(POLARIS_STREAM_FIFO_OTHER|POLARIS_STREAM_UPDATE_HELD|POLARIS_STREAM_UPDATE_OTHER) || words[10]!=0x1000010201ULL || words[11]!=0x101000000ULL || words[12]!=8)return false;continue;}
  u32 status=n==0?POLARIS_STREAM_CHANGED:n==1?POLARIS_STREAM_ALL_ONES:n==2?POLARIS_STREAM_BASELINE:POLARIS_STREAM_ROUTE;
  if(words[0]!=status)return false;for(u32 i=1;i<14;++i)if(words[i])return false;
 }
 terminal_writeln("  ACTUAL DP STREAM REFUSALS / CLEARED TAIL / RAW FIFO-UPDATE FLAGS PRESERVED: PASS");
 ManagedServiceSpec spec;spec.path="/bin/registerprobe.elf";spec.shutdown_message=REGISTER_PROBE_SHUTDOWN;spec.shutdown_reply=REGISTER_PROBE_STOPPED;
 for(u32 n=0;n<9;++n){
  ManagedServiceConnection c;IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=REGISTER_PROBE_STREAM_BEGIN;q.words[1]=POLARIS_STREAM_VERSION;
  if(!managed_service_start(&bad,&spec) || !managed_service_connect_kernel(&bad,&c))goto fail;
  if(n==4){q.words[1]=2;if(managed_service_call(&bad,&c,&q,&r) || !stop())goto fail;continue;}
  if(!managed_service_call(&bad,&c,&q,&r) || r.word_count!=4 || r.words[2]!=1 || r.words[3]!=96)goto fail;
  if(n>=6){
   polaris_stream_fixture(values);
   for(u32 i=0;i<96;++i){q.words[0]=REGISTER_PROBE_STREAM_DATA;q.words[1]=i;q.words[2]=values[i];if(!managed_service_call(&bad,&c,&q,&r) || r.words[2]!=i+1)goto fail;}
   q.words[0]=REGISTER_PROBE_STREAM_QUERY;q.words[1]=0;q.words[2]=1;
   if(n==7){if(!managed_service_call(&bad,&c,&q,&r) || r.words[3]!=polaris_stream_golden[0])goto fail;}
   else if(n==6)q.words[1]=14;
   else {q.words[0]=REGISTER_PROBE_STREAM_DATA;q.words[1]=96;q.words[2]=0;}
  }else if(n==2){q.words[0]=REGISTER_PROBE_STREAM_DATA;q.words[1]=0;q.words[2]=0;if(!managed_service_call(&bad,&c,&q,&r))goto fail;}
  else if(n==3){q.words[0]=REGISTER_PROBE_STREAM_DATA;q.words[1]=0;q.words[2]=0x100000000ULL;}
  else if(n==1){q.words[0]=REGISTER_PROBE_TIMING_BEGIN;q.words[1]=2;}
  else if(n==0){q.words[0]=REGISTER_PROBE_STREAM_QUERY;q.words[1]=0;q.words[2]=1;}
  /* n5 replays BEGIN; n2 replays DATA; n7 replays QUERY. */
  if(managed_service_call(&bad,&c,&q,&r) || !stop())goto fail;
 }
 terminal_writeln("  ACTUAL DP STREAM ORDER-ROLE-VERSION-WIDE-DUPLICATE REFUSALS / PROCESS CLOSED: PASS");
 if(pages!=pmm_stats().free_pages || processes!=process_object_count() || spaces!=address_space_object_count() || endpoints!=endpoint_object_count() || threads!=thread_object_count() || queues!=process_exit_queue_object_count() || caps!=capability_table_count(process_capabilities(process_kernel())))return false;
 terminal_writeln("  ACTUAL DP STREAM / FULL CPU RESOURCE BASELINE / NO GPU IO: PASS");return true;
fail:(void)stop();return false;
}
