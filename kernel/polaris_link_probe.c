#include "polaris_link.h"
#include "managed_service.h"
#include "register_probe_protocol.h"
#include "lib.h"
#include "timer.h"
static ManagedService decoder;
static bool stop(void){
 if(managed_service_state(&decoder)==MANAGED_SERVICE_STOPPED)return !program_instance_needs_cleanup(&decoder.program);
 ManagedServiceStopResult r=managed_service_stop_bounded(&decoder);
 return (r==MANAGED_SERVICE_STOP_GRACEFUL || r==MANAGED_SERVICE_STOP_FORCED) && !program_instance_needs_cleanup(&decoder.program);
}
static bool call(ManagedServiceConnection *c,u64 op,u64 a,u64 v,u64 tag,u64 expected,u64 start,u64 duration,u64 *out){
 if(timer_ticks()-start>=duration)return false;
 IpcMessage q,r;k_memset(&q,0,sizeof(q));k_memset(&r,0,sizeof(r));q.word_count=4;
 q.words[0]=op;q.words[1]=a;q.words[2]=v;
 bool ok=managed_service_call(&decoder,c,&q,&r) && r.word_count==4 && r.words[0]==REGISTER_PROBE_REPLY && r.words[1]==c->incarnation && r.words[2]==tag &&
  (out || r.words[3]==expected) && timer_ticks()-start<duration;
 if(ok && out)*out=r.words[3];return ok;
}
static bool decode_copy(const u32 *values,u32 count,u64 *report,u32 words,u64 begin,u64 data,u64 query,u32 max_status,u64 version){
 if(!report)return false;k_memset(report,0,words*sizeof(*report));
 if(!values || !timer_initialized() || !timer_frequency() || managed_service_state(&decoder)!=MANAGED_SERVICE_STOPPED || program_instance_needs_cleanup(&decoder.program))return false;
 ManagedServiceSpec spec;spec.path="/bin/registerprobe.elf";spec.shutdown_message=REGISTER_PROBE_SHUTDOWN;spec.shutdown_reply=REGISTER_PROBE_STOPPED;
 if(!managed_service_start(&decoder,&spec)){(void)stop();return false;}
 ManagedServiceConnection c;bool ok=managed_service_connect_kernel(&decoder,&c);
 u64 start=timer_ticks(),duration=(u64)timer_frequency()*4ULL;
 if(ok)ok=call(&c,begin,version,0,1,count,start,duration,0);
 for(u32 i=0;ok && i<count;++i)ok=call(&c,data,i,values[i],i+1,values[i],start,duration,0);
 for(u32 i=0;ok && i<words;++i)ok=call(&c,query,i,version,i+1,0,start,duration,&report[i]);
 bool ended=stop();
 if(!ok || !ended){k_memset(report,0,words*sizeof(*report));return false;}
 u32 status=(u32)(report[0]&65535U);
 bool shape=status<=max_status && (status ? !(report[0]>>16):((report[0]>>16)&65535U)==version);
 if(status)for(u32 i=1;i<words;++i)if(report[i])shape=false;
 if(!shape){k_memset(report,0,words*sizeof(*report));return false;}
 return true;
}
bool polaris_link_decode_copy(const u32 values[POLARIS_LINK_VALUES],u64 report[POLARIS_LINK_WORDS]){
 return decode_copy(values,POLARIS_LINK_VALUES,report,POLARIS_LINK_WORDS,REGISTER_PROBE_LINK_BEGIN,REGISTER_PROBE_LINK_DATA,REGISTER_PROBE_LINK_QUERY,POLARIS_LINK_CRTC_UNSUPPORTED,POLARIS_LINK_VERSION);
}
bool polaris_timing_decode_copy(const u32 values[POLARIS_TIMING_VALUES],u64 report[POLARIS_TIMING_WORDS]){
 return decode_copy(values,POLARIS_TIMING_VALUES,report,POLARIS_TIMING_WORDS,REGISTER_PROBE_TIMING_BEGIN,REGISTER_PROBE_TIMING_DATA,REGISTER_PROBE_TIMING_QUERY,POLARIS_TIMING_GEOMETRY,POLARIS_TIMING_VERSION);
}

bool polaris_vertical_decode_copy(const u32 values[POLARIS_VERTICAL_VALUES],u64 report[POLARIS_VERTICAL_WORDS]){
 return decode_copy(values,POLARIS_VERTICAL_VALUES,report,POLARIS_VERTICAL_WORDS,REGISTER_PROBE_VERTICAL_BEGIN,REGISTER_PROBE_VERTICAL_DATA,REGISTER_PROBE_VERTICAL_QUERY,POLARIS_VERTICAL_CONTROLS,POLARIS_VERTICAL_VERSION);
}

bool polaris_stream_decode_copy(const u32 values[POLARIS_STREAM_VALUES],u64 report[POLARIS_STREAM_WORDS]){
 return decode_copy(values,POLARIS_STREAM_VALUES,report,POLARIS_STREAM_WORDS,REGISTER_PROBE_STREAM_BEGIN,REGISTER_PROBE_STREAM_DATA,REGISTER_PROBE_STREAM_QUERY,POLARIS_STREAM_ROUTE,POLARIS_STREAM_VERSION);
}
