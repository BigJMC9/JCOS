#include "polaris_driver.h"
#include "lib.h"
#include "ipc.h"
#include "timer.h"
#include "system_console.h"
static bool call(PolarisDriver *d,u32 op,u64 a,u64 b,u64 c,IpcMessage *r) {
    IpcMessage q; k_memset(&q,0,sizeof(q)); k_memset(r,0,sizeof(*r));
    q.word_count=4; q.words[0]=POLARIS_NATIVE_HEADER(op); q.words[1]=a;q.words[2]=b;q.words[3]=c;
    return managed_service_call(&d->service,&d->connection,&q,r) && r->word_count==4 &&
        r->words[0]==POLARIS_NATIVE_REPLY && r->words[1]==d->connection.incarnation;
}
bool polaris_driver_reserve_profile(PolarisDriver *d,u32 role,const PolarisHardwareProfile *profile,const ProgramBorrowedMappingSpec *m) {
    if (!d || !m || !polaris_profile_valid(profile) || (role!=1 && role!=2) || (m->flags && m->flags!=(vm_flags_t)VM_UNCACHED) ||
        m->virtual_base!=POLARIS_NATIVE_FB_VA || m->size!=profile->surface.bytes ||
        (m->physical_base&0xFFF) || managed_service_state(&d->service)!=MANAGED_SERVICE_STOPPED) return false;
    if(!polaris_surface_runtime_init(profile,m->physical_base,m->size,&d->runtime))return false;
    ManagedServiceSpec s; s.path=role==1 ? "/bin/polarisnative.elf" : "/bin/polarisrescue.elf";
    s.shutdown_message=POLARIS_NATIVE_HEADER(POLARIS_NATIVE_STOP);s.shutdown_reply=POLARIS_NATIVE_REPLY;
    ManagedServiceLaunchExtras e; k_memset(&e,0,sizeof(e));
    e.borrowed_mapping=*m; e.startup_argument_count=2;e.startup_arguments[0]=POLARIS_NATIVE_FB_VA;
    e.startup_arguments[1]=d->runtime.gpu_address;e.shutdown_request_word_count=4;e.shutdown_request_words[0]=s.shutdown_message;
    d->role=role;d->process_id=0;d->program_attempted=false;d->retained_only=false;d->vtotal_attempted=false;
    if (!managed_service_start_ex(&d->service,&s,&e)) return false;
    d->process_id=managed_service_process_id(&d->service);
    return d->process_id && managed_service_connect_kernel(&d->service,&d->connection) &&
        polaris_driver_ping(d,0);
}
bool polaris_driver_reserve(PolarisDriver *d,u32 role,const ProgramBorrowedMappingSpec *m){
    return polaris_driver_reserve_profile(d,role,polaris_profile_qualified(),m);
}
bool polaris_driver_attach_retained(PolarisDriver *d,u64 output) {
    if (!d || !output || d->role!=POLARIS_NATIVE_ROLE_PRIMARY || d->program_attempted ||
        !d->process_id || managed_service_process_id(&d->service)!=d->process_id ||
        !managed_service_idle(&d->service)) return false;
    d->program_attempted=true;d->retained_only=true;
    IpcMessage r;
    return program_borrowed_set_writable(&d->service.program,d->process_id,false) &&
        call(d,POLARIS_NATIVE_RETAINED,output,0,0,&r) &&
        r.words[2]==POLARIS_NATIVE_RETAINED && r.words[3]==output;
}
bool polaris_driver_plan(PolarisDriver *d,u64 id,const PolarisPlaneWitness *w,PolarisProgramPlan *p) {
    if (!p) return false;
    k_memset(p,0,sizeof(*p));
    if (!d || !polaris_surface_runtime_valid(&d->runtime) || d->retained_only || !id || !w || polaris_plane_blockers(w) || w->values[0][0] || w->values[1][0]) return false;
    IpcMessage r;
    if (!call(d,POLARIS_NATIVE_CONFIG,id,(u64)w->values[0][2]|((u64)w->values[1][2]<<32),w->values[0][1],&r) ||
        r.words[2]!=id || r.words[3]) return false;
    p->incarnation=d->connection.incarnation;p->observation_id=id;p->gpu_address=d->runtime.gpu_address;
    p->pipe=d->runtime.pipe;p->crtc=d->runtime.crtc_control;p->witness=*w;
    PolarisPlanePreview preview;
    if (!polaris_plane_preview_init(&preview,p->incarnation,id,p->gpu_address,p->pipe,w)) return false;
    for (u32 i=0;i<5;++i) {
        if (!call(d,POLARIS_NATIVE_STEP,id,i,0,&r) ||
            !polaris_plane_preview_accept(&preview,r.words[1],id,r.words[2],r.words[3])) return false;
        p->tags[i]=r.words[2];p->values[i]=r.words[3];
    }
    return preview.closed && preview.next==5;
}
bool polaris_driver_draw(PolarisDriver *d) {
    IpcMessage r;
    return d && !d->retained_only && call(d,POLARIS_NATIVE_DRAW,d->role,0,0,&r) && r.words[2]==d->role && !r.words[3];
}
bool polaris_driver_stop(PolarisDriver *d) {
    if (!d) return false;
    if (managed_service_state(&d->service)==MANAGED_SERVICE_STOPPED) return !program_instance_needs_cleanup(&d->service.program);
    ManagedServiceStopResult r=managed_service_stop_bounded(&d->service);
    return (r==MANAGED_SERVICE_STOP_GRACEFUL || r==MANAGED_SERVICE_STOP_FORCED) &&
        !program_instance_needs_cleanup(&d->service.program);
}

