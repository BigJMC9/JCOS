#include "polaris_link_test.h"
#include "polaris_vertical_test.h"
#include "polaris_link_fixture.h"
#include "polaris_timing_fixture.h"
#include "polaris_timing_actual_fixture.h"
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
static ManagedService malformed;
static bool stop_bad(void){
 if(managed_service_state(&malformed)==MANAGED_SERVICE_STOPPED)return !program_instance_needs_cleanup(&malformed.program);
 ManagedServiceStopResult r=managed_service_stop_bounded(&malformed);
 return (r==MANAGED_SERVICE_STOP_GRACEFUL || r==MANAGED_SERVICE_STOP_FORCED) && !program_instance_needs_cleanup(&malformed.program);
}
bool polaris_link_test(void){
 if(!stop_bad())return false;
 u64 pages=pmm_stats().free_pages;u32 processes=process_object_count(),spaces=address_space_object_count(),endpoints=endpoint_object_count(),threads=thread_object_count(),queues=process_exit_queue_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
 u32 values[POLARIS_LINK_VALUES];u64 words[POLARIS_LINK_WORDS];polaris_link_fixture(values);
 const u64 expected[4]={0x0001040200010000ULL,0x0000010500000010ULL,0x0100000250000200ULL,0x0000003400010001ULL};
 if(!polaris_link_decode_copy(values,words))return false;
 for(u32 f=0;f<4;++f)if(words[f]!=expected[f])return false;
 terminal_writeln("  ACTUAL RING3 COPIED LINK / UNIQUE CRTC-FE-BE / GOLDEN REPORT: PASS");
 for(u32 n=0;n<2;++n){
  polaris_link_fixture(values);values[15]=n ? ~0U:values[15]^1U;k_memset(words,0xA5,sizeof(words));
  if(!polaris_link_decode_copy(values,words) || words[0]!=(n ? POLARIS_LINK_ALL_ONES:POLARIS_LINK_CHANGED))return false;
  for(u32 f=1;f<4;++f)if(words[f])return false;
 }
 terminal_writeln("  ACTUAL RING3 LINK CHANGED/ALL-ONES / CLEARED REPORT: PASS");
 ManagedServiceSpec spec;spec.path="/bin/registerprobe.elf";spec.shutdown_message=REGISTER_PROBE_SHUTDOWN;spec.shutdown_reply=REGISTER_PROBE_STOPPED;ManagedServiceConnection c;
 if(!managed_service_start(&malformed,&spec) || !managed_service_connect_kernel(&malformed,&c))goto fail;
 IpcMessage q,r;k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=REGISTER_PROBE_LINK_BEGIN;q.words[1]=POLARIS_LINK_VERSION;
 if(!managed_service_call(&malformed,&c,&q,&r) || r.words[2]!=1 || r.words[3]!=POLARIS_LINK_VALUES)goto fail;
 q.words[0]=REGISTER_PROBE_LINK_DATA;q.words[1]=0;q.words[2]=0;
 if(!managed_service_call(&malformed,&c,&q,&r) || r.words[2]!=1)goto fail;
 if(managed_service_call(&malformed,&c,&q,&r) || !stop_bad())goto fail;
 terminal_writeln("  ACTUAL LINK DUPLICATE INDEX / PROCESS CLOSED / NO REPLAY: PASS");

 u32 timing[POLARIS_TIMING_VALUES];u64 timing_words[POLARIS_TIMING_WORDS];polaris_timing_fixture(timing);
 const u64 timing_expected[9]={0x0002020000020000ULL,0x0128052804000540ULL,0x0023032303000326ULL,0x0006000000880000ULL,0x0000000000000101ULL,0x0000010001000000ULL,0x0000335B00008000ULL,0x0000005000000001ULL,0x0000335B0000335BULL};
 if(!polaris_timing_decode_copy(timing,timing_words))return false;
 for(u32 f=0;f<POLARIS_TIMING_WORDS;++f)if(timing_words[f]!=timing_expected[f])return false;
 terminal_writeln("  ACTUAL RING3 TIMING / 1024X768 GEOMETRY / RAW DP / GOLDEN REPORT: PASS");
 polaris_timing_fixture(timing);timing[27]=timing[61]=2199;timing[28]=timing[62]=(192U<<16)|2112;timing[29]=timing[63]=44U<<16;
 timing[31]=timing[65]=1124;timing[32]=timing[66]=(41U<<16)|1121;timing[33]=timing[67]=5U<<16;
 if(!polaris_timing_decode_copy(timing,timing_words) || ((timing_words[1]>>16)&65535)!=1920 || ((timing_words[2]>>16)&65535)!=1080)return false;
 terminal_writeln("  ACTUAL RING3 OBSERVED RASTER / GOP SIZE INDEPENDENT / NO MODE AUTHORITY: PASS");
 for(u32 n=0;n<3;++n){
  polaris_timing_fixture(timing);
  if(n==0)timing[33]^=1U;
  if(n==1)timing[27]=~0U;
  if(n==2)timing[27]=timing[61]=0;
  u32 expected_status=n==0 ? POLARIS_TIMING_CHANGED:n==1 ? POLARIS_TIMING_ALL_ONES:POLARIS_TIMING_GEOMETRY;
  if(!polaris_timing_decode_copy(timing,timing_words) || timing_words[0]!=expected_status)return false;
  for(u32 f=1;f<POLARIS_TIMING_WORDS;++f)if(timing_words[f])return false;
 }
 terminal_writeln("  ACTUAL RING3 TIMING REFUSAL / CHANGED-ALLONES-GEOMETRY / CLEARED REPORT: PASS");

 const u64 actual_expected[9]={0x0002020000020000ULL,0x00C0084007800898ULL,0x0029046104380465ULL,0x00050000002C0000ULL,0x0000000000000000ULL,0x0000010002000000ULL,0x0000758D00008000ULL,0x0000005000010201ULL,0x0000758B0000758DULL};
 polaris_timing_actual_fixture(timing);
 if(!polaris_timing_decode_copy(timing,timing_words))return false;
 for(u32 f=0;f<POLARIS_TIMING_WORDS;++f)if(timing_words[f]!=actual_expected[f])return false;
 terminal_writeln("  ACTUAL RING3 V36 REGISTER REPLAY / AUTO M PAIR / NINE V2 WORDS: PASS");
 for(u32 n=0;n<4;++n){
  polaris_timing_actual_fixture(timing);
  if(n==0)timing[37]=timing[71]=0;
  if(n==1)timing[73]^=0x1000000U;
  if(n==2)timing[72]^=1U;
  if(n==3)timing[71]^=POLARIS_TIMING_AUTO_MN_MASK;
  if(!polaris_timing_decode_copy(timing,timing_words) || timing_words[0]!=POLARIS_TIMING_CHANGED)return false;
  for(u32 f=1;f<POLARIS_TIMING_WORDS;++f)if(timing_words[f])return false;
 }
 terminal_writeln("  ACTUAL RING3 MANUAL M / RESERVED M / N AND GEN CHANGES REFUSED: PASS");
 /* Early QUERY,cross-role BEGIN,duplicateDATA and wideDWORD close fresh instances. */
 for(u32 n=0;n<4;++n){
  if(!managed_service_start(&malformed,&spec) || !managed_service_connect_kernel(&malformed,&c))goto fail;
  k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=REGISTER_PROBE_TIMING_BEGIN;q.words[1]=POLARIS_TIMING_VERSION;
  if(!managed_service_call(&malformed,&c,&q,&r) || r.words[2]!=1 || r.words[3]!=POLARIS_TIMING_VALUES)goto fail;
  if(n==2){
   q.words[0]=REGISTER_PROBE_TIMING_DATA;q.words[1]=0;q.words[2]=0;
   if(!managed_service_call(&malformed,&c,&q,&r) || r.words[2]!=1)goto fail;
  }else if(n==3){q.words[0]=REGISTER_PROBE_TIMING_DATA;q.words[1]=0;q.words[2]=0x100000000ULL;}
  else {q.words[0]=n ? REGISTER_PROBE_LINK_BEGIN:REGISTER_PROBE_TIMING_QUERY;q.words[1]=n ? POLARIS_LINK_VERSION:0;q.words[2]=n ? 0:POLARIS_TIMING_VERSION;}
  if(managed_service_call(&malformed,&c,&q,&r) || !stop_bad())goto fail;
 }
 terminal_writeln("  ACTUAL TIMING QUERY-ROLE-DUPLICATE-WIDE REFUSAL / PROCESS CLOSED: PASS");
 if(!managed_service_start(&malformed,&spec) || !managed_service_connect_kernel(&malformed,&c))goto fail;
 k_memset(&q,0,sizeof(q));q.word_count=4;q.words[0]=REGISTER_PROBE_TIMING_BEGIN;q.words[1]=1;
 if(managed_service_call(&malformed,&c,&q,&r) || !stop_bad())goto fail;
 terminal_writeln("  ACTUAL TIMING V1 BEGIN REFUSED / FRESH V2 ROLE: PASS");
 if(!polaris_vertical_test())return false;
 if(pages!=pmm_stats().free_pages || processes!=process_object_count() || spaces!=address_space_object_count() || endpoints!=endpoint_object_count() || threads!=thread_object_count() || queues!=process_exit_queue_object_count() || caps!=capability_table_count(process_capabilities(process_kernel())))return false;
 terminal_writeln("  ACTUAL LINK PROBE / FULL CPU RESOURCE BASELINE / NO GPU IO: PASS");
 terminal_writeln("  ACTUAL TIMING PROBE / FULL CPU RESOURCE BASELINE / NO GPU IO: PASS");return true;
fail:(void)stop_bad();return false;
}