bool polaris_driver_ping(PolarisDriver *d,u64 challenge) {
    IpcMessage r;
    return d && call(d,POLARIS_NATIVE_PING,challenge,0,0,&r) &&
        r.words[2]==d->role && r.words[3]==challenge;
}
bool polaris_driver_diagnostic(PolarisDriver *d,bool spin) {
    if (!d || !timer_initialized() || !timer_frequency() ||
        !d->process_id || managed_service_process_id(&d->service)!=d->process_id) return false;
    IpcMessage q,r;k_memset(&q,0,sizeof(q));k_memset(&r,0,sizeof(r));
    u64 op=spin ? POLARIS_NATIVE_TEST_SPIN : POLARIS_NATIVE_TEST_FAULT;
    q.word_count=4;q.words[0]=POLARIS_NATIVE_HEADER(op);q.words[1]=d->role;
    return managed_service_send_oneway(&d->service,&d->connection,&q) &&
        ipc_receive_blocking_for(process_kernel(),d->connection.receive_handle,&r,(u64)timer_frequency()*2ULL) &&
        r.word_count==4 && r.words[0]==POLARIS_NATIVE_REPLY && r.words[1]==d->connection.incarnation &&
        r.words[2]==op && r.words[3]==d->role;
}
bool polaris_driver_console_progress(PolarisDriver *d,bool busy,PolarisDriverProgress *out) {
    if (!out) return false;
    k_memset(out,0,sizeof(*out));
    if (!d || !system_console_running() || (busy &&
        (managed_service_process_id(&d->service)!=d->process_id || managed_service_idle(&d->service)))) return false;
    out->console_process_id=system_console_process_id();out->console_incarnation=system_console_incarnation();
    out->writes_before=system_console_portal_write_count();
    bool done=system_console_putchar('.');
    out->writes_after=system_console_portal_write_count();
    return done && out->console_process_id && out->console_incarnation &&
        system_console_running() && system_console_process_id()==out->console_process_id &&
        system_console_incarnation()==out->console_incarnation && out->writes_after>out->writes_before &&
        (!busy || (managed_service_process_id(&d->service)==d->process_id && !managed_service_idle(&d->service)));
}
